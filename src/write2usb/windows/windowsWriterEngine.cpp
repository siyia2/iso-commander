// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

import std;
import isocmd.core.cancellation;
import isocmd.write2usb.windows.windowsWriterEngine;
import isocmd.write2usb.util.checksum;

namespace fs = std::filesystem;

namespace windowsWriter_engine::copy {

    using IsoType = write2usb_detail::IsoType;
    using IsoCopyEntry = write2usb_detail::IsoCopyEntry;

    namespace {

        /**
         * @brief Per-file range accumulated before it is handed to the shared
         *        filesystem writeback pacer.
         */
        struct FileWritebackState {
            uint64_t pendOff    = 0;   ///< Start of the not-yet-submitted range.
            uint64_t pendLen    = 0;   ///< Length of the not-yet-submitted range.
            int      pendChunks = 0;   ///< Chunks accumulated in that range.
        };

        /** A submitted file range retained for a later writeback wait. */
        struct WritebackRange {
            int      fd = -1;
            uint64_t off = 0;
            uint64_t len = 0;
        };

        /**
         * @brief Closes the duplicated descriptors retained by a range batch.
         */
        void closeRanges(std::vector<WritebackRange>& ranges) {
            for (auto& range : ranges) {
                if (range.fd >= 0)
                    close(range.fd);
            }
            ranges.clear();
        }

        /**
         * @brief sync_file_range() on a file range, retrying interrupted calls.
         *
         * Checks for cancellation before the call and when interrupted by a signal.
         *
         * @param flags SYNC_FILE_RANGE_* flags passed through to the kernel.
         * @return Zero on success, or -1 on failure or cancellation.
         */
        int syncFileRange(int fd, uint64_t off, uint64_t len, unsigned int flags) {
            if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
                errno = ECANCELED;
                return -1;
            }

            int result;

            do {
                result = sync_file_range(
                    fd,
                    static_cast<off_t>(off),
                    static_cast<off_t>(len),
                    flags
                );

                if (result < 0 && errno == EINTR &&
                    GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
                    errno = ECANCELED;
                    return -1;
                }

            } while (result < 0 && errno == EINTR);

            return result;
        }

        /**
         * @brief Shared writeback pacing state for one filesystem copy pass.
         *
         * Each completed file range is submitted asynchronously immediately.
         * Chunk accounting is shared across files, so small files contribute to
         * the same pacing interval instead of resetting the threshold per file.
         * Once the current batch reaches kPaceEveryChunks, the previously
         * submitted batch is waited on and the current batch becomes previous.
         *
         * Duplicated descriptors keep the relevant file descriptions available
         * for the later range wait even after copyFileWithProgress() closes its
         * original descriptor. The destructor only closes descriptors; it does
         * not wait, preserving cancellation responsiveness.
         */
        struct FilesystemWritebackState {
            std::vector<WritebackRange> current;
            std::vector<WritebackRange> previous;
            int currentChunks = 0;

            ~FilesystemWritebackState() {
                closeRanges(current);
                closeRanges(previous);
            }

            void submitRange(int fd, uint64_t off, uint64_t len, int chunks) {
                if (len == 0 || chunks <= 0)
                    return;

                const int heldFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
                if (heldFd < 0)
                    return;

                // Start this range's writeback immediately. This is especially
                // important for a file that finishes below the chunk threshold.
                if (syncFileRange(heldFd, off, len, SYNC_FILE_RANGE_WRITE) != 0) {
                    close(heldFd);
                    return;
                }

                current.push_back({heldFd, off, len});
                currentChunks += chunks;

                if (currentChunks < kPaceEveryChunks)
                    return;

                // The just-submitted batch can drain while the previous batch
                // is waited on, retaining the original double-buffered pacing.
                for (const auto& range : previous) {
                    static_cast<void>(syncFileRange(
                        range.fd, range.off, range.len,
                        SYNC_FILE_RANGE_WAIT_BEFORE |
                        SYNC_FILE_RANGE_WRITE |
                        SYNC_FILE_RANGE_WAIT_AFTER
                    ));
                }

                closeRanges(previous);
                previous.swap(current);
                current.clear();
                currentChunks = 0;
            }
        };

        /**
         * @brief Accumulates contiguous chunks for the current file, then hands
         *        a completed pacing range to the shared filesystem state.
         */
        void paceWriteback(int fd_out, uint64_t off, uint64_t len,
                           FileWritebackState& fileState,
                           FilesystemWritebackState& filesystemState) {
            if (fileState.pendChunks == 0) {
                fileState.pendOff = off;
                fileState.pendLen = 0;
            }

            fileState.pendLen += len;

            if (++fileState.pendChunks < kPaceEveryChunks)
                return;

            filesystemState.submitRange(
                fd_out, fileState.pendOff, fileState.pendLen,
                fileState.pendChunks
            );

            fileState.pendChunks = 0;
            fileState.pendLen = 0;
        }

        /**
         * @brief Submits the final partial range when a file completes.
         *
         * The range is started asynchronously even when the file contains fewer
         * than kPaceEveryChunks chunks. Incomplete/cancelled files deliberately
         * do not submit their tail because the caller truncates them.
         */
        void finishFileWriteback(int fd_out,
                                 FileWritebackState& fileState,
                                 FilesystemWritebackState& filesystemState) {
            if (fileState.pendChunks == 0)
                return;

            filesystemState.submitRange(
                fd_out, fileState.pendOff, fileState.pendLen,
                fileState.pendChunks
            );

            fileState.pendChunks = 0;
            fileState.pendLen = 0;
        }

        /**
         * @brief Fill a buffer, accumulating short reads and retrying EINTR.
         *
         * @return Bytes filled (0 = EOF), or -1 on error / cancellation.
         */
        ssize_t fillChunk(int fd_in, char* buf, size_t bufferSize) {
            size_t total = 0;

            while (total < bufferSize) {
                if (GlobalState::g_operationCancelled.load())
                    return -1;

                const ssize_t n = read(
                    fd_in, buf + total, bufferSize - total
                );

                if (n < 0) {
                    if (errno == EINTR)
                        continue;

                    return -1;
                }

                if (n == 0)
                    break;

                total += static_cast<size_t>(n);
            }

            return static_cast<ssize_t>(total);
        }

        /**
         * @brief Write one chunk, retrying short writes and EINTR.
         *
         * Successfully written bytes are accounted for even if a subsequent
         * write fails. Once the whole chunk has landed, its range is accumulated
         * for the shared filesystem writeback pacer.
         *
         * @param fileWriteback      Per-file contiguous range accumulator.
         * @param filesystemWriteback Shared filesystem pacing state.
         * @return true if the entire chunk was written successfully.
         */
        bool writeOneChunk(int fd_out, const char* buf, size_t len,
                           uint64_t& fileOffset,
                           uint64_t& bytesCopiedTotal,
                           std::atomic<uint64_t>& totalAccumulator,
                           FileWritebackState& fileWriteback,
                           FilesystemWritebackState& filesystemWriteback) {
            const uint64_t chunkOff = fileOffset;
            size_t done = 0;

            while (done < len) {
                if (GlobalState::g_operationCancelled.load())
                    return false;

                const ssize_t written = pwrite(
                    fd_out,
                    buf + done,
                    len - done,
                    static_cast<off_t>(fileOffset)
                );

                if (written < 0) {
                    if (errno == EINTR)
                        continue;

                    return false;
                }

                if (written == 0) {
                    errno = EIO;
                    return false;
                }

                const uint64_t writtenBytes =
                    static_cast<uint64_t>(written);

                done += static_cast<size_t>(written);
                fileOffset += writtenBytes;

                totalAccumulator.fetch_add(writtenBytes);
                bytesCopiedTotal += writtenBytes;
            }

            paceWriteback(
                fd_out, chunkOff, len,
                fileWriteback, filesystemWriteback
            );
            return true;
        }

        /**
         * @brief Copies exactly the expected file size in bounded chunks.
         *
         * Never reads beyond the expected ISO file size and fails if the
         * source reaches EOF before all expected bytes have been copied.
         *
         * Writeback is paced per chunk by writeOneChunk(). A successful file
         * submits its final partial range asynchronously before it is closed;
         * incomplete files do not submit their remaining range.
         *
         * @return true only if exactly the expected file size was copied
         *         without failure or cancellation.
         */
        bool copyFileChunks(int fd_in, int fd_out,
                            uint64_t fileSize,
                            size_t bufferSize,
                            char* buf,
                            std::atomic<uint64_t>& totalAccumulator,
                            uint64_t& bytesCopiedTotal,
                            FilesystemWritebackState& filesystemWriteback,
                            usb_verify::Checksum64* checksum) {
            uint64_t fileOffset = 0;
            FileWritebackState fileWriteback;

            if (bufferSize == 0) {
                errno = EINVAL;
                return false;
            }

            bool success = true;

            while (fileOffset < fileSize &&
                   !GlobalState::g_operationCancelled.load()) {

                // Never read beyond the expected ISO file size.
                const size_t chunkSize = static_cast<size_t>(
                    std::min<uint64_t>(
                        fileSize - fileOffset,
                        static_cast<uint64_t>(bufferSize)
                    )
                );

                const ssize_t n = fillChunk(
                    fd_in, buf, chunkSize
                );

                if (n < 0) {
                    success = false;
                    break;
                }

                // fillChunk() only returns early when EOF is reached.
                if (static_cast<size_t>(n) != chunkSize) {
                    errno = EIO;
                    success = false;
                    break;
                }

                if (checksum) checksum->update(buf, chunkSize);

                if (!writeOneChunk(
                        fd_out, buf, chunkSize,
                        fileOffset, bytesCopiedTotal,
                        totalAccumulator, fileWriteback,
                        filesystemWriteback)) {
                    success = false;
                    break;
                }
            }

            // On failure or cancellation the caller truncates the file. Do not
            // submit this file's remaining range: it belongs to the incomplete file.
            if (!success || GlobalState::g_operationCancelled.load())
                return false;

            // Require an exact match, not merely a minimum.
            if (bytesCopiedTotal != fileSize)
                return false;

            // Submit the final partial range asynchronously before this file's
            // descriptor is closed. Small files therefore enter shared pacing.
            finishFileWriteback(fd_out, fileWriteback, filesystemWriteback);
            return true;
        }

        /**
         * @brief Opens, optionally pre-allocates and copies one file.
         *
         * Pre-allocation is done for NTFS only. On failure or cancellation,
         * partially written FAT32 files are truncated to zero bytes.
         */
        bool copyFileWithProgress(const fs::path& src, const fs::path& dst,
                                  uint64_t fileSize, bool preallocate,
                                  size_t bufferSize, char* buf,
                                  std::atomic<uint64_t>& totalAccumulator,
                                  FilesystemWritebackState& filesystemWriteback,
                                  uint64_t* expectedChecksum) {
            const int fd_in = open(
                src.c_str(), O_RDONLY | O_CLOEXEC
            );

            if (fd_in < 0)
                return false;

            posix_fadvise(
                fd_in, 0, 0, POSIX_FADV_SEQUENTIAL
            );

            const int fd_out = open(
                dst.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                0644
            );

            if (fd_out < 0) {
                close(fd_in);
                return false;
            }

            if (preallocate && fileSize > 0)
                posix_fallocate(
                    fd_out, 0, static_cast<off_t>(fileSize)
                );

            uint64_t bytesCopiedTotal = 0;

            usb_verify::Checksum64 checksum;
            const bool success = copyFileChunks(
                fd_in, fd_out, fileSize, bufferSize, buf,
                totalAccumulator, bytesCopiedTotal,
                filesystemWriteback, expectedChecksum ? &checksum : nullptr
            );

            if (!success) {
                // Drops dirty pages without writing them and frees any preallocation.
                // Retry an interruption, but stop on a permanent cleanup error.
                // The copy is already failed; never report completion in either case.
                int r;
                do {
                    r = ftruncate(fd_out, 0);
                } while (r == -1 && errno == EINTR);
            }

            posix_fadvise(
                fd_in, 0, 0, POSIX_FADV_DONTNEED
            );

            close(fd_in);
            close(fd_out);

            if (success && expectedChecksum) *expectedChecksum = checksum.value;
            return success &&
                   !GlobalState::g_operationCancelled.load();
        }

    } // namespace

    /**
     * @brief Creates all required directories and copies the entry list.
     *
     * Collects and deduplicates directory paths before creating them,
     * avoiding redundant directory creation when processing multiple files.
     * Files are copied only after all required directories exist.
     *
     * @return true if all entries were processed successfully.
     */
    bool processEntries(const std::vector<IsoCopyEntry>& entries,
                        IsoType isoType,
                        const std::string& ntfsMnt,
                        size_t bufferSize,
                        char* buf,
                        std::atomic<uint64_t>& totalAccumulator,
                        std::vector<uint64_t>* checksums) {

        if (checksums) {
            checksums->clear();
            checksums->reserve(entries.size());
        }
        // Collect every directory that needs to exist.
        std::vector<fs::path> directories;
        directories.reserve(entries.size());

        for (const auto& e : entries) {
            fs::path dir = e.isDir ? e.dst : e.dst.parent_path();

            if (!dir.empty())
                directories.push_back(std::move(dir));
        }

        // Eliminate duplicate directory paths.
        std::sort(directories.begin(), directories.end());

        directories.erase(
            std::unique(directories.begin(), directories.end()),
            directories.end()
        );

        // Create each required directory only once.
        for (const auto& dir : directories) {
            if (GlobalState::g_operationCancelled.load())
                return false;

            fs::create_directories(dir);
        }

        // One shared pacing state spans every file written to this filesystem
        // during this copy pass. Small files no longer reset the chunk threshold.
        FilesystemWritebackState filesystemWriteback;

        // All parent directories now exist. Copy files only.
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load())
                return false;

            if (e.isDir)
                continue;

            const bool toNtfs =
                (isoType == IsoType::WindowsInstall) &&
                e.dst.native().compare(0, ntfsMnt.size(), ntfsMnt) == 0;

            uint64_t checksum = 0;
            if (!copyFileWithProgress(
                    e.src, e.dst, e.size, toNtfs,
                    bufferSize, buf, totalAccumulator,
                    filesystemWriteback, checksums ? &checksum : nullptr)) {
                return false;
            }
            if (checksums) checksums->push_back(checksum);
        }

        return true;
    }

} // namespace windowsWriter_engine::copy
