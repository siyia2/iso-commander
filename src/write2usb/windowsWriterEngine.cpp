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
        * @brief Maximum amount of outstanding written data per file.
        */
       constexpr uint64_t kMaxWritebackBytes = 4ULL * 1024 * 1024;

       /**
        * @brief Tracks the current uncompleted writeback range.
        *
        * Includes all successfully written bytes, whether submitted for
        * writeback or still waiting to be submitted.
        */
       struct WritebackState {
           uint64_t rangeOff = 0;
           uint64_t rangeLen = 0;
       };

       /**
        * @brief Synchronizes a file range, retrying interrupted calls.
        *
        * Checks for cancellation when interrupted by a signal.
        *
        * @return Zero on success, or -1 on failure or cancellation.
        */
       int syncFileRange(int fd, uint64_t off, uint64_t len) {
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
                   SYNC_FILE_RANGE_WAIT_BEFORE |
                   SYNC_FILE_RANGE_WRITE |
                   SYNC_FILE_RANGE_WAIT_AFTER
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
        * @brief Submit and wait for the accumulated writeback range.
        *
        * Resets the tracked range only after synchronization succeeds.
        *
        * @return true if the range completed successfully or was empty.
        */
       bool flushWriteback(int fd_out, WritebackState& st) {
           if (st.rangeLen == 0)
               return true;

           if (syncFileRange(
                   fd_out, st.rangeOff, st.rangeLen) < 0) {
               return false;
           }

           st.rangeOff += st.rangeLen;
           st.rangeLen = 0;

           return true;
       }

       /**
        * @brief Account for successfully written bytes and enforce the budget.
        *
        * Tracks both unsubmitted and submitted-but-uncompleted data.
        * Once the accumulated range reaches kMaxWritebackBytes, writeback
        * is completed before additional data can be written.
        *
        * @return true if accounting and any required synchronization succeeded.
        */
       bool paceWriteback(int fd_out, uint64_t off, uint64_t len,
                          WritebackState& st) {
           if (len == 0)
               return true;

           if (st.rangeLen == 0)
               st.rangeOff = off;

           st.rangeLen += len;

           if (st.rangeLen >= kMaxWritebackBytes)
               return flushWriteback(fd_out, st);

           return true;
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
        * @brief Write one chunk while enforcing the writeback budget.
        *
        * Limits individual writes to the remaining outstanding budget.
        * When the budget is exhausted, waits for writeback completion
        * before issuing another write.
        *
        * Successfully written bytes are accounted for even if a subsequent
        * write or synchronization operation fails.
        *
        * @return true if the entire chunk was written successfully.
        */
       bool writeOneChunk(int fd_out, const char* buf, size_t len,
                          uint64_t& fileOffset,
                          uint64_t& bytesCopiedTotal,
                          std::atomic<uint64_t>& totalAccumulator,
                          WritebackState& wb) {
           size_t done = 0;

           while (done < len) {
               if (GlobalState::g_operationCancelled.load())
                   return false;

               // Never exceed the outstanding writeback budget.
               if (wb.rangeLen == kMaxWritebackBytes) {
                   if (!flushWriteback(fd_out, wb))
                       return false;
               }

               const uint64_t available =
                   kMaxWritebackBytes - wb.rangeLen;

               const size_t writeSize = static_cast<size_t>(
                   std::min<uint64_t>(
                       len - done,
                       available
                   )
               );

               const ssize_t written = pwrite(
                   fd_out,
                   buf + done,
                   writeSize,
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

               if (!paceWriteback(
                       fd_out,
                       fileOffset - writtenBytes,
                       writtenBytes,
                       wb)) {
                   return false;
               }
           }

           return true;
       }

       /**
        * @brief Copies exactly the expected file size in bounded chunks.
        *
        * Never reads beyond the expected ISO file size and fails if the
        * source reaches EOF before all expected bytes have been copied.
        *
        * Completes outstanding writeback on successful completion.
        * On failure or cancellation, skips the remaining writeback because
        * the caller discards the partially written destination file.
        *
        * @return true only if exactly the expected file size was copied
        *         and all tracked writeback completed successfully.
        */
       bool copyFileChunks(int fd_in, int fd_out,
                           uint64_t fileSize,
                           size_t bufferSize,
                           char* buf,
                           std::atomic<uint64_t>& totalAccumulator,
                           uint64_t& bytesCopiedTotal) {
           uint64_t fileOffset = 0;
           WritebackState wb;

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

               if (!writeOneChunk(
                       fd_out, buf, chunkSize,
                       fileOffset, bytesCopiedTotal,
                       totalAccumulator, wb)) {
                   success = false;
                   break;
               }
           }

           // On failure or cancellation the data is garbage: don't wait for it.
           // The caller truncates the file, which drops the dirty pages without I/O.
           if (!success || GlobalState::g_operationCancelled.load())
               return false;

           // Require an exact match, not merely a minimum.
           if (bytesCopiedTotal != fileSize)
               return false;

           // Success path only: complete the remaining partial writeback range.
           return flushWriteback(fd_out, wb);
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
                                 std::atomic<uint64_t>& totalAccumulator) {
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

           const bool success = copyFileChunks(
               fd_in, fd_out, fileSize, bufferSize, buf,
               totalAccumulator, bytesCopiedTotal
           );

           if (!success) {
               // Drops dirty pages without writing them and frees any preallocation.
               [[maybe_unused]] const int r = ftruncate(fd_out, 0);
           }

           posix_fadvise(
               fd_in, 0, 0, POSIX_FADV_DONTNEED
           );

           close(fd_in);
           close(fd_out);

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
                       std::atomic<uint64_t>& totalAccumulator) {

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

       // All parent directories now exist. Copy files only.
       for (const auto& e : entries) {
           if (GlobalState::g_operationCancelled.load())
               return false;

           if (e.isDir)
               continue;

           const bool toNtfs =
               (isoType == IsoType::WindowsInstall) &&
               e.dst.native().compare(0, ntfsMnt.size(), ntfsMnt) == 0;

           if (!copyFileWithProgress(
                   e.src, e.dst, e.size, toNtfs,
                   bufferSize, buf, totalAccumulator)) {
               return false;
           }
       }

       return true;
   }

} // namespace windowsWriter_engine::copy
