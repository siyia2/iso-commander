// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "terminalSync.h"

namespace {
	thread_local unsigned long sessions = 0;

	void announce(const char* marker) noexcept {

		const int saved = errno;
		if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO)) {
			fflush(stdout);
			fputs(marker, stderr);
			fflush(stderr);
		}
		errno = saved;

	}
}

unsigned long isocmd_tests::terminal_sync::readlineSession() noexcept {

	return sessions;

}

extern "C" {
char* __real_readline(const char*);
int __real_ppoll(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*);

char* __wrap_readline(const char* prompt) {

	++sessions;
	announce(isocmd_tests::terminal_sync::input);
	return __real_readline(prompt);

}

// The pause implementation is the only project caller of ppoll. It enters
// this wait after drawing the prompt and activating terminal/signal guards.
// A resize interrupts the wait and repaints before the next call.
int __wrap_ppoll(struct pollfd* fds, nfds_t count, const struct timespec* timeout,
                 const sigset_t* mask) {

	if (count == 1 && fds && fds[0].fd == STDIN_FILENO && !timeout && mask)
		announce(isocmd_tests::terminal_sync::pause);
	return __real_ppoll(fds, count, timeout, mask);

}
}
