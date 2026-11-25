// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

import std;
import isocmd.core.cancellation;
import isocmd.write2usb.util.alignedBuffer;
import isocmd.write2usb.util.isoDetect;
import isocmd.write2usb.windows.windowsWriter;
import isocmd.write2usb.ui.write2usbUI;

namespace {

    // ===========================================================================
    // Write pipeline
    // ===========================================================================

    //---- io: descriptors, sector size, buffers, ISO preparation --
    namespace io {

        /// Target size of each I/O buffer (rounded down to a sector multiple at use).
        constexpr size_t kDesiredBuffer  = 4 * 1024 * 1024;

        /// Upper bound for a single device write(); keeps progress/cancel granularity fine.
        constexpr size_t kMaxWriteChunk  = 4 * 1024 * 1024;

        /// Number of circulating buffers (double buffering).
        constexpr int    kNumBuffers     = 2;
        static_assert(kMaxWriteChunk <= kDesiredBuffer,
                      "write chunk larger than the buffer is pointless");

        /// Pool of sector-aligned buffers. AlignedBuffer (write2usb.h) is non-movable,
        /// so the pool holds unique_ptrs.
        using IsoBufferPool = std::vector<std::unique_ptr<AlignedBuffer>>;

        /**
         * @brief RAII owner of a file descriptor.
         *
         * Closes the descriptor on scope exit if it is valid (!= -1). Not copyable.
         */
        struct FdGuard {
            int fd;
            explicit FdGuard(int f) : fd(f) {}
            ~FdGuard() { if (fd != -1) close(fd); }
            FdGuard(const FdGuard&)            = delete;
            FdGuard& operator=(const FdGuard&) = delete;
        };

        /**
         * @brief Queries the sector size that O_DIRECT I/O must be aligned to.
         *
         * Returns max(logical, physical) sector size, which is correct for 512n,
         * 512e and 4Kn drives. BLKPBSZGET is best-effort: if it fails, the logical
         * size is used.
         *
         * @par Utility
         * Drives store data in fixed-size blocks called sectors, usually 512 or 4096
         * bytes. Writing straight to the drive, bypassing the system's cache, only works
         * if the data is sent in whole sectors of exactly the right size. The size
         * differs between drives, so the program has to ask the drive.
         *
         * @param devFd      Open block-device descriptor.
         * @param[out] sectorSize Receives the effective sector size in bytes.
         * @return true on success; false if the logical size cannot be determined.
         */
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

        /**
         * @brief Allocates kNumBuffers sector-aligned buffers into @p out.
         *
         * @par Utility
         * Direct writes also need the data to sit in memory at a specially aligned
         * address, which ordinary memory allocation does not promise. This prepares
         * those memory areas ("buffers"). There are two, so one can be filled from the
         * ISO while the other is being written to the drive.
         *
         * @param out        Pool to fill (appended to).
         * @param sectorSize Alignment of every buffer, in bytes.
         * @param bufferSize Size of every buffer, in bytes (multiple of sectorSize).
         * @return true if all buffers were allocated; false on the first failure.
         */
        bool allocateAlignedBuffers(IsoBufferPool& out, int sectorSize, size_t bufferSize) {
            for (int i = 0; i < kNumBuffers; ++i) {
                auto buf = std::make_unique<AlignedBuffer>(bufferSize, static_cast<size_t>(sectorSize));
                if (!buf->ok())
                    return false;
                out.push_back(std::move(buf));
            }
            return true;
        }

        /**
         * @brief Validates the ISO, rewinds it, and fetches its stat info.
         *
         * @par Utility
         * Gathers the checks that must pass before any copying starts: the file is a
         * real disc image, it will be read from its beginning, and its size is known.
         * The caller handles one failure instead of three.
         *
         * @param isoFd      Open, readable ISO descriptor.
         * @param[out] sb    Receives fstat() data on success.
         * @return true if the image is a valid ISO 9660/UDF image, the offset was
         *         reset to 0 and fstat succeeded.
         */
        bool prepareIso(int isoFd, struct stat& sb) {
            return iso_detect::isValidIso9660(isoFd)
                && lseek(isoFd, 0, SEEK_SET) != -1
                && fstat(isoFd, &sb) != -1;
        }

    } // namespace io

    //---- progress: failure flag and speed estimation -------------
    namespace progress {

        /// Clock used for all progress and speed timing.
        using Clock = std::chrono::high_resolution_clock;

        /**
         * @brief Flags one operation as failed.
         *
         * @par Utility
         * When something goes wrong, two things must always happen: the progress
         * display is told this job failed, and the function reports failure. Doing both
         * in one place keeps every error path consistent.
         *
         * @param progressData Progress array shared with the UI.
         * @param index        Slot belonging to this operation.
         * @return Always false, so callers can write `return markFailed(...)`.
         */
        bool markFailed(std::vector<ProgressInfo>& progressData, size_t index) {
            progressData[index].failed.store(true);
            return false;
        }

        /**
         * @brief Exponentially smoothed write-speed estimator with stall handling.
         *
         * Fed periodically with the cumulative byte count. Samples closer than one
         * second apart are ignored. While bytes keep arriving it publishes a smoothed
         * MB/s figure (heavier smoothing below 10 MB/s). If nothing arrives it holds
         * the last figure, and reports 0 after 60 s without progress.
         *
         * @par Utility
         * USB drives write in bursts, so the raw speed jumps up and down and would
         * flicker on screen. This evens it out, keeps showing the last speed during short
         * pauses, and shows 0 if nothing has been written for a minute.
         */
        class SpeedEstimator {
        public:
            /**
             * @brief Creates an estimator whose baseline is the transfer start.
             *
             * @par Utility
             * Starting the clock at the real start time makes the first measurement
             * accurate.
             * @param start Time the transfer started (initial baseline).
             */
            explicit SpeedEstimator(Clock::time_point start)
                : lastUpdate_(start), stallStart_(start) {}

            /**
             * @brief Feeds one observation.
             *
             * @par Utility
             * It is the single place where measurements are fed in. It ignores updates that
             * arrive too quickly and picks the right handling for "data was written" or
             * "nothing was written".
             *
             * @param currentWritten Cumulative bytes written so far.
             * @param now            Time of the observation.
             * @return Speed in MB/s to publish, or std::nullopt if nothing should change.
             */
            std::optional<double> sample(uint64_t currentWritten, Clock::time_point now) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           now - lastUpdate_).count();
                if (elapsedMs < kMinUpdateIntervalMs)
                    return std::nullopt;

                const uint64_t delta = (currentWritten >= lastWritten_) ? (currentWritten - lastWritten_) : 0;
                return delta > 0 ? onProgress(currentWritten, delta, elapsedMs, now)
                                 : onNoProgress(now);
            }

        private:
            /// Smoothing factor for normal speeds (0.2 is used below 10 MB/s).
            static constexpr double kAlpha = 0.3;
            /// Minimum time between estimates.
            static constexpr int kMinUpdateIntervalMs = 1000;
            /// Time without progress after which the speed is reported as 0.
            static constexpr int kStallTimeoutSeconds = 60;

            /**
             * @brief Handles a sample in which bytes arrived.
             *
             * Computes the instantaneous speed over the window since the last real
             * data point, folds it into the smoothed value and resets the baseline.
             *
             * @par Utility
             * Works out the speed from what was written since the last measurement, and
             * blends it with earlier values so the number changes smoothly.
             *
             * @param currentWritten Cumulative bytes written.
             * @param delta          Bytes written since the last baseline (> 0).
             * @param elapsedMs      Milliseconds since the last baseline.
             * @param now            Time of the observation.
             * @return The new smoothed speed in MB/s.
             */
            double onProgress(uint64_t currentWritten, uint64_t delta, long long elapsedMs,
                              Clock::time_point now) {
                stalled_ = false;

                const double instant = (static_cast<double>(delta) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
                // Heavier smoothing for slow drives (< 10 MB/s) to reduce display
                // jitter from bursty O_DIRECT writes at low throughput.
                const double factor = (instant < 10.0) ? 0.2 : kAlpha;

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

            /**
             * @brief Handles a sample in which no bytes arrived.
             *
             * Starts (or continues) stall tracking and leaves the baseline untouched
             * so the next real sample spans the full transfer duration.
             *
             * @par Utility
             * Handles the case where nothing new was written. It waits, keeps the last
             * speed on screen, and only reports 0 after a full minute of silence. The
             * measuring window keeps growing meanwhile, so a slow write is judged fairly.
             *
             * @param now Time of the observation.
             * @return 0.0 once the stall timeout is reached; otherwise std::nullopt
             *         (keep showing the last speed).
             */
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
                if (stallSeconds >= kStallTimeoutSeconds)
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

    } // namespace progress

    //---- stream: the threaded ISO -> device transfer -------------
    namespace stream {

        using progress::Clock;
        using io::IsoBufferPool;

        /**
         * @brief Streams an ISO to a block device using double buffering.
         *
         * A reader thread fills aligned buffers from the ISO (zero-padding the tail);
         * the calling thread writes them to the device with O_DIRECT; a monitor
         * thread publishes progress and speed. Buffers circulate between an "empty"
         * and a "full" queue. Single use: construct, call run() once.
         */
        class IsoStreamer {
        public:
            /**
             * @param isoFd         ISO descriptor, positioned at offset 0 (not owned).
             * @param devFd         Block device opened with O_DIRECT (not owned).
             * @param fileSize      True ISO size in bytes.
             * @param paddedSize    @p fileSize rounded up to a sector multiple.
             * @param bufferSize    Size of each buffer (multiple of the sector size).
             * @param writeChunk    Maximum bytes per write() (multiple of the sector size).
             * @param buffers       Pre-allocated aligned buffers; they must outlive the streamer.
             * @param progressData  Progress array shared with the UI.
             * @param progressIndex Slot of this operation in @p progressData.
             */
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

            /**
             * @brief Runs the transfer to completion, failure or cancellation.
             *
             * Starts the helper threads, writes on the calling thread, joins the
             * helpers, then (on success) drops the ISO from the page cache and
             * fsync()s the device.
             *
             * @return true if every byte was written and flushed; false on read
             *         failure, I/O failure, fsync failure or cancellation.
             */
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
            /// A buffer handed between the reader and the writer.
            struct Chunk {
                char*  buf;   ///< Aligned buffer start.
                size_t len;   ///< Valid bytes; 0 == end-of-stream sentinel.
            };

            /**
             * @brief Reads the global cancellation flag.
             *
             * @par Utility
             * The user can press cancel at any moment. This is the one way every part of
             * the copy asks "should I stop?".
             *
             * @return true if the user requested cancellation globally.
             */
            static bool isCancelled() { return GlobalState::g_operationCancelled.load(); }

            /**
             * @brief Tells whether the transfer was stopped on purpose.
             *
             * @par Utility
             * It tells "stopped on purpose" apart from "something went wrong". A cancelled
             * copy should not be shown as an error and does not need the final flush.
             *
             * @return true if this streamer stopped, or the user cancelled.
             */
            bool aborted() const { return cancelled_ || isCancelled(); }

            /**
             * @brief Flags this operation as failed.
             *
             * @par Utility
             * A shortcut for telling the progress display that this copy failed.
             *
             * @return Always false.
             */
            bool markFailed() {
                return progress::markFailed(progressData_, progressIndex_);
            }

            // ---- lifecycle -------------------------------------------------------

            /**
             * @brief Stops and joins the reader and monitor threads.
             *
             * Idempotent. The condition variables are notified so neither thread
             * stays blocked.
             *
             * @par Utility
             * The copy uses two helper workers, one reading the ISO and one reporting
             * progress. They must always be stopped and waited for, even after an error,
             * otherwise the program could crash or hang.
             */
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

            /**
             * @brief Publishes the final result if the whole image was written.
             *
             * Stores the average speed, sets progress to 100 and marks completion.
             *
             * @par Utility
             * It makes sure "100 %" and "done" are shown only when every byte has really
             * been written and the copy was not cancelled.
             *
             * @return true if the transfer completed; false if aborted or incomplete.
             */
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

            /**
             * @brief Waits one 500 ms monitor tick, interruptibly.
             *
             * Sleeps on a condition variable so shutdown wakes it immediately
             * instead of waiting out a fixed sleep.
             *
             * @return false once shutdown was requested.
             */
            bool waitForTick() {
                {
                    std::unique_lock<std::mutex> lk(monMutex_);
                    monCv_.wait_for(lk, std::chrono::milliseconds(500),
                                    [this] { return !monitoringActive_.load(); });
                }
                return monitoringActive_.load();
            }

            /**
             * @brief Publishes byte count and percentage to the UI.
             *
             * The percentage is capped at 99 until finishIfComplete() sets 100.
             *
             * @par Utility
             * Turns "bytes written" into the percentage shown on screen. It stops at 99 %
             * because the data is not safely on the drive until the final flush is done.
             *
             * @param currentWritten Cumulative bytes written, excluding tail padding.
             */
            void publishProgress(uint64_t currentWritten) {
                auto& info = progressData_[progressIndex_];
                info.bytesWritten.store(currentWritten);
                info.progress.store(static_cast<int>(
                    std::min(99.0, (static_cast<double>(currentWritten) / fileSize_) * 100.0)));
            }

            /**
             * @brief Monitor-thread body: every tick, publishes progress and the smoothed speed.
             *
             * @par Utility
             * A separate worker updates the progress and speed, so the screen stays
             * current even while the drive is busy with a slow write.
             */
            void monitorLoop() {
                progress::SpeedEstimator estimator(Clock::now());

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

            /**
             * @brief Queues the end-of-stream marker for the writer.
             *
             * @par Utility
             * The writer waits for data, so it has to be told clearly when no more will
             * come, whether the file ended normally or reading failed. The failure note is
             * recorded together with this message so the writer cannot miss it.
             *
             * @param failed If true, also records that the reader failed.
             */
            void pushSentinel(bool failed) {
                {
                    std::lock_guard<std::mutex> lk(qMutex_);
                    if (failed) readFailed_ = true;
                    fullQ_.push_back({nullptr, 0});
                }
                qCv_.notify_all();
            }

            /**
             * @brief Hands a filled buffer to the writer and wakes it.
             *
             * @par Utility
             * Passes the buffer on and alerts the writer in one step, so a buffer is never
             * left waiting unnoticed.
             */
            void pushFull(const Chunk& c) {
                {
                    std::lock_guard<std::mutex> lk(qMutex_);
                    fullQ_.push_back(c);
                }
                qCv_.notify_all();
            }

            /**
             * @brief Blocks until an empty buffer is available.
             *
             * @par Utility
             * The reader waits here until a buffer is free. That stops it from racing ahead
             * of the writer and using more and more memory. It can also be interrupted, so
             * the copy can always be stopped.
             *
             * @param[out] out The acquired buffer.
             * @return false if the reader was told to stop while waiting.
             */
            bool acquireEmptyBuffer(Chunk& out) {
                std::unique_lock<std::mutex> lk(qMutex_);
                qCv_.wait(lk, [this] { return stopReader_ || !emptyQ_.empty(); });
                if (stopReader_) return false;
                out = emptyQ_.front();
                emptyQ_.pop_front();
                return true;
            }

            /**
             * @brief Reads until @p toRead bytes are gathered, EOF, or an error.
             *
             * Retries on EINTR. A short read mid-file is not treated as EOF; only a
             * read() returning 0 is.
             *
             * @par Utility
             * A read can return less data than asked for even though more is coming, and
             * the drive wants full blocks. This keeps asking until the buffer is full or
             * the file really ends, and it tells a real end apart from an error.
             *
             * @param buf            Destination buffer.
             * @param toRead         Bytes wanted.
             * @param[out] got       Bytes actually read.
             * @param[out] reachedEof Set when read() returned 0.
             * @return false on a read error; true otherwise.
             */
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

            /**
             * @brief Reader-thread body.
             *
             * Fills buffers until the padded size is read, zero-padding the tail.
             * A source shorter than fstat reported is treated as a failure instead of
             * being silently padded. Always ends by pushing the end-of-stream
             * sentinel (unless told to stop).
             *
             * @par Utility
             * Reading the ISO runs alongside writing to the USB drive, so neither has to
             * wait for the other.
             */
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

            /**
             * @brief Blocks for the next filled buffer.
             *
             * @param[out] out The received chunk.
             * @return false on the end-of-stream sentinel (reader finished, failed
             *         or was cancelled).
             */
            bool popFull(Chunk& out) {
                std::unique_lock<std::mutex> lk(qMutex_);
                qCv_.wait(lk, [this] { return !fullQ_.empty(); });
                out = fullQ_.front();
                fullQ_.pop_front();
                return out.len != 0;
            }

            /**
             * @brief Returns a written buffer to the empty queue and wakes the reader.
             */
            void recycle(char* buf) {
                {
                    std::lock_guard<std::mutex> lk(qMutex_);
                    emptyQ_.push_back({buf, 0});
                }
                qCv_.notify_all();
            }

            /**
             * @brief Accounts for bytes just written to the device.
             *
             * Updates the internal total (including padding) and the reported total,
             * which is capped at the true file size so padding stays hidden.
             *
             * @par Utility
             * Keeps the running total. The padding added to fill the last block is left
             * out of the reported figure, so progress never goes beyond the real file size,
             * but it is still counted internally to know when everything is done.
             *
             * @param written Bytes written by the last write() call.
             */
            void recordWritten(size_t written) {
                // Cap reported progress at the true file size to hide padding.
                const uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize_ - std::min(localBytesWritten_, fileSize_));

                localBytesWritten_ += static_cast<uint64_t>(written);
                directBytesWritten_.fetch_add(reportable);
            }

            /**
             * @brief Writes one buffer to the device.
             *
             * Handles partial writes and EINTR. Each write() is capped at
             * writeChunk_ (a multiple of the sector size), so buffer address, length
             * and device offset all stay sector-aligned for O_DIRECT, while progress
             * is reported and cancellation checked after every write of <= 4 MiB.
             *
             * @par Utility
             * Sending data to a drive may take several tries, because it can be partial or
             * interrupted. This deals with that, and checks for cancel after each piece,
             * so the main loop can stay simple.
             *
             * @param c Chunk to write.
             * @return false on I/O failure or cancellation (ioFailed_ / cancelled_
             *         say which).
             */
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

            /**
             * @brief Writer loop (calling thread): pops full chunks, writes them, recycles buffers.
             *
             * @par Utility
             * This is the main loop of the writing side: take a filled buffer, write it,
             * hand it back. It is kept separate so errors can be handled in one place.
             */
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

    } // namespace stream

} // namespace

// ---------------------------------------------------------------------------
// Global scope: public entry point
// ---------------------------------------------------------------------------

/**
 * @brief Writes an ISO image to a raw block device, routing Windows and
 *        Linux media to the appropriate writer.
 *
 * Opens the ISO once, validates it with iso_detect::isValidIso9660, then checks
 * for Windows media with iso_detect::isWindowsIsoInitialCheck. Windows images
 * are handed to writeWindowsIsoToDevice (multi-partition layout, FAT32+NTFS,
 * cluster tuning). Everything else is written as a raw image using O_DIRECT.
 *
 * @par Raw write path
 * Uses two 4 MiB sector-aligned buffers, rounded down to the device's
 * effective sector size (the larger of BLKSSZGET and BLKPBSZGET; the physical
 * size is best-effort) so O_DIRECT alignment holds on 512n, 512e and 4Kn
 * drives. A reader thread fills one buffer, including zero-padding of the
 * final partial sector, while the calling thread writes the other. Device
 * writes are sector-aligned and capped at 4 MiB to keep progress and
 * cancellation responsive on slow USB drives. Short reads mid-file are not
 * treated as EOF.
 *
 * @par Progress
 * A monitor thread samples the written-byte count every 500 ms and publishes
 * progress (capped at 99% until done) and a smoothed speed. Speed holds
 * through stalls and resets to 0 after 60 s without progress.
 *
 * @par Cancellation
 * Both reader and writer check GlobalState::g_operationCancelled. On abort
 * they stop promptly and skip the final fsync.
 *
 * @param isoPath       Path to the source ISO.
 * @param device        Target block device (e.g. /dev/sdb).
 *                      WARNING: existing data is destroyed.
 * @param progressIndex Slot in the progress array for this operation.
 * @param progressData  Operation-local progress, speed and completion data.
 * @return true if the image was fully written and fsynced; false on I/O
 *         error, validation failure or cancellation. The result is not read
 *         back or verified against the ISO.
 *
 * @see iso_detect::isValidIso9660
 * @see iso_detect::isWindowsIsoInitialCheck
 * @see writeWindowsIsoToDevice
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex, std::vector<ProgressInfo>& progressData) {

    const auto fail = [&] { return progress::markFailed(progressData, progressIndex); };

    io::FdGuard iso(open(isoPath.c_str(), O_RDONLY));
    if (iso.fd == -1) return fail();

    struct stat sb;
    if (!io::prepareIso(iso.fd, sb)) return fail();

    if (iso_detect::isWindowsIsoInitialCheck(iso.fd, sb.st_size))
        return writeWindowsIsoToDevice(isoPath, device, progressIndex, progressData);

    if (lseek(iso.fd, 0, SEEK_SET) == -1) return fail();
    posix_fadvise(iso.fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    io::FdGuard dev(open(device.c_str(), O_WRONLY | O_DIRECT));
    if (dev.fd == -1) return fail();

    int sectorSize = 0;
    if (!io::queryEffectiveSectorSize(dev.fd, sectorSize))
        return fail();

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) return fail();

    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    size_t bufferSize = (io::kDesiredBuffer / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Device writes are capped at 4 MiB and kept sector-aligned so the
    // progress byte counter advances at fine granularity. With the current
    // 4 MiB buffers each buffer is one write(); the cap matters if
    // io::kDesiredBuffer is raised, because a single very large write() on a
    // slow USB drive (< 10 MB/s) blocks for many seconds and makes the
    // monitor's speed samples noisy. Must stay a multiple of sectorSize for
    // O_DIRECT.
    const size_t writeChunk = std::max<size_t>((io::kMaxWriteChunk / sectorSize) * sectorSize,
                                               static_cast<size_t>(sectorSize));

    io::IsoBufferPool buffers;
    if (!io::allocateAlignedBuffers(buffers, sectorSize, bufferSize))
        return fail();

    stream::IsoStreamer streamer(iso.fd, dev.fd, fileSize, paddedSize, bufferSize, writeChunk,
                                 buffers, progressData, progressIndex);
    return streamer.run();
}
