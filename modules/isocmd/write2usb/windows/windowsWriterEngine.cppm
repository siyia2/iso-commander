// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.windows.windowsWriterEngine;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/windows/windowsWriterEngine ----

// ------------------- Internal Windows writer implementation -------------------

namespace write2usb_detail {

	/**
	 * @brief Layout variants that drive partitioning and copy strategy.
	 */
	enum class IsoType {
		WindowsInstall,
		FatOnly
	};

	/**
	 * @brief A single file or directory discovered during the ISO tree walk.
	 */
	struct IsoCopyEntry {
		std::filesystem::path src;
		std::filesystem::path dst;
		std::uint64_t size;
		bool isDir;
	};

} // namespace write2usb_detail

namespace windowsWriter_engine::copy {

	inline constexpr std::size_t kBufferSize       = 4 * 1024 * 1024;
	inline constexpr std::size_t kFatBufferSize    = 4 * 1024 * 1024;
	inline constexpr std::size_t kBufferedSyncSize = 4 * 1024 * 1024;
	inline constexpr std::size_t kBufferAlign      = 4096;

	/**
	 * @brief Iterates one entry list, creating directories and copying files.
	 *
	 * Buffered writes are the default, bypassing direct I/O alignment and
	 * preallocation, with rolling 4 MiB sync_file_range writeback pacing.
	 * In direct mode, NTFS-bound files use aligned O_DIRECT with best-effort
	 * preallocation; FAT32-bound files use aligned O_DIRECT with bounded
	 * chunk initialization.
	 * Multi-chunk files overlap source reads and checksumming with ordered writes
	 * using two aligned buffers; single-chunk files remain synchronous.
	 *
	 * @param checksums Optional output of CRC-64 values, one per file in entry order.
	 * @param buffered Use buffered writes (default); false selects O_DIRECT.
	 * @return false on the first copy failure or cancellation; true otherwise.
	 */
	bool processEntries(const std::vector<write2usb_detail::IsoCopyEntry>& entries,
	                    write2usb_detail::IsoType isoType,
	                    const std::string& ntfsMnt,
	                    std::size_t bufferSize,
	                    char* buf,
	                    std::atomic<std::uint64_t>& totalAccumulator,
	                    std::vector<std::uint64_t>* checksums = nullptr,
	                    bool buffered = true,
	                    const std::stop_token& cancelToken = {});

} // namespace windowsWriter_engine::copy

} // extern "C++"
