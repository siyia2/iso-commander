// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Readback telemetry, descriptor checksumming and regular-file CRC validation.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import isocmd.write2usb.util.checksum;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.telemetry;

namespace usb_verify {

	namespace {
		/**
		 * @brief Compile-time byte lookup table for CRC-64/ECMA-182.
		 * Polynomial 0x42F0E1EBA9EA3693, initial value and final XOR zero,
		 * non-reflected input and output. Detects accidental corruption, not authenticity.
		 */
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

		/// Eight slicing tables; each successive slice advances the previous by one zero byte.
		inline constexpr auto crcSlices = [] {

			std::array<std::array<std::uint64_t, 256>, 8> tables{};
			tables[0] = crcTable;
			for (std::size_t slice = 1; slice < tables.size(); ++slice) {
				for (std::size_t i = 0; i < tables[slice].size(); ++i) {
					const auto previous = tables[slice - 1][i];
					tables[slice][i] = (previous << 8) ^ tables[0][previous >> 56];
				}
			}
			return tables;

		}();
	} // namespace

	void Checksum64::update(const char* data, std::size_t size) noexcept {

		const auto* bytes = reinterpret_cast<const unsigned char*>(data);
		while (size >= 8) {
			// Byte loads preserve input order on all endiannesses and need no alignment.
			value = crcSlices[7][(value >> 56) ^ bytes[0]] ^
			        crcSlices[6][((value >> 48) & 0xFF) ^ bytes[1]] ^
			        crcSlices[5][((value >> 40) & 0xFF) ^ bytes[2]] ^
			        crcSlices[4][((value >> 32) & 0xFF) ^ bytes[3]] ^
			        crcSlices[3][((value >> 24) & 0xFF) ^ bytes[4]] ^
			        crcSlices[2][((value >> 16) & 0xFF) ^ bytes[5]] ^
			        crcSlices[1][((value >> 8) & 0xFF) ^ bytes[6]] ^
			        crcSlices[0][(value & 0xFF) ^ bytes[7]];
			bytes += 8;
			size -= 8;
		}
		for (std::size_t i = 0; i < size; ++i)
		    value = (value << 8) ^ crcSlices[0][(value >> 56) ^ bytes[i]];

	}

	bool ReadbackProgress::cancelled() const noexcept {

		return isUsbWriteCancelled(slot_.cancelToken);

	}

	/// Switches the existing progress row to verification telemetry.
	ReadbackProgress::ReadbackProgress(ProgressInfo& slot, std::uint64_t total)
	    : slot_(slot), total_(total),
	      estimator_(write_telemetry::Clock::now(), write_telemetry::windowsStallTimeoutSeconds) {

		slot_.progress.store(0);
		slot_.verificationSpeed.store(0);
		slot_.bytesVerified.store(0);
		slot_.verifying.store(true);

	}

	/// Updates cumulative readback bytes, percentage and sampled speed.
	void ReadbackProgress::record(std::size_t bytes) {

		done_ += bytes;
		slot_.bytesVerified.store(done_);
		if (total_) slot_.progress.store(write_telemetry::transferPercent(done_, total_));
		if (auto speed = estimator_.sample(done_, write_telemetry::Clock::now()))
		    slot_.verificationSpeed.store(*speed);

	}

	/// Publishes successful completion after caller-side checksum validation.
	bool ReadbackProgress::finish() {

		if (done_ != total_ || cancelled()) return false;
		slot_.progress.store(100);
		slot_.completed.store(true);
		return true;

	}

	/**
	 * @brief Computes a CRC over the requested descriptor range using repeated reads.
	 * Validates buffer and transfer alignment before reading. Checks cancellation
	 * between reads and retries interrupted system calls without double-counting bytes.
	 */
	bool readChecksum(int fd, std::uint64_t size, char* buffer, std::size_t bufferSize,
	                  std::size_t alignment, ReadbackProgress& progress, std::uint64_t& result) {

		if (!buffer || !bufferSize || !alignment ||
		    bufferSize % alignment || size % alignment ||
		    reinterpret_cast<std::uintptr_t>(buffer) % alignment)
		    return false;
		Checksum64 checksum;
		std::uint64_t done = 0;
		while (done < size) {
			if (progress.cancelled()) return false;
			const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, bufferSize));
			const ssize_t n = read(fd, buffer, count);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0 || static_cast<std::size_t>(n) > count || static_cast<std::size_t>(n) % alignment)
			    return false;
			checksum.update(buffer, static_cast<std::size_t>(n));
			done += static_cast<std::size_t>(n);
			progress.record(static_cast<std::size_t>(n));
		}
		result = checksum.value;
		return !progress.cancelled();

	}

	/**
	 * @brief Opens a target file without following a final-component symlink.
	 * Checks type and exact length before readback; a close failure also fails verification.
	 */
	bool verifyFile(const std::filesystem::path& path, std::uint64_t size, std::uint64_t expected,
	                char* buffer, std::size_t bufferSize, ReadbackProgress& progress) {

		if (progress.cancelled()) return false;
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (fd < 0) return false;
		struct stat st{};
		std::uint64_t actual = 0;
		bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
		          static_cast<std::uint64_t>(st.st_size) == size &&
		          readChecksum(fd, size, buffer, bufferSize, 1, progress, actual) && actual == expected;
		if (close(fd) != 0) ok = false;
		return ok;

	}
} // namespace usb_verify
