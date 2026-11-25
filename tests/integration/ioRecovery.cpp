// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/ioFaults.h"

import isocmd.core.cancellation;
import isocmd.write2usb.windows.windowsWriterEngine;

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

			void run(Plan& plan, bool success, std::uint64_t expectedBytes, bool ntfs = false, std::size_t remaining = 0) {

				struct Reset { ~Reset() {

						GlobalState::g_operationCancelled = false;

					} } reset;

				GlobalState::g_operationCancelled = false;
				std::atomic<std::uint64_t> total{0};
				bool result;
				{
					Scope scope(plan);
					result = processEntries({{src, dst, data.size(), false}},
					    ntfs ? IsoType::WindowsInstall : IsoType::FatOnly,
					    ntfs ? tmp.path.string() : "", buffer.size(), buffer.data(), total);
				}
				CHECK(result == success);
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
			p[Call::Pwrite].set(output, {error(EINTR), shortTransfer(713), shortTransfer(37)});
			f.run(p, true, f.data.size());
		}
		{
			Plan p;
			p[Call::SyncRange].set(output, {error(EINTR), {}});
			f.run(p, true, f.data.size());
		}
		{
			// Preallocation is an optimization, not a requirement for copying.
			Plan p;
			p[Call::Fallocate].set(output, {error(EOPNOTSUPP)});
			f.run(p, true, f.data.size(), true);
		}

	}

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
				p[Call::Pwrite].set(output, {{}, shortTransfer(713), fault});
				f.run(p, false, 4096 + 713, ntfs);
			}
		}
		{
			Plan p;
			p[Call::Pwrite].set(output, {{}, cancelAfterShort(713, cancel)});
			f.run(p, false, 4096 + 713);
		}
		// A subsequent operation must succeed with no fault and no stale cancel state.
		Plan clean;
		f.run(clean, true, f.data.size());

	}

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
			p[Call::Pwrite].set(output, {{}, shortTransfer(713), error(EIO)});
			p[Call::Truncate].set(output, {error(EINTR), {}});
			f.run(p, false, 4096 + 713);
			CHECK(p[Call::Truncate].calls.load() == 2);

		});
		scenario("hard ftruncate failure retains failed status and recoverable prefix", [] {

			CopyFixture f;
			ReadFd target(f.dst);
			const Identity output(target.fd);
			Plan p;
			p[Call::Pwrite].set(output, {{}, shortTransfer(713), error(EIO)});
			p[Call::Truncate].set(output, {error(EIO)});
			// No cleanup is possible while the truncate syscall fails. The writer
			// must still report failure, never completion. A fresh copy replaces it.
			f.run(p, false, 4096 + 713, false, 4096 + 713);
			CHECK(p[Call::Truncate].calls.load() == 1);
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
				p[Call::Pwrite].set(output, {shortTransfer(713), error(EIO)});
				p[Call::Close].set(onSource ? input : output, {error(code)});
				f.run(p, false, 713);
				CHECK(p[Call::Close].calls.load() == 1);
				CHECK(p[Call::Close].effects.load() == 1);
				Plan clean;
				f.run(clean, true, f.data.size());

			});
		}
		CHECK(failures == 0);

	}

	void windowsFinalWriteback() {

		CopyFixture f;
		// One copy chunk = one writeback submission, independent of pacing threshold.
		f.buffer.resize(f.data.size());
		ReadFd target(f.dst);
		const Identity output(target.fd);
		{
			// The single final asynchronous range retries interruptions.
			Plan p;
			p[Call::SyncRange].set(output, {error(EINTR), error(EINTR), {}});
			f.run(p, true, f.data.size());
			CHECK(p[Call::SyncRange].calls.load() == 3);
		}
		{
			// Writeback pacing is best-effort: the full Windows writer's later
			// syncfs (not this copy engine) is responsible for durable completion.
			Plan p;
			p[Call::SyncRange].set(output, {error(EINTR), error(EIO)});
			f.run(p, true, f.data.size());
			CHECK(p[Call::SyncRange].calls.load() == 2);
		}

	}
} // namespace isocmd_tests
