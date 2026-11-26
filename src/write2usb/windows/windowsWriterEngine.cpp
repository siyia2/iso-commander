// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stddef.h>

import std;
import isocmd.core.cancellation;
import isocmd.write2usb.windows.windowsWriterEngine;
import isocmd.write2usb.util.checksum;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.alignedBuffer;

namespace fs = std::filesystem;

namespace windowsWriter_engine::copy {

	using IsoType = write2usb_detail::IsoType;
	using IsoCopyEntry = write2usb_detail::IsoCopyEntry;

	namespace {

		constexpr unsigned kMaxDirectWriteRestarts = 8;
		constexpr std::uint64_t kFatMaxFileSize = 0xffffffffULL;

		/**
		 * @brief Use the reported DIO alignment, independently of libc's statx
		 *        definition, with a conservative 4 KiB minimum and fallback.
		 *        Some drivers do not report alignment for resident files.
		 */
		std::size_t directAlignment(int fd) {

			constexpr unsigned kStatxDioAlign = 0x00002000U;

			// Linux statx UAPI: fixed 256-byte layout, including reserved fields.
			struct alignas(8) StatxDio {
				std::uint32_t mask;
				std::uint8_t reserved1[148];
				std::uint32_t memAlign;
				std::uint32_t offsetAlign;
				std::uint8_t reserved2[96];
			};

			static_assert(sizeof(StatxDio) == 256);
			static_assert(offsetof(StatxDio, mask) == 0);
			static_assert(offsetof(StatxDio, memAlign) == 152);
			static_assert(offsetof(StatxDio, offsetAlign) == 156);

			StatxDio info{};
			if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC,
			    kStatxDioAlign, &info) == 0 && (info.mask & kStatxDioAlign)) {
				return std::max({
				    kBufferAlign,
				    static_cast<std::size_t>(info.memAlign),
				    static_cast<std::size_t>(info.offsetAlign)
				});
			}

			return kBufferAlign;

		}

		/**
		 * @brief Fill a buffer, accumulating short reads and retrying EINTR.
		 *
		 * @return Bytes filled (0 = EOF), or -1 on error / cancellation.
		 */
		ssize_t fillChunk(int fd_in, char* buf, std::size_t bufferSize, const std::stop_token& cancelToken) {

			std::size_t total = 0;

			while (total < bufferSize) {
				if (isUsbWriteCancelled(cancelToken))
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

				total += static_cast<std::size_t>(n);
			}

			return static_cast<ssize_t>(total);

		}

		/**
		 * @brief Writes a logical chunk using positional I/O. In direct mode, the final
		 *        fragment is normally zero-padded by the caller.
		 *        After an unaligned direct short write, replay the same chunk
		 *        from its aligned start. Count logical bytes only once.
		 */
		bool writeOneChunk(int fd, const char* buf, std::size_t logicalLen,
		                   std::size_t writeLen, std::uint64_t chunkOff,
		                   std::size_t alignment,
		                   std::uint64_t& bytesCopiedTotal,
		                   std::atomic<std::uint64_t>& totalAccumulator,
		                   const std::stop_token& cancelToken) {

			std::size_t done = 0;
			std::size_t highWater = 0;
			unsigned restarts = 0;

			while (done < writeLen) {
				if (isUsbWriteCancelled(cancelToken))
				    return false;

				const ssize_t written = pwrite(fd, buf + done, writeLen - done,
				    static_cast<off_t>(chunkOff + done));

				if (written < 0) {
					if (errno == EINTR) continue;
					return false;
				}

				if (written == 0 || static_cast<std::size_t>(written) > writeLen - done) {
					errno = EIO;
					return false;
				}

				done += static_cast<std::size_t>(written);
				const std::size_t logicalDone = std::min(done, logicalLen);
				if (logicalDone > highWater) {
					const auto delta = logicalDone - highWater;
					bytesCopiedTotal += delta;
					totalAccumulator.fetch_add(delta);
					highWater = logicalDone;
				}

				if (alignment > 1 && done < writeLen && done % alignment != 0) {
					if (restarts == kMaxDirectWriteRestarts) {
						errno = EIO;
						return false;
					}
					++restarts;
					done = 0;
				}
			}

			return true;

		}

		/** Retry interrupted truncation both for exact tails and failed copies. */
		bool truncateFile(int fd, std::uint64_t size) {

			int result;
			do {
				result = ftruncate(fd, static_cast<off_t>(size));
			} while (result < 0 && errno == EINTR);
			return result == 0;

		}

		/** Select a chunk that can be aligned without crossing the FAT32 size limit. */
		std::size_t logicalChunkSize(std::uint64_t offset, std::uint64_t fileSize,
		                             std::size_t capacity, std::size_t alignment, bool toNtfs) {

			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(fileSize - offset, capacity));
			const auto limit = kFatMaxFileSize & ~static_cast<std::uint64_t>(alignment - 1);
			if (!toNtfs && offset + count > limit && count >= alignment)
			    return count - count % alignment;
			return count;

		}

		/** Remove final padding and drain any dirty FAT32 tail page. */
		bool finishDirectFile(int fd, std::uint64_t fileSize, std::size_t alignment, bool toNtfs,
		                      const std::stop_token& cancelToken) {

			const auto tail = fileSize % alignment;
			if (!tail) return true;
			if (!truncateFile(fd, fileSize)) return false;
			if (toNtfs) return true;
			if (isUsbWriteCancelled(cancelToken)) return false;

			int result;
			do {
				result = sync_file_range(fd, static_cast<off_t>(fileSize - tail),
				    static_cast<off_t>(tail),
				    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
			} while (result < 0 && errno == EINTR && !isUsbWriteCancelled(cancelToken));
			return result == 0;

		}

		/** A rolling 4 MiB writeback window shared across buffered files. */
		class BufferedWriteback {
		public:
			explicit BufferedWriteback(const std::stop_token& token) : cancelToken(token) {}
			BufferedWriteback(const BufferedWriteback&) = delete;
			BufferedWriteback& operator=(const BufferedWriteback&) = delete;

			~BufferedWriteback() {

				if (fd_ >= 0) close(fd_);

			}

			/** Submit full ranges and the final nonempty tail of a completed file. */
			bool pace(int fd, std::uint64_t end, std::uint64_t fileSize,
			          std::uint64_t& offset) {

				while (end - offset >= kBufferedSyncSize || (end == fileSize && end > offset)) {
					const auto len = std::min<std::uint64_t>(end - offset, kBufferedSyncSize);
					if (!submit(fd, offset, len)) return false;
					offset += len;
				}
				return true;

			}

		private:
			static constexpr unsigned waitFlags = SYNC_FILE_RANGE_WAIT_BEFORE |
			    SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER;

			bool syncRange(int fd, std::uint64_t off, std::uint64_t len, unsigned flags) {

				for (;;) {
					if (isUsbWriteCancelled(cancelToken)) {
						errno = ECANCELED;
						return false;
					}
					if (sync_file_range(fd, static_cast<off_t>(off),
					    static_cast<off_t>(len), flags) == 0) return true;
					if (errno != EINTR) return false;
				}

			}

			/** Start the new range before waiting for the preceding range.
			 *  Keep one descriptor alive across file boundaries. Never drain on
			 *  destruction: cancellation must not introduce another explicit wait.
			 */
			bool submit(int fd, std::uint64_t off, std::uint64_t len) {

				const int held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
				if (held < 0) return false;

				if (!syncRange(held, off, len, SYNC_FILE_RANGE_WRITE) ||
				    (fd_ >= 0 && !syncRange(fd_, off_, len_, waitFlags))) {
					const int saved = errno;
					close(held);
					errno = saved;
					return false;
				}

				if (fd_ >= 0) close(fd_);
				fd_ = held;
				off_ = off;
				len_ = len;
				return true;

			}

			const std::stop_token& cancelToken;
			int fd_ = -1;
			std::uint64_t off_ = 0, len_ = 0;
		};

		struct CopyContext {
			int fd_in;
			int fd_out;
			std::uint64_t fileSize;
			bool toNtfs;
			std::size_t bufferSize;
			std::size_t alignment; // 1 for buffered I/O; otherwise the direct I/O alignment.
			std::atomic<std::uint64_t>& totalAccumulator;
			usb_verify::Checksum64* checksum;
			BufferedWriteback* writeback; // Null for direct I/O.
			const std::stop_token& cancelToken;
		};

		struct CopyBuffers {
			char* primary;
			char* spare = nullptr;
		};

		struct CopyChunk {
			char* buf;
			std::uint64_t offset = 0;
			std::size_t logicalLen = 0;
			std::size_t writeLen = 0;
			bool exactTail = false;
		};

		/** Read and hash logical bytes in source order; pad only for direct I/O. */
		bool prepareChunk(const CopyContext& ctx, CopyChunk& chunk) {

			chunk.logicalLen = logicalChunkSize(chunk.offset, ctx.fileSize, ctx.bufferSize, ctx.alignment, ctx.toNtfs);
			// At the FAT32 limit, padding would exceed the maximum file size.
			chunk.exactTail = !ctx.toNtfs && chunk.offset + chunk.logicalLen >
			    (kFatMaxFileSize & ~static_cast<std::uint64_t>(ctx.alignment - 1));

			const ssize_t n = fillChunk(ctx.fd_in, chunk.buf, chunk.logicalLen, ctx.cancelToken);
			if (n < 0) return false;
			if (static_cast<std::size_t>(n) != chunk.logicalLen) {
				errno = EIO;
				return false;
			}

			if (ctx.checksum) ctx.checksum->update(chunk.buf, chunk.logicalLen);
			const auto remainder = chunk.logicalLen % ctx.alignment;
			chunk.writeLen = remainder && !chunk.exactTail
			    ? chunk.logicalLen + ctx.alignment - remainder : chunk.logicalLen;
			if (chunk.writeLen != chunk.logicalLen)
			    std::memset(chunk.buf + chunk.logicalLen, 0, chunk.writeLen - chunk.logicalLen);
			return true;

		}

		/** One reader thread per multi-chunk file, with at most one outstanding read.
		 *  The semaphore handoffs transfer ownership of the spare buffer. On any
		 *  exit, drain the outstanding read and join before closing either descriptor.
		 */
		class ChunkReader {
		public:
			explicit ChunkReader(const CopyContext& ctx) : ctx_(ctx), worker_([this] {
				for (;;) {
					request_.acquire();
					if (!chunk_) return;
					success_ = prepareChunk(ctx_, *chunk_);
					error_ = errno;
					ready_.release();
				}
			}) {}

			~ChunkReader() {

				const int saved = errno;
				if (pending_) ready_.acquire();
				chunk_ = nullptr;
				request_.release();
				worker_.join();
				errno = saved;

			}

			void submit(CopyChunk& chunk) {

				chunk_ = &chunk;
				pending_ = true;
				request_.release();

			}

			bool wait() {

				ready_.acquire();
				pending_ = false;
				if (!success_) errno = error_;
				return success_;

			}

		private:
			const CopyContext& ctx_;
			std::binary_semaphore request_{0}, ready_{0};
			CopyChunk* chunk_ = nullptr;
			bool pending_ = false;
			bool success_ = false;
			int error_ = 0;
			std::thread worker_;
		};

		/** Grow only the current FAT32 chunk, never the prefetched one. */
		bool writePreparedChunk(const CopyContext& ctx, const CopyChunk& chunk,
		                        std::uint64_t& bytesCopiedTotal) {

			if (isUsbWriteCancelled(ctx.cancelToken)) return false;
			if (ctx.alignment > 1 && !ctx.toNtfs && !chunk.exactTail &&
			    !truncateFile(ctx.fd_out, chunk.offset + chunk.writeLen))
			    return false;

			return writeOneChunk(ctx.fd_out, chunk.buf, chunk.logicalLen, chunk.writeLen,
			    chunk.offset, ctx.alignment, bytesCopiedTotal, ctx.totalAccumulator, ctx.cancelToken);

		}

		/** Overlap one source chunk with one destination write; keep writes ordered. */
		bool copyPreparedChunks(const CopyContext& ctx, CopyChunk& current, CopyChunk& next,
		                        ChunkReader* reader, std::uint64_t& bytesCopiedTotal) {

			std::uint64_t rangeOffset = 0;
			for (;;) {
				if (isUsbWriteCancelled(ctx.cancelToken)) return false;
				const auto nextOffset = current.offset + current.logicalLen;
				const bool hasNext = nextOffset < ctx.fileSize;
				if (hasNext) {
					next.offset = nextOffset;
					reader->submit(next);
				}

				if (!writePreparedChunk(ctx, current, bytesCopiedTotal)) return false;
				if (ctx.writeback && !ctx.writeback->pace(ctx.fd_out, nextOffset, ctx.fileSize, rangeOffset))
				    return false;
				if (!hasNext) return true;
				if (!reader->wait()) return false;
				std::swap(current, next);
			}

		}

		/** Prepare the first chunk and start a worker only for multi-chunk files. */
		bool copyFileChunks(const CopyContext& ctx, const CopyBuffers& buffers) {

			if (isUsbWriteCancelled(ctx.cancelToken)) return false;
			if (ctx.fileSize == 0) return true;

			std::uint64_t bytesCopiedTotal = 0;
			CopyChunk current{buffers.primary}, next{buffers.spare};
			if (!prepareChunk(ctx, current)) return false;

			std::optional<ChunkReader> reader;
			if (buffers.spare) {
				try {
					reader.emplace(ctx);
				} catch (const std::system_error& e) {
					errno = e.code().value();
					return false;
				}
			}

			if (!copyPreparedChunks(ctx, current, next, reader ? &*reader : nullptr, bytesCopiedTotal))
			    return false;

			reader.reset();
			if (isUsbWriteCancelled(ctx.cancelToken) || bytesCopiedTotal != ctx.fileSize)
			    return false;

			return ctx.alignment == 1 ||
			       finishDirectFile(ctx.fd_out, ctx.fileSize, ctx.alignment, ctx.toNtfs, ctx.cancelToken);

		}

		/** Validate the buffer geometry and the padded destination size. */
		bool validCopyGeometry(const CopyContext& ctx, const char* buf, std::uint64_t padding) {

			const auto maxOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
			if (ctx.bufferSize == 0 || !std::has_single_bit(ctx.alignment) ||
			    ctx.bufferSize % ctx.alignment != 0 || ctx.fileSize > maxOffset - padding || !buf) {
				errno = EINVAL;
				return false;
			}
			return true;

		}

		/** Allocate a replacement only when the supplied buffer is unaligned. */
		bool preparePrimaryBuffer(const CopyContext& ctx, CopyBuffers& buffers,
		                          std::optional<AlignedBuffer>& scratch) {

			if (reinterpret_cast<std::uintptr_t>(buffers.primary) % ctx.alignment != 0) {
				scratch.emplace(ctx.bufferSize, ctx.alignment);
				buffers.primary = scratch->data;
			}
			if (!buffers.primary) {
				errno = ENOMEM;
				return false;
			}
			return true;

		}

		/** Lazily allocate the second buffer and reuse it for subsequent large files. */
		bool prepareSpareBuffer(const CopyContext& ctx, CopyBuffers& buffers,
		                        std::optional<AlignedBuffer>& spareStorage) {

			if (ctx.fileSize <= logicalChunkSize(0, ctx.fileSize, ctx.bufferSize, ctx.alignment, ctx.toNtfs))
			    return true;

			if (!spareStorage || !spareStorage->data || spareStorage->size < ctx.bufferSize ||
			    reinterpret_cast<std::uintptr_t>(spareStorage->data) % ctx.alignment != 0)
			    spareStorage.emplace(ctx.bufferSize, std::max(ctx.alignment, kBufferAlign));
			buffers.spare = spareStorage->data;
			if (!buffers.spare) {
				errno = ENOMEM;
				return false;
			}
			return true;

		}

		/** Prepare the buffers and direct-mode NTFS preallocation before copying. */
		bool copyOpenedFile(const CopyContext& ctx, char* buf,
		                    std::optional<AlignedBuffer>& spareStorage) {

			if (!ctx.toNtfs && ctx.fileSize > kFatMaxFileSize) {
				errno = EFBIG;
				return false;
			}
			const auto padding = (ctx.alignment - ctx.fileSize % ctx.alignment) % ctx.alignment;
			if (!validCopyGeometry(ctx, buf, padding)) return false;

			CopyBuffers buffers{buf};
			std::optional<AlignedBuffer> scratch;
			if (!preparePrimaryBuffer(ctx, buffers, scratch)) return false;
			if (isUsbWriteCancelled(ctx.cancelToken)) return false;
			if (!prepareSpareBuffer(ctx, buffers, spareStorage)) return false;
			if (isUsbWriteCancelled(ctx.cancelToken)) return false;

			if (ctx.alignment > 1 && ctx.toNtfs && ctx.fileSize > 0)
			    posix_fallocate(ctx.fd_out, 0, static_cast<off_t>(ctx.fileSize + padding));

			return copyFileChunks(ctx, buffers);

		}

		/**
		 * @brief Direct mode uses O_DIRECT on both filesystems, with NTFS preallocation
		 *        and incremental FAT32 growth. Buffered mode writes exact logical bytes
		 *        with rolling 4 MiB writeback pacing.
		 *        The caller's final syncfs flushes data and metadata in both modes.
		 */
		bool copyFileWithProgress(const IsoCopyEntry& entry, bool toNtfs, BufferedWriteback* writeback,
		                          std::size_t bufferSize, char* buf,
		                          std::optional<AlignedBuffer>& spareStorage,
		                          std::atomic<std::uint64_t>& totalAccumulator,
		                          std::uint64_t* expectedChecksum,
		                          const std::stop_token& cancelToken) {

			if (isUsbWriteCancelled(cancelToken)) return false;

			const int fd_in = open(entry.src.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd_in < 0) return false;

			posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);
			if (isUsbWriteCancelled(cancelToken)) { close(fd_in); return false; }

			const int fd_out = open(entry.dst.c_str(),
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (writeback ? 0 : O_DIRECT), 0644);

			if (fd_out < 0) {
				close(fd_in);
				return false;
			}

			usb_verify::Checksum64 checksum;
			const CopyContext ctx{fd_in, fd_out, entry.size, toNtfs, bufferSize,
			    writeback ? 1 : directAlignment(fd_out), totalAccumulator,
			    expectedChecksum ? &checksum : nullptr, writeback, cancelToken};
			bool success = copyOpenedFile(ctx, buf, spareStorage);

			if (!success || isUsbWriteCancelled(cancelToken)) {
				// Discard incomplete file data and release preallocation.
				// A cleanup error must not turn a failed copy into success.
				static_cast<void>(truncateFile(fd_out, 0));
				success = false;
			}

			posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);
			close(fd_in);
			if (close(fd_out) != 0) success = false;

			if (success && expectedChecksum) *expectedChecksum = checksum.value;
			return success && !isUsbWriteCancelled(cancelToken);

		}

		/** Collect, deduplicate and create all required directories before copying. */
		bool prepareDirectories(const std::vector<IsoCopyEntry>& entries, const std::stop_token& cancelToken) {

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
				if (isUsbWriteCancelled(cancelToken))
				    return false;

				fs::create_directories(dir);
			}

			return true;

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
	                    std::size_t bufferSize,
	                    char* buf,
	                    std::atomic<std::uint64_t>& totalAccumulator,
	                    std::vector<std::uint64_t>* checksums,
	                    bool buffered,
	                    const std::stop_token& cancelToken) {

		if (checksums) {
			checksums->clear();
			checksums->reserve(entries.size());
		}

		if (isUsbWriteCancelled(cancelToken) || !prepareDirectories(entries, cancelToken)) return false;

		// All parent directories now exist. Copy files only.
		const auto ntfsPrefix = ntfsMnt + "/";
		std::optional<AlignedBuffer> spareStorage;
		BufferedWriteback writeback(cancelToken);
		for (const auto& e : entries) {
			if (isUsbWriteCancelled(cancelToken))
			    return false;

			if (e.isDir)
			    continue;

			const bool toNtfs =
			    (isoType == IsoType::WindowsInstall) && !ntfsMnt.empty() &&
			    e.dst.native().starts_with(ntfsPrefix);

			std::uint64_t checksum = 0;
			if (!copyFileWithProgress(
			        e, toNtfs, buffered ? &writeback : nullptr,
			        bufferSize, buf, spareStorage, totalAccumulator,
			        checksums ? &checksum : nullptr, cancelToken)) {
				return false;
			}
			if (checksums) checksums->push_back(checksum);
		}

		return true;

	}

} // namespace windowsWriter_engine::copy
