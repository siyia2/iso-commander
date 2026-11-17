// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
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
bool isValidIso9660(int fd) {
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

    int iso_fd = open(isoPath.c_str(), O_RDONLY);
    if (iso_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeIso = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeIso)> isoGuard(&iso_fd, closeIso);

    if (!isValidIso9660(iso_fd)) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    struct stat sb;
    if (fstat(iso_fd, &sb) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    if (isWindowsIsoInitialCheck(iso_fd, sb.st_size)) {
        return writeWindowsIsoToDevice(isoPath, device, progressIndex, progressData);
    }

    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    int dev_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (dev_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<int, decltype(closeIso)> devGuard(&dev_fd, closeIso);

    int logicalSectorSize  = 0;
    int physicalSectorSize = 0;

    if (ioctl(dev_fd, BLKSSZGET, &logicalSectorSize) < 0 || logicalSectorSize <= 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    // BLKPBSZGET is best-effort; fall back to logical size if unavailable.
    if (ioctl(dev_fd, BLKPBSZGET, &physicalSectorSize) < 0 || physicalSectorSize <= 0)
        physicalSectorSize = logicalSectorSize;

    const int sectorSize = std::max(logicalSectorSize, physicalSectorSize);

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    constexpr size_t DESIRED_BUFFER = 4 * 1024 * 1024;
    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Device writes are capped at 4 MiB and kept sector-aligned so the
    // progress byte counter advances at fine granularity. With the current
    // 4 MiB buffers each buffer is one write(); the cap matters if
    // DESIRED_BUFFER is raised, because a single very large write() on a slow
    // USB drive (< 10 MB/s) blocks for many seconds and makes the monitor's
    // speed samples noisy. Must stay a multiple of sectorSize for O_DIRECT.
    constexpr size_t MAX_WRITE_CHUNK = 4 * 1024 * 1024;
    static_assert(MAX_WRITE_CHUNK <= DESIRED_BUFFER,
                  "write chunk larger than the buffer is pointless");
    const size_t writeChunk = std::max<size_t>((MAX_WRITE_CHUNK / sectorSize) * sectorSize,
                                               static_cast<size_t>(sectorSize));

    constexpr int NUM_BUFFERS = 2;
    std::unique_ptr<char, decltype(&free)> bufferGuards[NUM_BUFFERS] = {
        {nullptr, &free}, {nullptr, &free}
    };
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        char* p = nullptr;
        if (posix_memalign(reinterpret_cast<void**>(&p), sectorSize, bufferSize) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
        bufferGuards[i].reset(p);
    }

    struct Chunk {
        char*  buf;
        size_t len;   // 0 == end-of-stream sentinel
    };

    std::mutex              qMutex;
    std::condition_variable qCv;
    std::deque<Chunk>       emptyQ;
    std::deque<Chunk>       fullQ;
    bool                    stopReader = false;
    bool                    readFailed = false;

    for (int i = 0; i < NUM_BUFFERS; ++i)
        emptyQ.push_back({bufferGuards[i].get(), 0});

    std::atomic<uint64_t> directBytesWrittenAccumulator{0};
    std::atomic<bool> monitoringActive{true};
    std::mutex              monMutex;
    std::condition_variable monCv;

    auto startTime = std::chrono::high_resolution_clock::now();

    std::thread progressMonitorThread;
    std::thread readerThread;

    // Idempotent: stops and joins both helper threads.
    auto shutdownThreads = [&]() {
        {
            std::lock_guard<std::mutex> lk(qMutex);
            stopReader = true;
        }
        qCv.notify_all();
        if (readerThread.joinable()) readerThread.join();

        {
            std::lock_guard<std::mutex> lk(monMutex);
            monitoringActive.store(false);
        }
        monCv.notify_all();
        if (progressMonitorThread.joinable()) progressMonitorThread.join();
    };

    uint64_t localBytesWritten = 0;
    bool     cancelled = false;
    bool     ioFailed  = false;

    try {
        progressMonitorThread = std::thread([&, fileSize, progressIndex]() {
            auto     lastUpdate    = std::chrono::high_resolution_clock::now();
            uint64_t lastWritten   = 0;
            double   smoothedSpeed = 0.0;
            bool     haveEstimate  = false;

            auto     stallStart    = std::chrono::high_resolution_clock::now();
            bool     stalled       = false;

            constexpr double alpha = 0.3;
            constexpr int MIN_UPDATE_INTERVAL_MS = 1000;
            constexpr int STALL_TIMEOUT_SECONDS = 60;

            while (monitoringActive.load()) {
                {
                    // Interruptible 500 ms tick: wakes immediately on
                    // shutdown instead of making the caller wait out a sleep.
                    std::unique_lock<std::mutex> lk(monMutex);
                    monCv.wait_for(lk, std::chrono::milliseconds(500),
                                   [&] { return !monitoringActive.load(); });
                }
                if (!monitoringActive.load()) break;

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();

                uint64_t currentWritten = directBytesWrittenAccumulator.load();

                progressData[progressIndex].bytesWritten.store(currentWritten);
                progressData[progressIndex].progress.store(static_cast<int>(
                    std::min(99.0, (static_cast<double>(currentWritten) / fileSize) * 100.0)));

                uint64_t deltaBytes = (currentWritten >= lastWritten) ? (currentWritten - lastWritten) : 0;

                if (deltaBytes > 0 && elapsed >= MIN_UPDATE_INTERVAL_MS) {
                    stalled = false;

                    double instantSpeed = (static_cast<double>(deltaBytes) / (1024.0 * 1024.0)) / (elapsed / 1000.0);

                    // For slow drives (< 10 MB/s), apply heavier smoothing to
                    // reduce display jitter from bursty O_DIRECT writes at low
                    // throughput.
                    double smoothingFactor = (instantSpeed < 10.0) ? 0.2 : alpha;

                    smoothedSpeed = haveEstimate
                        ? (smoothingFactor * instantSpeed + (1.0 - smoothingFactor) * smoothedSpeed)
                        : instantSpeed;
                    haveEstimate = true;

                    progressData[progressIndex].speed.store(smoothedSpeed);

                    // Only reset the baseline when bytes actually arrived —
                    // this keeps `elapsed` spanning back to the last real data
                    // point instead of an arbitrary polling tick, which was
                    // previously truncating the window to ~1s and producing a
                    // spurious floor (one write chunk per interval) on
                    // slow/bursty O_DIRECT writers.
                    lastWritten = currentWritten;
                    lastUpdate  = now;
                } else if (deltaBytes == 0 && elapsed >= MIN_UPDATE_INTERVAL_MS) {
                    if (!stalled) {
                        stalled = true;
                        stallStart = now;
                    }

                    auto stallDuration = std::chrono::duration_cast<std::chrono::seconds>(
                                            now - stallStart).count();

                    if (stallDuration >= STALL_TIMEOUT_SECONDS) {
                        progressData[progressIndex].speed.store(0.0);
                    }

                    // Deliberately do NOT touch lastWritten/lastUpdate here —
                    // let the elapsed window keep growing while a slow write
                    // is still in flight, so the eventual speed sample reflects
                    // the true transfer duration.
                }
            }
        });

        readerThread = std::thread([&]() {
            uint64_t readTotal = 0;

            auto pushSentinel = [&](bool failed) {
                {
                    std::lock_guard<std::mutex> lk(qMutex);
                    if (failed) readFailed = true;
                    fullQ.push_back({nullptr, 0});
                }
                qCv.notify_all();
            };

            while (readTotal < paddedSize && !GlobalState::g_operationCancelled.load()) {
                Chunk c;
                {
                    std::unique_lock<std::mutex> lk(qMutex);
                    qCv.wait(lk, [&] { return stopReader || !emptyQ.empty(); });
                    if (stopReader) return;
                    c = emptyQ.front();
                    emptyQ.pop_front();
                }

                const size_t toRead = static_cast<size_t>(
                    std::min<uint64_t>(bufferSize, paddedSize - readTotal));

                size_t got = 0;
                bool err = false;
                bool reachedEof = false;

                while (got < toRead) {
                    ssize_t r = read(iso_fd, c.buf + got, toRead - got);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        err = true;
                        break;
                    }
                    if (r == 0) {
                        reachedEof = true;
                        break;
                    }
                    got += static_cast<size_t>(r);
                }

                if (err) {
                    pushSentinel(true);
                    return;
                }

                // A source shorter than fstat reported (truncated file) must
                // fail rather than be zero-padded silently.
                if (reachedEof) {
                    if (readTotal + got < fileSize) {
                        pushSentinel(true);
                        return;
                    }
                }

                if (got < toRead) {
                    std::memset(c.buf + got, 0, toRead - got);
                }

                c.len = toRead;
                readTotal += toRead;

                {
                    std::lock_guard<std::mutex> lk(qMutex);
                    fullQ.push_back(c);
                }
                qCv.notify_all();

                if (reachedEof) break;
            }

            pushSentinel(false);
        });

        while (true) {
            Chunk c;
            {
                std::unique_lock<std::mutex> lk(qMutex);
                qCv.wait(lk, [&] { return !fullQ.empty(); });
                c = fullQ.front();
                fullQ.pop_front();
            }

            if (c.len == 0) break;   // sentinel: reader finished, failed, or was cancelled

            if (GlobalState::g_operationCancelled.load()) {
                cancelled = true;
                break;
            }

            // Write loop — handles partial writes and EINTR. Each write() is
            // capped at writeChunk (a multiple of sectorSize), so buffer
            // address, length and device offset all stay sector-aligned for
            // O_DIRECT while progress is reported (and cancellation checked)
            // after every write of <= 4 MiB.
            size_t remainingToWrite = c.len;
            char*  writePtr         = c.buf;
            while (remainingToWrite > 0) {
                const size_t thisWrite = std::min(remainingToWrite, writeChunk);
                ssize_t written = write(dev_fd, writePtr, thisWrite);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    ioFailed = true;
                    break;
                }

                if (written == 0) {   // no progress; avoid infinite spin
                    ioFailed = true;
                    break;
                }

                if (GlobalState::g_operationCancelled.load()) {
                    cancelled = true;
                    break;
                }

                remainingToWrite -= static_cast<size_t>(written);
                writePtr         += written;

                // Cap reported progress at the true file size to hide padding.
                uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize - std::min(localBytesWritten, fileSize));

                localBytesWritten += static_cast<uint64_t>(written);

                directBytesWrittenAccumulator.fetch_add(reportable);
            }

            if (ioFailed || cancelled) break;

            {
                std::lock_guard<std::mutex> lk(qMutex);
                emptyQ.push_back({c.buf, 0});
            }
            qCv.notify_all();
        }
    } catch (...) {
        shutdownThreads();
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    shutdownThreads();

    if (readFailed || ioFailed) {
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_DONTNEED);

    if (!cancelled && !GlobalState::g_operationCancelled.load()) {
        if (fsync(dev_fd) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
    }

    if (!cancelled && !GlobalState::g_operationCancelled.load() && localBytesWritten >= paddedSize) {
        auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTime);
        double seconds  = totalElapsed.count() / 1000.0;
        double avgSpeed = seconds > 0.0 ? (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / seconds : 0.0;

        progressData[progressIndex].speed.store(avgSpeed);
        progressData[progressIndex].progress.store(100);
        progressData[progressIndex].completed.store(true);
        return true;
    }

    return false;
}
