// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// Project Headers
#include "../state.h"
#include "../write2usbUI.h"
#include "../write2usb.h"

/**
 * @brief Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
 *
 * Walks the Volume Recognition Sequence (sectors 16-23, 2048-byte spacing)
 * with pread, so the descriptor's offset is untouched. Accepts an ISO 9660
 * PVD (type 0x01, "CD001"), which covers UDF-bridge images, or a UDF
 * "NSR02"/"NSR03" descriptor, which covers pure-UDF images. "CD001" non-PVD
 * entries, "BEA01" and "TEA01" are skipped; any other identifier rejects the
 * image immediately.
 *
 * @param fd Open, readable descriptor for the image. Not closed by this
 *           function.
 * @return true if an accepted descriptor is found; false on a short read, an
 *         unrecognized identifier, or no match within 8 sectors.
 *
 * @note Acceptance does not imply the image is bootable. The name is
 *       historical; isValidOpticalImage would describe it more accurately.
 */
static bool isValidIso9660(int fd) {
    for (int i = 0; i < 8; ++i) {
        uint8_t d[7];
        if (pread(fd, d, sizeof(d), 32768 + i * 2048) != sizeof(d)) return false;

        std::string_view id(reinterpret_cast<char*>(d + 1), 5);
        if ((id == "CD001" && d[0] == 0x01) ||        // ISO 9660 PVD
            id == "NSR02" || id == "NSR03")           // UDF
            return true;
        if (id != "CD001" && id != "BEA01" && id != "TEA01")
            return false;                              // not a recognition sequence
    }
    return false;
}

namespace {

    using Clock = std::chrono::high_resolution_clock;

    constexpr size_t DESIRED_BUFFER  = 4 * 1024 * 1024;
    constexpr size_t MAX_WRITE_CHUNK = 4 * 1024 * 1024;
    constexpr int    NUM_BUFFERS     = 2;
    static_assert(MAX_WRITE_CHUNK <= DESIRED_BUFFER,
                  "write chunk larger than the buffer is pointless");

    // AlignedBuffer (write2usb.h) is non-movable, so the pool holds unique_ptrs.
    using IsoBufferPool = std::vector<std::unique_ptr<AlignedBuffer>>;

    bool markFailed(std::vector<ProgressInfo>& progressData, size_t index) {
        progressData[index].failed.store(true);
        return false;
    }

    // RAII file descriptor (closes on scope exit if valid).
    struct FdGuard {
        int fd;
        explicit FdGuard(int f) : fd(f) {}
        ~FdGuard() { if (fd != -1) close(fd); }
        FdGuard(const FdGuard&)            = delete;
        FdGuard& operator=(const FdGuard&) = delete;
    };

    // Effective O_DIRECT alignment: max(logical, physical) sector size.
    // BLKPBSZGET is best-effort and falls back to the logical size.
    bool queryEffectiveSectorSize(int devFd, int& sectorSize) {
        int logical  = 0;
        int physical = 0;

        if (ioctl(devFd, BLKSSZGET, &logical) < 0 || logical <= 0)
            return false;
        if (ioctl(devFd, BLKPBSZGET, &physical) < 0 || physical <= 0)
            physical = logical;

        sectorSize = std::max(logical, physical);
        return true;
    }

    bool allocateAlignedBuffers(IsoBufferPool& out, int sectorSize, size_t bufferSize) {
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            auto buf = std::make_unique<AlignedBuffer>(bufferSize, static_cast<size_t>(sectorSize));
            if (!buf->ok())
                return false;
            out.push_back(std::move(buf));
        }
        return true;
    }

    // Validates the ISO, rewinds, and fetches its stat info.
    bool prepareIso(int isoFd, struct stat& sb) {
        return isValidIso9660(isoFd)
            && lseek(isoFd, 0, SEEK_SET) != -1
            && fstat(isoFd, &sb) != -1;
    }

    // Exponentially smoothed write-speed estimator with stall handling.
    class SpeedEstimator {
    public:
        explicit SpeedEstimator(Clock::time_point start)
            : lastUpdate_(start), stallStart_(start) {}

        // Returns a speed (MB/s) to publish, or nullopt if nothing should change.
        std::optional<double> sample(uint64_t currentWritten, Clock::time_point now) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now - lastUpdate_).count();
            if (elapsedMs < MIN_UPDATE_INTERVAL_MS)
                return std::nullopt;

            const uint64_t delta = (currentWritten >= lastWritten_) ? (currentWritten - lastWritten_) : 0;
            return delta > 0 ? onProgress(currentWritten, delta, elapsedMs, now)
                             : onNoProgress(now);
        }

    private:
        static constexpr double ALPHA = 0.3;
        static constexpr int MIN_UPDATE_INTERVAL_MS = 1000;
        static constexpr int STALL_TIMEOUT_SECONDS  = 60;

        double onProgress(uint64_t currentWritten, uint64_t delta, long long elapsedMs,
                          Clock::time_point now) {
            stalled_ = false;

            const double instant = (static_cast<double>(delta) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
            // Heavier smoothing for slow drives (< 10 MB/s) to reduce display
            // jitter from bursty O_DIRECT writes at low throughput.
            const double factor = (instant < 10.0) ? 0.2 : ALPHA;

            smoothed_ = haveEstimate_ ? (factor * instant + (1.0 - factor) * smoothed_) : instant;
            haveEstimate_ = true;

            // Only reset the baseline when bytes actually arrived — this keeps
            // `elapsed` spanning back to the last real data point instead of an
            // arbitrary polling tick, which would truncate the window to ~1s and
            // produce a spurious floor (one write chunk per interval) on
            // slow/bursty O_DIRECT writers.
            lastWritten_ = currentWritten;
            lastUpdate_  = now;
            return smoothed_;
        }

        std::optional<double> onNoProgress(Clock::time_point now) {
            if (!stalled_) {
                stalled_    = true;
                stallStart_ = now;
            }
            const auto stallSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                          now - stallStart_).count();
            // Deliberately do NOT touch lastWritten_/lastUpdate_ here — let the
            // elapsed window keep growing while a slow write is still in flight,
            // so the eventual speed sample reflects the true transfer duration.
            if (stallSeconds >= STALL_TIMEOUT_SECONDS)
                return 0.0;
            return std::nullopt;
        }

        Clock::time_point lastUpdate_;
        Clock::time_point stallStart_;
        uint64_t lastWritten_  = 0;
        double   smoothed_     = 0.0;
        bool     haveEstimate_ = false;
        bool     stalled_      = false;
    };

    // Double-buffered reader-thread -> writer-thread ISO streamer with a progress
    // monitor thread.
    class IsoStreamer {
    public:
        IsoStreamer(int isoFd, int devFd, uint64_t fileSize, uint64_t paddedSize,
                    size_t bufferSize, size_t writeChunk,
                    IsoBufferPool& buffers,
                    std::vector<ProgressInfo>& progressData, size_t progressIndex)
            : isoFd_(isoFd), devFd_(devFd), fileSize_(fileSize), paddedSize_(paddedSize),
              bufferSize_(bufferSize), writeChunk_(writeChunk),
              progressData_(progressData), progressIndex_(progressIndex) {
            for (auto& b : buffers)
                emptyQ_.push_back({b->data, 0});
        }

        bool run() {
            startTime_ = Clock::now();

            try {
                monitorThread_ = std::thread([this] { monitorLoop(); });
                readerThread_  = std::thread([this] { readerLoop(); });
                pumpToDevice();
            } catch (...) {
                shutdownThreads();
                if (!isCancelled()) markFailed();
                return false;
            }

            shutdownThreads();

            if (readFailed_ || ioFailed_) {
                if (!isCancelled()) markFailed();
                return false;
            }

            posix_fadvise(isoFd_, 0, 0, POSIX_FADV_DONTNEED);

            if (!aborted() && fsync(devFd_) != 0) {
                markFailed();
                return false;
            }

            return finishIfComplete();
        }

    private:
        struct Chunk {
            char*  buf;
            size_t len;   // 0 == end-of-stream sentinel
        };

        static bool isCancelled() { return GlobalState::g_operationCancelled.load(); }
        bool aborted() const { return cancelled_ || isCancelled(); }

        bool markFailed() {
            return ::markFailed(progressData_, progressIndex_);
        }

        // ---- lifecycle -------------------------------------------------------

        // Idempotent: stops and joins both helper threads.
        void shutdownThreads() {
            {
                std::lock_guard<std::mutex> lk(qMutex_);
                stopReader_ = true;
            }
            qCv_.notify_all();
            if (readerThread_.joinable()) readerThread_.join();

            {
                std::lock_guard<std::mutex> lk(monMutex_);
                monitoringActive_.store(false);
            }
            monCv_.notify_all();
            if (monitorThread_.joinable()) monitorThread_.join();
        }

        bool finishIfComplete() {
            if (aborted() || localBytesWritten_ < paddedSize_)
                return false;

            const auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          Clock::now() - startTime_);
            const double seconds  = totalElapsed.count() / 1000.0;
            const double avgSpeed = seconds > 0.0
                ? (static_cast<double>(fileSize_) / (1024.0 * 1024.0)) / seconds
                : 0.0;

            auto& info = progressData_[progressIndex_];
            info.speed.store(avgSpeed);
            info.progress.store(100);
            info.completed.store(true);
            return true;
        }

        // ---- progress monitor ------------------------------------------------

        // Interruptible 500 ms tick: wakes immediately on shutdown instead of
        // making the caller wait out a sleep. Returns false once shutdown was
        // requested.
        bool waitForTick() {
            {
                std::unique_lock<std::mutex> lk(monMutex_);
                monCv_.wait_for(lk, std::chrono::milliseconds(500),
                                [this] { return !monitoringActive_.load(); });
            }
            return monitoringActive_.load();
        }

        void publishProgress(uint64_t currentWritten) {
            auto& info = progressData_[progressIndex_];
            info.bytesWritten.store(currentWritten);
            info.progress.store(static_cast<int>(
                std::min(99.0, (static_cast<double>(currentWritten) / fileSize_) * 100.0)));
        }

        void monitorLoop() {
            SpeedEstimator estimator(Clock::now());

            while (monitoringActive_.load()) {
                if (!waitForTick()) break;

                const auto now = Clock::now();
                const uint64_t currentWritten = directBytesWritten_.load();

                publishProgress(currentWritten);

                if (auto speed = estimator.sample(currentWritten, now))
                    progressData_[progressIndex_].speed.store(*speed);
            }
        }

        // ---- reader thread ---------------------------------------------------

        void pushSentinel(bool failed) {
            {
                std::lock_guard<std::mutex> lk(qMutex_);
                if (failed) readFailed_ = true;
                fullQ_.push_back({nullptr, 0});
            }
            qCv_.notify_all();
        }

        void pushFull(const Chunk& c) {
            {
                std::lock_guard<std::mutex> lk(qMutex_);
                fullQ_.push_back(c);
            }
            qCv_.notify_all();
        }

        // Returns false if the reader was told to stop.
        bool acquireEmptyBuffer(Chunk& out) {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [this] { return stopReader_ || !emptyQ_.empty(); });
            if (stopReader_) return false;
            out = emptyQ_.front();
            emptyQ_.pop_front();
            return true;
        }

        // Reads until `toRead` bytes are gathered, EOF, or error (false on error).
        // Short reads mid-file are not treated as EOF.
        bool readFully(char* buf, size_t toRead, size_t& got, bool& reachedEof) {
            got = 0;
            reachedEof = false;
            while (got < toRead) {
                ssize_t r = read(isoFd_, buf + got, toRead - got);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                if (r == 0) {
                    reachedEof = true;
                    break;
                }
                got += static_cast<size_t>(r);
            }
            return true;
        }

        void readerLoop() {
            uint64_t readTotal = 0;

            while (readTotal < paddedSize_ && !isCancelled()) {
                Chunk c;
                if (!acquireEmptyBuffer(c)) return;

                const size_t toRead = static_cast<size_t>(
                    std::min<uint64_t>(bufferSize_, paddedSize_ - readTotal));

                size_t got = 0;
                bool reachedEof = false;

                if (!readFully(c.buf, toRead, got, reachedEof)) {
                    pushSentinel(true);
                    return;
                }

                // A source shorter than fstat reported (truncated file) must
                // fail rather than be zero-padded silently.
                if (reachedEof && readTotal + got < fileSize_) {
                    pushSentinel(true);
                    return;
                }

                if (got < toRead)
                    std::memset(c.buf + got, 0, toRead - got);

                c.len = toRead;
                readTotal += toRead;
                pushFull(c);

                if (reachedEof) break;
            }

            pushSentinel(false);
        }

        // ---- writer (calling thread) -----------------------------------------

        // Blocks for the next full chunk. Returns false on the end sentinel
        // (reader finished, failed, or was cancelled).
        bool popFull(Chunk& out) {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [this] { return !fullQ_.empty(); });
            out = fullQ_.front();
            fullQ_.pop_front();
            return out.len != 0;
        }

        void recycle(char* buf) {
            {
                std::lock_guard<std::mutex> lk(qMutex_);
                emptyQ_.push_back({buf, 0});
            }
            qCv_.notify_all();
        }

        void recordWritten(size_t written) {
            // Cap reported progress at the true file size to hide padding.
            const uint64_t reportable = std::min<uint64_t>(
                static_cast<uint64_t>(written),
                fileSize_ - std::min(localBytesWritten_, fileSize_));

            localBytesWritten_ += static_cast<uint64_t>(written);
            directBytesWritten_.fetch_add(reportable);
        }

        // Write loop — handles partial writes and EINTR. Each write() is capped at
        // writeChunk_ (a multiple of sectorSize), so buffer address, length and
        // device offset all stay sector-aligned for O_DIRECT while progress is
        // reported (and cancellation checked) after every write of <= 4 MiB.
        // Returns false on I/O failure or cancellation (ioFailed_/cancelled_ say
        // which).
        bool writeBuffer(const Chunk& c) {
            size_t remaining = c.len;
            char*  ptr       = c.buf;

            while (remaining > 0) {
                const size_t thisWrite = std::min(remaining, writeChunk_);
                ssize_t written = write(devFd_, ptr, thisWrite);

                if (written < 0) {
                    if (errno == EINTR) continue;
                    ioFailed_ = true;
                    return false;
                }
                if (written == 0) {   // no progress; avoid infinite spin
                    ioFailed_ = true;
                    return false;
                }
                if (isCancelled()) {
                    cancelled_ = true;
                    return false;
                }

                remaining -= static_cast<size_t>(written);
                ptr       += written;
                recordWritten(static_cast<size_t>(written));
            }
            return true;
        }

        void pumpToDevice() {
            Chunk c;
            while (popFull(c)) {
                if (isCancelled()) {
                    cancelled_ = true;
                    break;
                }
                if (!writeBuffer(c)) break;
                recycle(c.buf);
            }
        }

        // ---- state -----------------------------------------------------------

        const int      isoFd_;
        const int      devFd_;
        const uint64_t fileSize_;
        const uint64_t paddedSize_;
        const size_t   bufferSize_;
        const size_t   writeChunk_;

        std::vector<ProgressInfo>& progressData_;
        const size_t               progressIndex_;

        std::mutex              qMutex_;
        std::condition_variable qCv_;
        std::deque<Chunk>       emptyQ_;
        std::deque<Chunk>       fullQ_;
        bool                    stopReader_ = false;
        bool                    readFailed_ = false;

        std::atomic<uint64_t>   directBytesWritten_{0};
        std::atomic<bool>       monitoringActive_{true};
        std::mutex              monMutex_;
        std::condition_variable monCv_;

        std::thread       readerThread_;
        std::thread       monitorThread_;
        Clock::time_point startTime_;

        uint64_t localBytesWritten_ = 0;
        bool     cancelled_         = false;
        bool     ioFailed_          = false;
    };

} // namespace

/**
 * @brief Writes an ISO image to a raw block device, auto-routing Windows vs.
 *        Linux configurations.
 *
 * High-level orchestration routine that opens the ISO once, validates it,
 * then intercepts Windows installation media using an optimized metadata
 * signature probe. Standard/Linux distributions fall through directly into a
 * specialized, unbuffered block-level stream.
 *
 * @par Single fd lifecycle
 * The ISO is opened once at entry via open(O_RDONLY) and held for the
 * duration of the call. The same descriptor is passed to isValidIso9660 and
 * isWindowsIsoInitialCheck, avoiding redundant opens. File size is retrieved
 * once via fstat and reused across all paths.
 *
 * @par Early validation
 * isValidIso9660 is invoked before any device access or routing decision,
 * ensuring invalid images are rejected with minimal overhead.
 *
 * @par Windows path routing
 * If isWindowsIsoInitialCheck passes, execution is delegated to
 * writeWindowsIsoToDevice, which automatically handles multi-partitioning,
 * hybrid FAT32+NTFS tables, and cluster tuning.
 *
 * @par Raw O_DIRECT pipeline
 * Non-Windows targets are imaged via raw unbuffered disk I/O. Data transfers
 * use two 4 MiB buffers allocated with posix_memalign (8 MiB per call), each
 * rounded down to a multiple of the device's effective sector size and
 * aligned to it. That size is derived by querying both the logical sector
 * size (ioctl(BLKSSZGET)) and the physical sector size (ioctl(BLKPBSZGET))
 * and taking the larger of the two. This ensures correct O_DIRECT alignment
 * on 512n, 512e, and 4Kn drives. BLKPBSZGET is treated as best-effort and
 * falls back to the logical size if the kernel or device does not support it.
 * POSIX_FADV_SEQUENTIAL is applied to the source descriptor before the write
 * loop to enable aggressive kernel read-ahead; POSIX_FADV_DONTNEED is applied
 * after to release the ISO from page cache once writing completes.
 *
 * @par Double buffering
 * A dedicated reader thread fills one aligned buffer from the ISO (including
 * tail zero-padding) while the calling thread writes the other buffer to the
 * device. The two buffers circulate between an "empty" and a "full" queue, so
 * the device is not left idle while the next chunk is being read. The reader
 * can run at most one buffer ahead of the writer, which bounds memory use per
 * call.
 *
 * @par Chunked device writes
 * Each device write() is sector-aligned and capped at 4 MiB. Buffer address,
 * length, and device offset all remain aligned for O_DIRECT. With the current
 * 4 MiB buffers each buffer goes out as a single write; the cap keeps
 * progress and cancellation granularity fine (important for slow USB drives,
 * < 10 MB/s) if the buffer size is increased later.
 *
 * @par Progress monitoring
 * A separate thread samples the written-byte counter every 500 ms, publishes
 * progress (capped at 99% until completion), and computes an exponentially
 * smoothed speed (heavier smoothing below 10 MB/s). The last known speed is
 * held through stalls and reset to 0 after 60 s without progress. The monitor
 * sleeps on a condition variable, so it wakes immediately on shutdown.
 *
 * @par Tail-block padding
 * Detects partial blocks at EOF, automatically zero-padding the remaining
 * buffer slice up to the strict sector layout boundary to prevent kernel
 * rejection errors (EINVAL). Short read() results mid-file are not treated as
 * EOF; the reader keeps reading until the chunk is full or read() returns 0.
 *
 * @par Asynchronous cancellation safety
 * Evaluates GlobalState::g_operationCancelled in both the reader and the
 * writer. On user abort, short-circuits execution, stops the reader thread,
 * and leaves the disk safely without triggering a cascading fsync block.
 *
 * @param isoPath       Absolute path to the source ISO image file on the host.
 * @param device        Target destination block node path (e.g., /dev/sdb).
 *                      WARNING: All existing underlying data will be
 *                      destructively overwritten.
 * @param progressIndex Unique thread tracking identifier mapped inside the
 *                      global progress array.
 * @param progressData  Reference to the operation-local progress data array
 *                      used for live progress, speed, and completion
 *                      reporting.
 * @return true if the image data was written in full and flushed to the
 *         device with fsync; false on I/O failure, validation failure, or
 *         user cancellation. The written data is not read back or verified
 *         against the ISO.
 *
 * @see isValidIso9660
 * @see isWindowsIsoInitialCheck
 * @see writeWindowsIsoToDevice
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex, std::vector<ProgressInfo>& progressData) {

    FdGuard iso(open(isoPath.c_str(), O_RDONLY));
    if (iso.fd == -1) return markFailed(progressData, progressIndex);

    struct stat sb;
    if (!prepareIso(iso.fd, sb)) return markFailed(progressData, progressIndex);

    if (isWindowsIsoInitialCheck(iso.fd, sb.st_size))
        return writeWindowsIsoToDevice(isoPath, device, progressIndex, progressData);

    if (lseek(iso.fd, 0, SEEK_SET) == -1) return markFailed(progressData, progressIndex);
    posix_fadvise(iso.fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    FdGuard dev(open(device.c_str(), O_WRONLY | O_DIRECT));
    if (dev.fd == -1) return markFailed(progressData, progressIndex);

    int sectorSize = 0;
    if (!queryEffectiveSectorSize(dev.fd, sectorSize))
        return markFailed(progressData, progressIndex);

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) return markFailed(progressData, progressIndex);

    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Device writes are capped at 4 MiB and kept sector-aligned so the
    // progress byte counter advances at fine granularity. With the current
    // 4 MiB buffers each buffer is one write(); the cap matters if
    // DESIRED_BUFFER is raised, because a single very large write() on a slow
    // USB drive (< 10 MB/s) blocks for many seconds and makes the monitor's
    // speed samples noisy. Must stay a multiple of sectorSize for O_DIRECT.
    const size_t writeChunk = std::max<size_t>((MAX_WRITE_CHUNK / sectorSize) * sectorSize,
                                               static_cast<size_t>(sectorSize));

    IsoBufferPool buffers;
    if (!allocateAlignedBuffers(buffers, sectorSize, bufferSize))
        return markFailed(progressData, progressIndex);

    IsoStreamer streamer(iso.fd, dev.fd, fileSize, paddedSize, bufferSize, writeChunk,
                         buffers, progressData, progressIndex);
    return streamer.run();
}
