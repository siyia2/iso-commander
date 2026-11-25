// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

import std;
import isocmd.ui.bar.progressBarSizeWatcher;
import isocmd.ui.cli.termSize;

namespace rendering_detail {
namespace {

/// Write end of the self-pipe the SIGWINCH handler pokes. -1 = no watcher.
inline std::atomic<int> progressWinchFd{-1};

/// Set by the SIGWINCH handler, cleared when the size is re-read.
inline std::atomic<bool> progressWinchDirty{false};

/// Set around the handler's write() so closeWake() can wait for it.
inline std::atomic<bool> progressWinchInHandler{false};

/// Handler installed before ours; forwarded to preserve existing handling.
inline struct sigaction previousProgressWinch {};

/// Serializes closeWake() against an in-flight handler write.
inline std::mutex progressWinchCloseMutex;

static_assert(
    std::atomic<int>::is_always_lock_free &&
    std::atomic<bool>::is_always_lock_free,
    "handler state must be lock-free to be used from a signal handler");

/**
 * @brief SIGWINCH handler.
 *
 * Marks the terminal size dirty and writes to the non-blocking self-pipe.
 * The previous handler is forwarded to after our work is complete.
 */
inline void onProgressWinch(int sig, siginfo_t* info, void* ctx) {
    const int savedErrno = errno;

    progressWinchDirty.store(true, std::memory_order_release);

    progressWinchInHandler.store(true, std::memory_order_seq_cst);

    const int fd = progressWinchFd.load(std::memory_order_acquire);

    if (fd >= 0) {
        const char b = 1;
        [[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);
    }

    progressWinchInHandler.store(false, std::memory_order_release);

    errno = savedErrno;

    if (previousProgressWinch.sa_handler == SIG_DFL ||
        previousProgressWinch.sa_handler == SIG_IGN) {
        return;
    }

    if (previousProgressWinch.sa_flags & SA_SIGINFO) {
        if (previousProgressWinch.sa_sigaction) {
            previousProgressWinch.sa_sigaction(sig, info, ctx);
        }
    } else {
        previousProgressWinch.sa_handler(sig);
    }
}

} // namespace
} // namespace rendering_detail

ProgressBarSizeWatcher::ProgressBarSizeWatcher() {
    update();

    if (rendering_detail::progressWinchFd.load(std::memory_order_acquire) >= 0) {
        polling_ = true;
        return;
    }

    if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
        wake_[0] = wake_[1] = -1;
        polling_ = true;
        return;
    }

    rendering_detail::progressWinchFd.store(wake_[1], std::memory_order_release);

    struct sigaction sa {};
    sa.sa_sigaction = rendering_detail::onProgressWinch;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (::sigaction(SIGWINCH, &sa, &rendering_detail::previousProgressWinch) != 0) {
        closeWake();
        polling_ = true;
        return;
    }

    installed_ = true;
}

ProgressBarSizeWatcher::~ProgressBarSizeWatcher() {
    stop();
}

TermSize ProgressBarSizeWatcher::unpack() const {
    const std::uint64_t v = packed_.load(std::memory_order_relaxed);

    return {
        static_cast<std::size_t>(v >> 32),
        static_cast<std::size_t>(v & 0xFFFFFFFFu)
    };
}

TermSize ProgressBarSizeWatcher::get() {
    if (polling_ ||
        rendering_detail::progressWinchDirty.exchange(false, std::memory_order_acquire)) {
        update();
    }

    return unpack();
}

TermSize ProgressBarSizeWatcher::refresh() {
    rendering_detail::progressWinchDirty.store(false, std::memory_order_release);

    update();

    return unpack();
}

void ProgressBarSizeWatcher::drainWake() const {
    if (wake_[0] < 0) {
        return;
    }

    char buf[64];

    while (::read(wake_[0], buf, sizeof buf) > 0) {
    }
}

void ProgressBarSizeWatcher::stop() {
    if (!installed_) {
        return;
    }

    installed_ = false;

    // Restore the previous handler.
    ::sigaction(SIGWINCH, &rendering_detail::previousProgressWinch, nullptr);

    // Disarm the descriptor and wait for any in-flight handler write.
    closeWake();

    polling_ = true;
}

/// Queries the terminal and stores a valid size; keeps the old size on failure.
bool ProgressBarSizeWatcher::update() {
    struct winsize ws {};

    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 ||
        ws.ws_col == 0 || ws.ws_row == 0) {
        return false;
    }

    const std::uint64_t v =
        (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row};

    return packed_.exchange(v, std::memory_order_relaxed) != v;
}

/// Disarms and closes the self-pipe, waiting for any in-flight handler write.
void ProgressBarSizeWatcher::closeWake() {
    std::lock_guard<std::mutex> lk(rendering_detail::progressWinchCloseMutex);

    rendering_detail::progressWinchFd.store(-1, std::memory_order_release);

    while (rendering_detail::progressWinchInHandler.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
    }

    for (int& fd : wake_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}
