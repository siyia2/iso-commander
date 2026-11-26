// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.checksum;

import std;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.telemetry;

export extern "C++" {
namespace usb_verify {

	// CRC-64/ECMA-182: polynomial 0x42F0E1EBA9EA3693, init/xorout 0,
	// non-reflected. Intended for detecting accidental corruption, not authenticity.
	inline constexpr auto crcTable = [] {

		std::array<std::uint64_t, 256> table{};
		for (std::size_t i = 0; i < table.size(); ++i) {
			std::uint64_t value = static_cast<std::uint64_t>(i) << 56;
			for (int bit = 0; bit < 8; ++bit)
			    value = (value << 1) ^ ((value >> 63) ? 0x42F0E1EBA9EA3693ULL : 0);
			table[i] = value;
		}
		return table;

	}();

	struct Checksum64 {
		std::uint64_t value = 0;

		void update(const char* data, std::size_t size) noexcept {

			for (std::size_t i = 0; i < size; ++i)
			    value = (value << 8) ^ crcTable[(value >> 56) ^ static_cast<unsigned char>(data[i])];

		}
	};

	// Reuse the existing row and speed estimator, but keep write bytes and speed separate.
	class ReadbackProgress {
	public:
		ReadbackProgress(ProgressInfo& slot, std::uint64_t total);
		void record(std::size_t bytes);
		bool finish();
	private:
		ProgressInfo& slot_;
		std::uint64_t total_;
		std::uint64_t done_ = 0;
		write_telemetry::SpeedEstimator estimator_;
	};

	// Reads exactly size bytes, using the caller's aligned buffer for raw devices.
	// For direct I/O, alignment > 1; unaligned short reads fail safely.
	bool readChecksum(int fd, std::uint64_t size, char* buffer, std::size_t bufferSize,
	                  std::size_t alignment, ReadbackProgress& progress, std::uint64_t& result);

	// Checks a regular file's exact length and CRC; symlinks are rejected.
	bool verifyFile(const std::filesystem::path& path, std::uint64_t size, std::uint64_t expected,
	                char* buffer, std::size_t bufferSize, ReadbackProgress& progress);

} // namespace usb_verify
} // extern "C++"
