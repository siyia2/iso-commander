// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import isocmd.core.cancellation;
import isocmd.write2usb.util.checksum;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.telemetry;

namespace usb_verify {
	ReadbackProgress::ReadbackProgress(ProgressInfo& slot, std::uint64_t total)
	    : slot_(slot), total_(total), start_(write_telemetry::Clock::now()),
	      estimator_(start_, write_telemetry::windowsStallTimeoutSeconds) {

		slot_.progress.store(0);
		slot_.speed.store(0);
		slot_.bytesVerified.store(0);
		slot_.verifying.store(true);

	}

	void ReadbackProgress::record(std::size_t bytes) {

		done_ += bytes;
		slot_.bytesVerified.store(done_);
		if (total_) slot_.progress.store(write_telemetry::transferPercent(done_, total_));
		if (auto speed = estimator_.sample(done_, write_telemetry::Clock::now()))
		    slot_.speed.store(*speed);

	}

	bool ReadbackProgress::finish() {

		if (done_ != total_ || GlobalState::g_operationCancelled.load()) return false;
		const double seconds = std::chrono::duration<double>(write_telemetry::Clock::now() - start_).count();
		slot_.speed.store(seconds > 0 ? done_ / (1024.0 * 1024.0) / seconds : 0);
		slot_.progress.store(100);
		slot_.completed.store(true);
		return true;

	}

	bool readChecksum(int fd, std::uint64_t size, char* buffer, std::size_t bufferSize,
	                  std::size_t alignment, ReadbackProgress& progress, std::uint64_t& result) {

    	if (!buffer || !bufferSize || !alignment ||
        bufferSize % alignment || size % alignment ||
        reinterpret_cast<std::uintptr_t>(buffer) % alignment)
            return false;
		Checksum64 checksum;
		std::uint64_t done = 0;
		while (done < size) {
			if (GlobalState::g_operationCancelled.load()) return false;
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
		return !GlobalState::g_operationCancelled.load();

	}

	bool verifyFile(const std::filesystem::path& path, std::uint64_t size, std::uint64_t expected,
	                char* buffer, std::size_t bufferSize, ReadbackProgress& progress) {

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
