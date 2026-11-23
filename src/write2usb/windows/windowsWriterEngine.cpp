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
        * @brief Per-file writeback pacing state: the chunks written since the last
        *        pacing point, and the range paced last time.
        */
       struct WritebackState {
           uint64_t pendOff    = 0;   ///< Start of the not-yet-paced range.
           uint64_t pendLen    = 0;   ///< Length of the not-yet-paced range.
           int      pendChunks = 0;   ///< Chunks accumulated in that range.
           uint64_t prevOff    = 0;   ///< Start of the range paced last time.
           uint64_t prevLen    = 0;   ///< Length of the range paced last time.
           bool     havePrev   = false;
       };

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
        * @brief Bound the dirty-page backlog of an output file to a few chunks,
        *        pacing once every kPaceEveryChunks chunks.
        *
        * Chunks are accumulated into one contiguous range. When it reaches
        * kPaceEveryChunks chunks, writeback of the whole range is started
        * (SYNC_FILE_RANGE_WRITE, does not block on the device), then the range
        * paced last time is waited on (WAIT_BEFORE | WRITE | WAIT_AFTER). The
        * device therefore always has the newest range queued while the previous
        * one drains, and a cancel never has more than about
        * kPaceEveryChunks + 1 chunks left to flush.
        *
        * Best effort: failures (e.g. a filesystem without support, or a cancel)
        * are ignored, as with the caller's final syncfs() flush; the copy then
        * degrades to plain buffered I/O.
        *
        * @param fd_out Output file descriptor.
        * @param off    File offset of the chunk just written.
        * @param len    Length in bytes of the chunk just written (must be > 0).
        * @param st     Pacing state for this output file.
        */
       void paceWriteback(int fd_out, uint64_t off, uint64_t len,
                          WritebackState& st) {
           if (st.pendChunks == 0) {
               st.pendOff = off;
               st.pendLen = 0;
           }

           st.pendLen += len;   // chunks are contiguous within a file

           if (++st.pendChunks < kPaceEveryChunks)
               return;

           static_cast<void>(syncFileRange(
               fd_out, st.pendOff, st.pendLen,
               SYNC_FILE_RANGE_WRITE
           ));

           if (st.havePrev) {
               static_cast<void>(syncFileRange(
                   fd_out, st.prevOff, st.prevLen,
                   SYNC_FILE_RANGE_WAIT_BEFORE |
                   SYNC_FILE_RANGE_WRITE |
                   SYNC_FILE_RANGE_WAIT_AFTER
               ));
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
        * write fails. Once the whole chunk has landed, writeback is paced
        * (see paceWriteback()) so the dirty backlog stays at a few chunks and
        * a cancel drains quickly.
        *
        * @param wb Writeback pacing state for this output file.
        * @return true if the entire chunk was written successfully.
        */
       bool writeOneChunk(int fd_out, const char* buf, size_t len,
                          uint64_t& fileOffset,
                          uint64_t& bytesCopiedTotal,
                          std::atomic<uint64_t>& totalAccumulator,
                          WritebackState& wb) {
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

           paceWriteback(fd_out, chunkOff, len, wb);
           return true;
       }

       /**
        * @brief Copies exactly the expected file size in bounded chunks.
        *
        * Never reads beyond the expected ISO file size and fails if the
        * source reaches EOF before all expected bytes have been copied.
        *
        * Writeback is paced per chunk by writeOneChunk(); no per-file flush
        * is done here. Any range still in flight when the file is closed is
        * covered by the caller's final syncfs().
        *
        * @return true only if exactly the expected file size was copied
        *         without failure or cancellation.
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

           // On failure or cancellation the data is garbage.
           // The caller truncates the file, which drops the dirty pages without I/O.
           if (!success || GlobalState::g_operationCancelled.load())
               return false;

           // Require an exact match, not merely a minimum.
           return bytesCopiedTotal == fileSize;
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
