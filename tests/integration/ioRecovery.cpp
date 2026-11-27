// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Exercises Windows copy-engine I/O recovery, write modes and cancellation using injected faults.
 */

#include "../common/ioFaults.h"

import isocmd.core.cancellation;
import isocmd.write2usb.windows.windowsWriterEngine;
import isocmd.write2usb.util.checksum;
import isocmd.write2usb.util.writeTypes;

namespace isocmd_tests {

	namespace {
		using namespace io_faults;
		using write2usb_detail::IsoCopyEntry;
		using write2usb_detail::IsoType;
		using windowsWriter_engine::copy::processEntries;

		struct CopyFixture {
			TempDir tmp;
			fs::path src = tmp.path / "source", dst = tmp.path / "target";
			std::string data = std::string(10003, '\0');
			std::vector<char> buffer = std::vector<char>(4096);

			CopyFixture() {

				for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i % 251);
				writeFile(src, data);
				writeFile(dst, "sentinel");

			}

			void run(Plan& plan, bool success, std::uint64_t expectedBytes, bool ntfs = false, std::size_t remaining = 0, bool buffered = false) {

				struct Reset { ~Reset() {

						GlobalState::g_operationCancelled = false;

					} } reset;

				GlobalState::g_operationCancelled = false;
				std::atomic<std::uint64_t> total{0};
				bool result;
				std::vector<std::uint64_t> checksums;
				{
					Scope scope(plan);
					result = processEntries({{src, dst, data.size(), false}},
					    ntfs ? IsoType::WindowsInstall : IsoType::FatOnly,
					    ntfs ? tmp.path.string() : "", buffer.size(), buffer.data(), total, &checksums, buffered);
				}
				CHECK(result == success);
				if (success) {
					usb_verify::Checksum64 expected;
					expected.update(data.data(), data.size());
					CHECK(checksums == std::vector<std::uint64_t>{expected.value});
				}
				for (const auto& rule : plan.rules) CHECK(rule.consumed());
				CHECK(total == expectedBytes);
				CHECK(readFile(src) == data);
				CHECK(readFile(dst) == (success ? data : data.substr(0, remaining)));

			}
		};

		void cancel() {

			GlobalState::g_operationCancelled = true;

		}
	} // namespace

	// Rendezvous inside the second read and first write. A serial copy cannot
	// satisfy both callbacks without hitting the deadline.
	namespace {
		std::atomic<bool> readStarted{false}, writeStarted{false}, overlapTimedOut{false};

		void waitForOverlap(const std::atomic<bool>& flag) {

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!flag.load()) {
				if (std::chrono::steady_clock::now() >= deadline) {
					overlapTimedOut = true;
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

		}

		void observeRead() {

			readStarted = true;
			waitForOverlap(writeStarted);

		}

		void observeWrite() {

			writeStarted = true;
			waitForOverlap(readStarted);

		}
	}

	/**
	 * @brief Checks read/write overlap and worker cleanup in both copy modes.
	 */
	void windowsDoubleBuffering() {

		for (const bool buffered : {false, true}) {
			for (const bool ntfs : {false, true}) {
				CopyFixture f;
				ReadFd source(f.src), target(f.dst);
				const Identity input(source.fd), output(target.fd);
				readStarted = false;
				writeStarted = false;
				overlapTimedOut = false;
				Plan p;
				p[Call::Read].set(input, {{}, observeBefore(observeRead), {}});
				p[Call::Pwrite].set(output, {observeBefore(observeWrite), {}, {}});
				f.run(p, true, f.data.size(), ntfs, 0, buffered);
				CHECK(readStarted && writeStarted && !overlapTimedOut);
				CHECK(p[Call::Read].calls.load() == 3);
				CHECK(p[Call::Pwrite].calls.load() == 3);

				// A failed write must join its prefetched read before cleanup, and
				// a new copy must be able to run immediately afterwards.
				Plan failure;
				failure[Call::Pwrite].set(output, {error(EIO)});
				f.run(failure, false, 0, ntfs, 0, buffered);
				Plan clean;
				f.run(clean, true, f.data.size(), ntfs, 0, buffered);
			}
		}

	}

	// Stall a write, latch cancellation and reset the UI flag before it returns.
	namespace {
		std::binary_semaphore blockedWrite{0}, resumeWrite{0};

		void blockWrite() {
			blockedWrite.release();
			resumeWrite.acquire();
		}

		void checkLatchedWindowsCancellation(bool buffered) {
			CopyFixture f;
			const auto second = f.tmp.path / "second";
			ReadFd target(f.dst);
			Plan plan;
			plan[Call::Pwrite].set(Identity(target.fd), {observeBefore(blockWrite)});
			std::atomic<std::uint64_t> total{0};
			std::stop_source cancellation;
			const auto token = cancellation.get_token();
			const std::vector<IsoCopyEntry> entries{
			    {f.src, f.dst, 4096, false}, {f.src, second, 4096, false}};
			writeFile(f.src, f.data.substr(0, 4096));
			GlobalState::g_operationCancelled = false;
			{
				Scope scope(plan);
				auto task = std::async(std::launch::async, [&] {
					return processEntries(entries, IsoType::FatOnly, "", f.buffer.size(),
					    f.buffer.data(), total, nullptr, buffered, token);
				});
				const bool entered = blockedWrite.try_acquire_for(std::chrono::seconds(5));
				GlobalState::g_operationCancelled = true;
				cancellation.request_stop();
				GlobalState::g_operationCancelled = false;
				resumeWrite.release();
				const bool success = task.get();
				CHECK(entered && !success);
			}
			CHECK(total == 4096 && !fs::exists(second));
			CHECK(plan[Call::Pwrite].calls.load() == 1);
			std::stop_source nextBatch;
			total = 0;
			CHECK(processEntries(entries, IsoType::FatOnly, "", f.buffer.size(),
			    f.buffer.data(), total, nullptr, buffered, nextBatch.get_token()));
			CHECK(total == 8192 && readFile(second) == readFile(f.src));
		}
	}

	/**
	 * @brief Checks that Windows copy cancellation survives a UI flag reset.
	 */
	void windowsLatchedCancellation() {
		for (bool buffered : {false, true}) checkLatchedWindowsCancellation(buffered);
	}

	/**
	 * @brief Checks cancellation latched while a verification read is in flight.
	 */
	void readbackLatchedDuringIo() {
		TempDir tmp;
		const auto path = tmp.path / "payload";
		writeFile(path, "123456789");
		ReadFd target(path);
		Plan plan;
		plan[Call::Read].set(Identity(target.fd), {observeBefore(blockWrite)});
		std::stop_source cancellation;
		ProgressInfo row("test", "fixture", "9 B", "source");
		row.cancelToken = cancellation.get_token();
		usb_verify::ReadbackProgress readback(row, 9);
		char buffer[4];
		GlobalState::g_operationCancelled = false;
		{
			Scope scope(plan);
			auto task = std::async(std::launch::async, [&] {
				return usb_verify::verifyFile(path, 9, 0x6C40DF5F0B497347ULL,
				    buffer, sizeof buffer, readback);
			});
			const bool entered = blockedWrite.try_acquire_for(std::chrono::seconds(5));
			GlobalState::g_operationCancelled = true;
			cancellation.request_stop();
			GlobalState::g_operationCancelled = false;
			resumeWrite.release();
			const bool success = task.get();
			CHECK(entered && !success);
		}
		CHECK(plan[Call::Read].calls.load() == 1 && row.bytesVerified == sizeof buffer);
		CHECK(!readback.finish() && !row.completed);
	}

	/**
	 * @brief Checks recoverable syscall faults and short transfers in the Windows copy engine.
	 */
	void windowsIoRecovery() {

		CopyFixture f;
		ReadFd source(f.src), target(f.dst);
		const Identity input(source.fd), output(target.fd);
		{
			Plan p;
			p[Call::Read].set(input, {error(EINTR), shortTransfer(137), shortTransfer(901)});
			f.run(p, true, f.data.size());
		}
		{
			Plan p;
			p[Call::Pwrite].set(output, {error(EINTR), unalignedDirectShort(4096), {}});
			f.run(p, true, f.data.size());
		}
		{
			Plan p;
			p[Call::Pwrite].set(output, {error(EINTR), unalignedDirectShort(4096), {}});
			f.run(p, true, f.data.size(), true);
			CHECK(p[Call::Pwrite].effects.load() == 1);
		}
		{
			// Preallocation is an optimization, not a requirement for copying.
			Plan p;
			p[Call::Fallocate].set(output, {error(EOPNOTSUPP)});
			f.run(p, true, f.data.size(), true);
		}

	}

	/**
	 * @brief Checks failed copy accounting, partial output cleanup and subsequent recovery.
	 */
	void windowsIoFailures() {

		CopyFixture f;
		ReadFd source(f.src), target(f.dst);
		const Identity input(source.fd), output(target.fd);
		for (const auto fault : {error(EIO), zero()}) {
			Plan p;
			// The first full chunk lands; the incomplete second chunk must not.
			p[Call::Read].set(input, {{}, shortTransfer(137), fault});
			f.run(p, false, 4096);
		}
		for (const auto fault : {error(EIO), error(ENOSPC), zero()}) {
			for (const bool ntfs : {false, true}) {
				Plan p;
				p[Call::Pwrite].set(output, {{}, unalignedDirectShort(4096), fault});
				f.run(p, false, 4096 + 4095, ntfs);
			}
		}
		{
			Plan p;
			p[Call::Pwrite].set(output, {{}, cancelAfterShort(4096, cancel)});
			f.run(p, false, 4096 + 4096);
		}
		// A subsequent operation must succeed with no fault and no stale cancel state.
		Plan clean;
		f.run(clean, true, f.data.size());

	}

	/**
	 * @brief Checks cleanup syscall errors and their effect on copy completion.
	 */
	void windowsCleanupFailures() {

		int failures = 0;
		auto scenario = [&](const char* name, auto test) {

			try { test(); std::cout << "PASS: Windows cleanup: " << name << '\n'; }
			catch (const std::exception& e) {
				++failures;
				std::cerr << "FAIL: Windows cleanup: " << name << ": " << e.what() << '\n';
			}

		};
		scenario("ftruncate EINTR is retried", [] {

			CopyFixture f;
			ReadFd target(f.dst);
			const Identity output(target.fd);
			Plan p;
			p[Call::Pwrite].set(output, {{}, unalignedDirectShort(4096), error(EIO)});
			p[Call::Truncate].set(output, {{}, {}, error(EINTR), {}});
			f.run(p, false, 4096 + 4095);
			CHECK(p[Call::Truncate].calls.load() == 4);

		});
		scenario("hard ftruncate failure retains failed status and recoverable prefix", [] {

			CopyFixture f;
			ReadFd target(f.dst);
			const Identity output(target.fd);
			Plan p;
			p[Call::Pwrite].set(output, {{}, unalignedDirectShort(4096), error(EIO)});
			p[Call::Truncate].set(output, {{}, {}, error(EIO)});
			// No cleanup is possible while the truncate syscall fails. The writer
			// must still report failure, never completion. A fresh copy replaces it.
			f.run(p, false, 4096 + 4095, false, 8192);
			CHECK(p[Call::Truncate].calls.load() == 3);
			Plan clean;
			f.run(clean, true, f.data.size());

		});
		for (const auto& [name, onSource, code] : std::initializer_list<std::tuple<const char*, bool, int>>{
				 {"source close EINTR after failed copy", true, EINTR},
			     {"destination close EIO after failed copy", false, EIO}}) {
			scenario(name, [&] {

				CopyFixture f;
				ReadFd source(f.src), target(f.dst);
				const Identity input(source.fd), output(target.fd);
				Plan p;
				p[Call::Pwrite].set(output, {unalignedDirectShort(4096), error(EIO)});
				p[Call::Close].set(onSource ? input : output, {error(code)});
				f.run(p, false, 4095);
				CHECK(p[Call::Close].calls.load() == 1);
				CHECK(p[Call::Close].effects.load() == 1);
				Plan clean;
				f.run(clean, true, f.data.size());

			});
		}
		CHECK(failures == 0);

	}

	/**
	 * @brief Checks direct-I/O behavior of the FAT32 and NTFS copy paths.
	 */
	void windowsFilesystemWriteModes() {

		CopyFixture f;
		
		ReadFd target(f.dst);
		const Identity output(target.fd);
		for (const bool ntfs : {false, true}) {
			const auto chunkSize = ntfs ? windowsWriter_engine::copy::kBufferSize : windowsWriter_engine::copy::kFatBufferSize;
			f.buffer.resize(chunkSize);
			f.data.resize(chunkSize + 1, 'x');
			writeFile(f.src, f.data);
			Plan p;
			p[Call::Pwrite].set(output, {{}, {}});
			p[Call::Truncate].target = output;
			p[Call::SyncRange].target = output;
			p[Call::Fallocate].target = output;
			f.run(p, true, f.data.size(), ntfs);
			CHECK(p[Call::Pwrite].calls.load() == 2);
			const int flags = p[Call::Pwrite].statusFlags.load();
			CHECK(flags >= 0);
			CHECK((flags & O_DIRECT) == O_DIRECT);
			CHECK(!(flags & O_DSYNC));
			CHECK((flags & O_SYNC) != O_SYNC);
			if (ntfs) {
				CHECK(p[Call::SyncRange].calls.load() == 0);
				CHECK(p[Call::Fallocate].calls.load() == 1);
				CHECK(p[Call::Truncate].calls.load() == 1);
			} else {
				CHECK(p[Call::SyncRange].calls.load() == 1);
				CHECK(p[Call::Fallocate].calls.load() == 0);
				CHECK(p[Call::Truncate].calls.load() == 3);
			}
		}

	}

	static void checkBufferedWriteback(bool ntfs) {

		CopyFixture f;
		constexpr auto range = windowsWriter_engine::copy::kBufferedSyncSize;
		// Smaller source buffers must still accumulate exactly one 4 MiB range.
		f.buffer.resize(range / 2);
		f.data.resize(2 * range + 1, 'x');
		writeFile(f.src, f.data);
		ReadFd target(f.dst);
		const Identity output(target.fd);
		{
			Plan p;
			p[Call::SyncRange].target = output;
			f.run(p, true, f.data.size(), ntfs, 0, true);
			// Three submissions, two waits; syncfs owns the final drain.
			CHECK(p[Call::SyncRange].calls.load() == 5);
		}
		{
			Plan p;
			p[Call::SyncRange].set(output, {error(EINTR), {}});
			f.run(p, true, f.data.size(), ntfs, 0, true);
			CHECK(p[Call::SyncRange].calls.load() == 6);
		}
		{
			Plan p;
			p[Call::SyncRange].set(output, {error(EIO)});
			f.run(p, false, range, ntfs, 0, true);
		}
		{
			Plan p;
			p[Call::SyncRange].set(output, {{}, {}, error(EIO)});
			f.run(p, false, 2 * range, ntfs, 0, true);
		}
		{
			Plan p;
			p[Call::Pwrite].set(output, {{}, {}, cancelAfterShort(137, cancel)});
			p[Call::SyncRange].target = output;
			f.run(p, false, range + 137, ntfs, 0, true);
			CHECK(p[Call::SyncRange].calls.load() == 1);
		}

	}

	static void checkBufferedCrossFileWriteback() {

		CopyFixture f;
		const auto second = f.tmp.path / "second";
		ReadFd target(f.dst);
		Plan p;
		p[Call::SyncRange].target = Identity(target.fd);
		std::atomic<std::uint64_t> total{0};
		GlobalState::g_operationCancelled = false;
		{
			Scope scope(p);
			CHECK(processEntries({{f.src, f.dst, f.data.size(), false},
			    {f.src, second, f.data.size(), false}}, IsoType::FatOnly, "",
			    f.buffer.size(), f.buffer.data(), total));
		}
		// Default buffered mode: the first small file is submitted and waited on by the second file.
		CHECK(p[Call::SyncRange].calls.load() == 2);
		CHECK(total == 2 * f.data.size());
		CHECK(readFile(f.dst) == f.data && readFile(second) == f.data);

	}

	/**
	 * @brief Checks buffered copy transfers, writeback pacing and cancellation.
	 */
	void windowsBufferedWrites() {

		for (const bool ntfs : {false, true}) {
			CopyFixture f;
			ReadFd target(f.dst);
			const Identity output(target.fd);
			Plan p;
			// Buffered short writes resume from the exact byte, without replay.
			p[Call::Pwrite].set(output, {error(EINTR), shortTransfer(137), {}, {}, {}});
			p[Call::Truncate].target = output;
			p[Call::SyncRange].target = output;
			p[Call::Fallocate].target = output;
			f.run(p, true, f.data.size(), ntfs, 0, true);
			CHECK(p[Call::Pwrite].calls.load() == 5);
			const int flags = p[Call::Pwrite].statusFlags.load();
			CHECK(flags >= 0 && !(flags & O_DIRECT) && !(flags & O_DSYNC));
			CHECK(p[Call::Truncate].calls.load() == 0);
			CHECK(p[Call::SyncRange].calls.load() == 1); // Final partial range.
			CHECK(p[Call::Fallocate].calls.load() == 0);

			Plan failure;
			failure[Call::Pwrite].set(output, {{}, shortTransfer(137), error(ENOSPC)});
			failure[Call::SyncRange].target = output;
			f.run(failure, false, 4096 + 137, ntfs, 0, true);
			CHECK(failure[Call::SyncRange].calls.load() == 0);

			Plan cancelled;
			cancelled[Call::Pwrite].set(output, {{}, cancelAfterShort(137, cancel)});
			cancelled[Call::SyncRange].target = output;
			f.run(cancelled, false, 4096 + 137, ntfs, 0, true);
			CHECK(cancelled[Call::SyncRange].calls.load() == 0);

			Plan retry;
			f.run(retry, true, f.data.size(), ntfs, 0, true);
			f.data.clear();
			writeFile(f.src, f.data);
			Plan empty;
			empty[Call::Pwrite].target = output;
			f.run(empty, true, 0, ntfs, 0, true);
			CHECK(empty[Call::Pwrite].calls.load() == 0);
			checkBufferedWriteback(ntfs);
		}
		checkBufferedCrossFileWriteback();

	}

	/**
	 * @brief Checks aligned and unaligned short direct writes, replay and failure handling.
	 */
	void windowsDirectRecovery() {

		for (const bool ntfs : {false, true}) {
			CopyFixture f;
			f.buffer.resize(8192);
			ReadFd target(f.dst);
			const Identity output(target.fd);
			{
				Plan p;
				// Aligned short write: continue from the exact remaining offset.
				p[Call::Pwrite].set(output, {shortTransfer(4096), {}, {}});
				f.run(p, true, f.data.size(), ntfs);
				CHECK(p[Call::Pwrite].calls.load() == 3);
			}
			{
				Plan p;
				// The entire first chunk is replayed after an unaligned report.
				p[Call::Pwrite].set(output, {unalignedDirectShort(4096), {}, {}});
				f.run(p, true, f.data.size(), ntfs);
				CHECK(p[Call::Pwrite].effects.load() == 1);
			}
			{
				Plan p;
				const auto shortWrite = unalignedDirectShort(4096);
				p[Call::Pwrite].set(output, {shortWrite, shortWrite, shortWrite,
				    shortWrite, shortWrite, shortWrite, shortWrite, shortWrite, shortWrite});
				f.run(p, false, 4095, ntfs);
				CHECK(p[Call::Pwrite].effects.load() == 9);
			}
			{
				Plan p;
				p[Call::Pwrite].set(output, {cancelAfterShort(4096, cancel)});
				f.run(p, false, 4096, ntfs);
			}
			{
				Plan p;
				if (ntfs) p[Call::Truncate].set(output, {error(EINTR), {}});
				else p[Call::Truncate].set(output, {{}, {}, error(EINTR), {}});
				f.run(p, true, f.data.size(), ntfs);
				CHECK(p[Call::Truncate].calls.load() == (ntfs ? 2 : 4));
			}
			{
				Plan p;
				if (ntfs) p[Call::Truncate].set(output, {error(EIO), {}});
				else p[Call::Truncate].set(output, {{}, {}, error(EIO), {}});
				f.run(p, false, f.data.size(), ntfs);
				CHECK(p[Call::Truncate].calls.load() == (ntfs ? 2 : 4));
			}
			{
				// Padded bytes are never counted; even a replay of a tiny file
				// must preserve exact contents and CRC-64.
				f.data.resize(1);
				writeFile(f.src, f.data);
				Plan p;
				p[Call::Pwrite].set(output, {unalignedDirectShort(4096), {}});
				f.run(p, true, 1, ntfs);
				CHECK(p[Call::Pwrite].effects.load() == 1);
			}

			if (!ntfs) {
				Plan growthFailure;
				growthFailure[Call::Truncate].set(output, {error(ENOSPC), {}});
				growthFailure[Call::Pwrite].target = output;
				f.run(growthFailure, false, 0);
				CHECK(growthFailure[Call::Pwrite].calls.load() == 0);

				Plan tailFailure;
				tailFailure[Call::SyncRange].set(output, {error(EIO)});
				f.run(tailFailure, false, f.data.size());

				Plan tailRetry;
				tailRetry[Call::SyncRange].set(output, {error(EINTR), {}});
				f.run(tailRetry, true, f.data.size());
			}

			f.data.clear();
			writeFile(f.src, f.data);
			Plan empty;
			empty[Call::Pwrite].target = output;
			f.run(empty, true, 0, ntfs);
			CHECK(empty[Call::Pwrite].calls.load() == 0);

		}
	}
} // namespace isocmd_tests
