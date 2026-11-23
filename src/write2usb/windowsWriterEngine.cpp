// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

// Project Headers
#include "isocmd/core/cancellation.h"
#include "isocmd/write2usb/windowsWriterEngine.h"

namespace fs = std::filesystem;

namespace windowsWriter_engine::copy {

    using IsoType = write2usb_detail::IsoType;
    using IsoCopyEntry = write2usb_detail::IsoCopyEntry;

    namespace {

        /**
         * @brief Per-file writeback pacing state.
         */
        struct WritebackState {
            uint64_t pendOff    = 0;
            uint64_t pendLen    = 0;
            int      pendChunks = 0;
            uint64_t prevOff    = 0;
            uint64_t prevLen    = 0;
            bool     havePrev   = false;
        };

        /**
         * @brief Bound the dirty-page backlog of an output file.
         */
        void paceWriteback(int fd_out, uint64_t off, uint64_t len,
                           WritebackState& st) {
            if (GlobalState::g_operationCancelled.load()) {
                st.pendChunks = 0;
                st.havePrev = false;
                return;
            }

            if (st.pendChunks == 0) {
                st.pendOff = off;
                st.pendLen = 0;
            }

            st.pendLen += len;

            if (++st.pendChunks < kPaceEveryChunks) return;

            sync_file_range(fd_out, static_cast<off_t>(st.pendOff),
                            static_cast<off_t>(st.pendLen),
                            SYNC_FILE_RANGE_WRITE);

            if (GlobalState::g_operationCancelled.load()) {
                st.pendChunks = 0;
                st.havePrev = false;
                return;
            }

            if (st.havePrev) {
                sync_file_range(fd_out, static_cast<off_t>(st.prevOff),
                                static_cast<off_t>(st.prevLen),
                                SYNC_FILE_RANGE_WAIT_BEFORE |
                                SYNC_FILE_RANGE_WRITE |
                                SYNC_FILE_RANGE_WAIT_AFTER);
            }

            st.prevOff    = st.pendOff;
            st.prevLen    = st.pendLen;
            st.havePrev   = true;
            st.pendChunks = 0;
        }

        /**
         * @brief Fill a buffer, accumulating short reads and retrying EINTR.
         *
         * @return Bytes filled (0 = EOF), or -1 on error / cancellation.
         */
        ssize_t fillChunk(int fd_in, char* buf, size_t bufferSize) {
            size_t total = 0;

            while (total < bufferSize) {
                if (GlobalState::g_operationCancelled.load()) return -1;

                const ssize_t n = read(fd_in, buf + total, bufferSize - total);

                if (n < 0) {
                    if (errno == EINTR) continue;
                    return -1;
                }

                if (n == 0) break;

                total += static_cast<size_t>(n);
            }

            return static_cast<ssize_t>(total);
        }

        /**
         * @brief Write one chunk and advance the file offset.
         *
         * @return true if the chunk was written completely; false otherwise.
         */
        bool writeOneChunk(int fd_out, const char* buf, size_t len,
                           uint64_t& fileOffset,
                           uint64_t& bytesCopiedTotal,
                           std::atomic<uint64_t>& totalAccumulator,
                           WritebackState& wb) {
            size_t done = 0;

            while (done < len) {
                if (GlobalState::g_operationCancelled.load()) return false;

                const ssize_t written = pwrite(
                    fd_out, buf + done, len - done,
                    static_cast<off_t>(fileOffset + done));

                if (written < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }

                if (written == 0) {
                    errno = EIO;
                    return false;
                }

                done += static_cast<size_t>(written);
                totalAccumulator.fetch_add(static_cast<uint64_t>(written));
                bytesCopiedTotal += static_cast<uint64_t>(written);
            }

            paceWriteback(fd_out, fileOffset, len, wb);
            fileOffset += len;

            return true;
        }

        /**
         * @brief Copies file data in chunks while tracking progress and checking
         *        for cancellation and source truncation.
         *
         * @return true only if the complete expected file size was copied.
         */
        bool copyFileChunks(int fd_in, int fd_out,
                            uint64_t fileSize,
                            size_t bufferSize,
                            char* buf,
                            std::atomic<uint64_t>& totalAccumulator,
                            uint64_t& bytesCopiedTotal) {
            uint64_t fileOffset = 0;
            WritebackState wb;
            bool success = true;

            while (!GlobalState::g_operationCancelled.load()) {
                const ssize_t n = fillChunk(fd_in, buf, bufferSize);

                if (n < 0) {
                    success = false;
                    break;
                }

                if (n == 0) break;

                if (!writeOneChunk(fd_out, buf, static_cast<size_t>(n),
                                   fileOffset, bytesCopiedTotal,
                                   totalAccumulator, wb)) {
                    success = false;
                    break;
                }
            }

            if (GlobalState::g_operationCancelled.load())
                success = false;

            // Fewer bytes than the ISO walk reported means the source was truncated.
            if (success && bytesCopiedTotal < fileSize)
                success = false;

            return success;
        }

        /**
         * @brief Opens, optionally pre-allocates and copies one file.
         *
         * Pre-allocation is done for NTFS only. On failure or cancellation,
         * partially written FAT32 files are truncated to 0 bytes.
         */
        bool copyFileWithProgress(const fs::path& src, const fs::path& dst,
                                  uint64_t fileSize, bool preallocate,
                                  size_t bufferSize, char* buf,
                                  std::atomic<uint64_t>& totalAccumulator) {
            const int fd_in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_in < 0) return false;

            posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

            const int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

            if (fd_out < 0) {
                close(fd_in);
                return false;
            }

            if (preallocate && fileSize > 0)
                posix_fallocate(fd_out, 0, static_cast<off_t>(fileSize));

            uint64_t bytesCopiedTotal = 0;

            const bool success = copyFileChunks(
                fd_in, fd_out, fileSize, bufferSize, buf,
                totalAccumulator, bytesCopiedTotal);

            if (!success && !preallocate) {
                // Don't leave partial FAT32 files behind.
                [[maybe_unused]] const int r = ftruncate(fd_out, 0);
            }

            posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);

            close(fd_in);
            close(fd_out);

            return success && !GlobalState::g_operationCancelled.load();
        }

    } // namespace

    /**
     * @brief Iterate one entry list, creating directories and copying files.
     */
    bool processEntries(const std::vector<IsoCopyEntry>& entries,
                        IsoType isoType,
                        const std::string& ntfsMnt,
                        size_t bufferSize,
                        char* buf,
                        std::atomic<uint64_t>& totalAccumulator) {
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load()) return false;

            if (e.isDir) {
                fs::create_directories(e.dst);
            } else {
                fs::create_directories(e.dst.parent_path());

                const bool toNtfs =
                    (isoType == IsoType::WindowsInstall) &&
                    (e.dst.string().rfind(ntfsMnt, 0) == 0);

                if (!copyFileWithProgress(e.src, e.dst, e.size, toNtfs,
                                          bufferSize, buf, totalAccumulator)) {
                    return false;
                }
            }
        }

        return true;
    }

} // namespace windowsWriter_engine::copy
