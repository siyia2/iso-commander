// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include "ioFaults.h"

namespace isocmd_tests::io_faults {
	namespace {
		std::atomic<Plan*> active{nullptr};

		Step next(Call op, int fd) noexcept {

			auto* plan = active.load(std::memory_order_acquire);
			if (!plan) return {};
			auto& rule = (*plan)[op];
			if (!rule.target || !rule.target->matches(fd)) return {};
			const auto i = rule.calls.fetch_add(1, std::memory_order_relaxed);
			return i < rule.steps.size() ? rule.steps[i] : Step{};

		}

		void recordEffect(Call op) noexcept {

			if (auto* plan = active.load(std::memory_order_acquire))
			    (*plan)[op].effects.fetch_add(1, std::memory_order_relaxed);

		}

		template<class Invoke>
		ssize_t transferStep(const Step& step, size_t count, Invoke invoke) {

			if (step.kind == Kind::Error) { errno = step.error; return -1; }
			if (step.kind == Kind::Zero) return 0;
			if (step.kind == Kind::Short || step.kind == Kind::CancelAfterShort)
			    count = std::min(count, step.bytes);
			const ssize_t result = invoke(count);
			if (step.kind == Kind::CancelAfterShort && result > 0 && step.cancel) step.cancel();
			return result;

		}

		template<class Invoke>
		ssize_t transfer(Call op, int fd, size_t count, Invoke invoke) {

			return transferStep(next(op, fd), count, invoke);

		}
	} // namespace

	bool Identity::matches(int fd) const noexcept {

		struct stat actual{};
		const int saved = errno;
		const bool match = fstat(fd, &actual) == 0 && actual.st_dev == value.st_dev &&
		    actual.st_ino == value.st_ino && (actual.st_mode & S_IFMT) == (value.st_mode & S_IFMT) &&
		    actual.st_rdev == value.st_rdev;
		errno = saved;
		return match;

	}

	Scope::Scope(Plan& plan) {

		Plan* expected = nullptr;
		if (!active.compare_exchange_strong(expected, &plan))
		    throw std::logic_error("nested I/O fault injection");

	}

	Scope::~Scope() {

		active.store(nullptr, std::memory_order_release);

	}
} // namespace isocmd_tests::io_faults

// Separate symbols referenced by --wrap only in integration/privileged test
// executables. The normal application and its CLI test binary are untouched.
using namespace isocmd_tests::io_faults;
extern "C" {
ssize_t __real_read(int, void*, size_t);
ssize_t __real_write(int, const void*, size_t);
ssize_t __real_pwrite(int, const void*, size_t, off_t);
int __real_fsync(int);
int __real_sync_file_range(int, off_t, off_t, unsigned int);
int __real_posix_fallocate(int, off_t, off_t);
int __real_ftruncate(int, off_t);
int __real_close(int);

ssize_t __wrap_read(int fd, void* buf, size_t n) {

	const auto step = next(Call::Read, fd);
	const auto result = transferStep(step, n, [&](size_t count) { return __real_read(fd, buf, count); });
	if (step.kind == Kind::CorruptRead && result > 0) {
		static_cast<unsigned char*>(buf)[0] ^= 1;
	}
	return result;

}

ssize_t __wrap_write(int fd, const void* buf, size_t n) {

	const auto step = next(Call::Write, fd);
	if (step.kind == Kind::UnalignedShort) {
		const auto sector = step.bytes;
		const auto offset = lseek(fd, 0, SEEK_CUR);
		const int flags = fcntl(fd, F_GETFL);
		// Refuse to simulate this on buffered, unaligned or non-seekable I/O.
		if (flags < 0 || !(flags & O_DIRECT) || sector < 2 || n < sector ||
		    n % sector || reinterpret_cast<std::uintptr_t>(buf) % sector ||
		    offset < 0 || static_cast<std::uintmax_t>(offset) % sector) {
			errno = EINVAL;
			return -1;
		}
		const auto written = __real_write(fd, buf, sector);
		if (written != static_cast<ssize_t>(sector)) return written;
		if (lseek(fd, -1, SEEK_CUR) != offset + written - 1) { errno = EIO; return -1; }
		recordEffect(Call::Write);
		return written - 1;
	}
	return transferStep(step, n, [&](size_t count) { return __real_write(fd, buf, count); });

}

ssize_t __wrap_pwrite(int fd, const void* buf, size_t n, off_t off) {

	return transfer(Call::Pwrite, fd, n, [&](size_t count) { return __real_pwrite(fd, buf, count, off); });

}

int __wrap_fsync(int fd) {

	const auto step = next(Call::Fsync, fd);
	if (step.kind == Kind::Error) { errno = step.error; return -1; }
	return __real_fsync(fd);

}

int __wrap_sync_file_range(int fd, off_t off, off_t len, unsigned int flags) {

	const auto step = next(Call::SyncRange, fd);
	if (step.kind == Kind::Error) { errno = step.error; return -1; }
	return __real_sync_file_range(fd, off, len, flags);

}

int __wrap_posix_fallocate(int fd, off_t off, off_t len) {

	const auto step = next(Call::Fallocate, fd);
	// Unlike write/fsync, posix_fallocate returns the error number itself.
	if (step.kind == Kind::Error) return step.error;
	return __real_posix_fallocate(fd, off, len);

}

int __wrap_ftruncate(int fd, off_t len) {

	const auto step = next(Call::Truncate, fd);
	if (step.kind == Kind::Error) { errno = step.error; return -1; }
	return __real_ftruncate(fd, len);

}

int __wrap_close(int fd) {

	const auto step = next(Call::Close, fd);
	// Linux releases the descriptor even when close reports EIO/EINTR.
	// Always close the actual fixture fd; never simulate a leaked descriptor.
	const int result = __real_close(fd);
	if (result == 0 && step.kind == Kind::Error) {
		recordEffect(Call::Close);
		errno = step.error;
		return -1;
	}
	return result;

}
}
