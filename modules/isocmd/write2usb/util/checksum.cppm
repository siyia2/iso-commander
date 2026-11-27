// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Incremental CRC-64 checksums and USB readback verification interfaces.
 */

export module isocmd.write2usb.util.checksum;

import std;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.telemetry;

export extern "C++" {
namespace usb_verify {


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

	/**
	 * @brief Incremental CRC-64/ECMA-182 accumulator.
	 * Processes eight bytes at a time with slicing-by-eight tables, then handles
	 * any remaining bytes individually. Chunk boundaries do not affect the result.
	 */
	struct Checksum64 {
		std::uint64_t value = 0; ///< Running CRC, also the final value without extra XOR.

		/**
		 * @brief Appends bytes to the running CRC in input order.
		 * @param data Buffer readable for at least @p size bytes; no alignment required.
		 * @param size Number of bytes to include in the CRC.
		 */
		void update(const char* data, std::size_t size) noexcept {

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
	};

	/**
	 * @brief Publishes readback progress in an existing USB progress row.
	 *
	 * Restarts the displayed percentage for verification, while preserving write
	 * bytes and the write-only speed. A single worker owns this tracker; the UI
	 * reads the row's atomic fields. The referenced row must outlive the tracker.
	 * The caller validates the checksum before declaring success with finish().
	 */
	class ReadbackProgress {
	public:
		/**
		 * @brief Starts verification and resets the verification counters and speed.
		 * @param slot Progress row to update; must outlive this tracker.
		 * @param total Expected readback bytes across the complete verification phase.
		 */
		ReadbackProgress(ProgressInfo& slot, std::uint64_t total);

		/**
		 * @brief Accumulates bytes read and refreshes verification telemetry.
		 * @param bytes Newly read bytes, not a cumulative count.
		 * @note Percentage remains below 100 until finish() succeeds.
		 */
		void record(std::size_t bytes);

		/**
		 * @brief Marks the row complete after the expected byte count is read.
		 * Completion also requires that no cancellation is detected.
		 * @pre The caller has already validated all required checksums.
		 * @return true on completion; false on cancellation or byte-count mismatch.
		 * @note Does not compare checksums or clear the verifying flag itself.
		 */
		bool finish();

		/// Checks both global cancellation and the row's latched batch stop token.
		bool cancelled() const noexcept { return isUsbWriteCancelled(slot_.cancelToken); }

	private:
		ProgressInfo& slot_;                        ///< Borrowed UI progress row.
		std::uint64_t total_;                       ///< Expected aggregate byte count.
		std::uint64_t done_ = 0;                    ///< Bytes read so far.
		write_telemetry::SpeedEstimator estimator_; ///< Readback-only throughput estimate.
	};

	/**
	 * @brief Reads exactly @p size bytes and computes their CRC-64.
	 * @param fd Open readable descriptor, positioned at the start of the data to verify.
	 * @param size Number of bytes to read; must be a multiple of alignment.
	 * @param buffer Caller-owned read buffer, aligned to @p alignment.
	 * @param bufferSize Nonzero buffer capacity in bytes, a multiple of alignment.
	 * @param alignment Nonzero required alignment; 1 for ordinary buffered reads.
	 * @param progress Aggregate verification telemetry and cancellation tracker.
	 * @param[out] result CRC computed after all requested bytes are read.
	 * @return true on complete readback without cancellation; false on invalid
	 *         alignment, read failure, premature EOF or cancellation.
	 * @note Retries EINTR and accepts aligned short reads. Advances the descriptor
	 *       offset, does not close it and does not compare the CRC with an expected value.
	 *       A final cancellation check can fail after result has been assigned.
	 */
	bool readChecksum(int fd, std::uint64_t size, char* buffer, std::size_t bufferSize,
	                  std::size_t alignment, ReadbackProgress& progress, std::uint64_t& result);

	/**
	 * @brief Checks a regular file's exact length and CRC-64 against expected values.
	 * @param path Target file path; a symbolic link at the final component is rejected.
	 * @param size Expected file size in bytes.
	 * @param expected Expected CRC-64 of the file contents.
	 * @param buffer Caller-owned read buffer.
	 * @param bufferSize Nonzero buffer capacity in bytes.
	 * @param progress Aggregate verification telemetry and cancellation tracker.
	 * @return true if opening, validation, readback, comparison and close succeed;
	 *         false on failure or cancellation detected during readback.
	 * @note Uses buffered reads and does not validate parent path components,
	 *       filesystem metadata beyond type and size, or physical media persistence.
	 */
	bool verifyFile(const std::filesystem::path& path, std::uint64_t size, std::uint64_t expected,
	                char* buffer, std::size_t bufferSize, ReadbackProgress& progress);

} // namespace usb_verify
} // extern "C++"
