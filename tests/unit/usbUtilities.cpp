// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks ISO detection, CRC-64 verification, progress state and aligned buffers.
 */

#include "../common/testHarness.h"

import isocmd.write2usb.util.isoDetect;
import isocmd.write2usb.util.alignedBuffer;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.checksum;
import isocmd.core.cancellation;

namespace isocmd_tests {

	void isoDetection() {

		TempDir tmp;
		const auto path = tmp.path / "fixture.iso";
		const auto check = [&](const std::string& bytes, bool optical, bool windows) {

			{ std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(bytes.data(), bytes.size()); CHECK(out.good()); }
			ReadFd f(path);
			CHECK(iso_detect::isValidIso9660(f.fd) == optical);
			CHECK(iso_detect::isWindowsIsoInitialCheck(f.fd, static_cast<off_t>(bytes.size())) == windows);

		};
		check("tiny", false, false);
		std::string bytes(65536, '\0');
		check(bytes, false, false);
		bytes.replace(32768, 6, "\001CD001", 6);
		check(bytes, true, false);
		bytes.replace(40000, 8, "BOOT.WIM");
		check(bytes, true, false);
		bytes.replace(41000, 11, "BootMgr.EFI");
		check(bytes, true, true);
		bytes.replace(32768, 6, "\002CD001", 6);
		check(bytes, false, false);
		bytes.replace(32768, 6, "\000NSR03", 6);
		check(bytes, true, true);
		std::string udf(65536, '\0');
		udf.replace(32768, 6, "\000BEA01", 6);
		udf.replace(34816, 6, "\000NSR02", 6);
		check(udf, true, false);
		for (bool little : {true, false}) {
			std::string encoded(65536, '\0');
			encoded.replace(32768, 6, "\001CD001", 6);
			const auto encode = [little](std::string_view s) {

				std::string result;
				for (char c : s) { result += little ? c : '\0'; result += little ? '\0' : c; }
				return result;

			};
			encoded.replace(40000, 16, encode("BOOT.WIM"));
			encoded.replace(41000, 14, encode("bootmgr"));
			check(encoded, true, true);
		}

	}

	/**
	 * @brief Checks CRC-64 calculation and readback verification outcomes.
	 */
	void checksumVerification() {

		using namespace usb_verify;
		Checksum64 crc;
		crc.update("123456789", 9);
		CHECK(crc.value == 0x6C40DF5F0B497347ULL);
		Checksum64 split;
		split.update("1234", 4); split.update("56789", 5);
		CHECK(split.value == crc.value);
		CHECK(Checksum64{}.value == 0);

		TempDir tmp;
		const auto path = tmp.path / "payload";
		writeFile(path, "123456789");
		ProgressInfo row("test", "fixture", "9 B", "source");
		row.bytesWritten = 9;
		row.speed = 25.0;
		row.verificationSpeed = 100.0;
		char buffer[4];
		ReadbackProgress readback(row, 9);
		CHECK(row.verifying && row.progress == 0 && !row.completed);
		CHECK(row.speed == 25.0 && row.verificationSpeed == 0.0);
		CHECK(verifyFile(path, 9, crc.value, buffer, sizeof buffer, readback));
		CHECK(row.bytesVerified == 9 && row.bytesWritten == 9 && row.progress == 99 && !row.completed);
		CHECK(readback.finish());
		CHECK(row.completed && row.progress == 100 && row.speed == 25.0);

		ProgressInfo bad("test", "fixture", "9 B", "source");
		ReadbackProgress badProgress(bad, 9);
		writeFile(path, "123456788");
		CHECK(!verifyFile(path, 9, crc.value, buffer, sizeof buffer, badProgress));
		CHECK(!bad.completed && bad.progress < 100);
		writeFile(path, "12345678");
		CHECK(!verifyFile(path, 9, crc.value, buffer, sizeof buffer, badProgress));
		writeFile(path, "");
		CHECK(verifyFile(path, 0, 0, buffer, sizeof buffer, badProgress));
		CHECK(!verifyFile(path, 0, 1, buffer, sizeof buffer, badProgress));
		const auto link = tmp.path / "symlink";
		fs::create_symlink(path, link);
		CHECK(!verifyFile(link, 0, 0, buffer, sizeof buffer, badProgress));

		struct Reset { ~Reset() {

				GlobalState::g_operationCancelled = false;

			} } reset;

		GlobalState::g_operationCancelled = true;
		CHECK(!verifyFile(path, 0, 0, buffer, sizeof buffer, badProgress));

	}

	/**
	 * @brief Checks that readback cancellation survives resetting the UI cancellation flag.
	 */
	void latchedReadbackCancellation() {
		using namespace usb_verify;
		TempDir tmp;
		const auto path = tmp.path / "payload";
		writeFile(path, "123456789");
		ProgressInfo row("test", "fixture", "9 B", "source");
		std::stop_source stop;
		row.cancelToken = stop.get_token();
		row.bufferedWrites = true;
		ProgressInfo moved(std::move(row));
		row = std::move(moved);
		CHECK(row.cancelToken.stop_possible() && row.bufferedWrites == true);
		GlobalState::g_operationCancelled = true;
		stop.request_stop();
		GlobalState::g_operationCancelled = false;
		CHECK(row.cancelToken.stop_requested());
		ReadbackProgress cancelled(row, 9);
		char buffer[4];
		CHECK(!verifyFile(path, 9, 0x6C40DF5F0B497347ULL, buffer, sizeof buffer, cancelled));
		CHECK(!cancelled.finish() && row.bytesVerified == 0 && !row.completed);
		std::stop_source fresh;
		row.cancelToken = fresh.get_token();
		ReadbackProgress active(row, 9);
		CHECK(verifyFile(path, 9, 0x6C40DF5F0B497347ULL, buffer, sizeof buffer, active));
		CHECK(active.finish() && row.completed);
	}

	static void checkProgressRows(const std::vector<ProgressInfo>& progress) {

		for (int i = 0; i < 4096; ++i) {
			const auto& p = progress[i];
			CHECK(p.filename == std::to_string(i) && p.device == "fake-device" && p.sourcePath == "source.iso" && p.totalSize == "4 MB");
			CHECK(p.completed.load() == (i % 2 == 0) && p.failed.load() == (i % 3 == 0));
			CHECK(p.writeCompleted.load() == (i % 5 == 0));
			CHECK(p.verifyRequested == (i % 2 == 0) && p.verifying.load() == (i % 3 == 0) && p.bytesVerified.load() == i * 128u);
			CHECK(p.bytesWritten.load() == i * 4096u && p.progress.load() == i % 101 && p.speed.load() == i * 0.5);
			CHECK(p.verificationSpeed.load() == i * 0.25);
		}

	}

	static void checkProgressMove(std::vector<ProgressInfo>& progress) {

		ProgressInfo target("", "", "", "");
		target = std::move(progress.back());
		CHECK(target.filename == "4095" && target.bytesWritten.load() == 4095u * 4096u);
		CHECK(!target.verifyRequested && target.verifying && target.bytesVerified == 4095u * 128u);
		CHECK(target.writeCompleted && target.verificationSpeed == 4095 * 0.25);
		auto& alias = target; target = std::move(alias);
		CHECK(target.filename == "4095" && target.progress.load() == 4095 % 101);

	}

	void progressAndBuffer() {

		std::vector<ProgressInfo> progress;
		for (int i = 0; i < 4096; ++i) {
			progress.emplace_back(std::to_string(i), "fake-device", "4 MB", "source.iso");
			auto& p = progress.back();
			p.completed = i % 2 == 0; p.failed = i % 3 == 0;
			p.writeCompleted = i % 5 == 0;
			p.verifyRequested = i % 2 == 0; p.verifying = i % 3 == 0; p.bytesVerified = i * 128u;
			p.bytesWritten = i * 4096u; p.progress = i % 101; p.speed = i * 0.5; p.verificationSpeed = i * 0.25;
		}
		checkProgressRows(progress);
		checkProgressMove(progress);
		for (std::size_t alignment : {512, 4096}) {
			AlignedBuffer buffer(8193, alignment);
			CHECK(buffer.ok() && buffer.size == 8193);
			CHECK(reinterpret_cast<std::uintptr_t>(buffer.data) % alignment == 0);
			std::memset(buffer.data, 0x5a, buffer.size);
			CHECK(buffer.data[8192] == 0x5a);
		}

	}

} // namespace isocmd_tests
