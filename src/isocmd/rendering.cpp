// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// C / System Headers
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../rendering.h"

// ------------------------------------------------------------------------- //
// ListResizeWatcher                                                         //
// ------------------------------------------------------------------------- //

/**
 * @brief State for the SIGWINCH-driven resize watcher.
 *
 * The signal handler only sets an async-signal-safe flag (and forwards to the
 * previously installed handler, e.g. Readline's own). Terminal output and
 * Readline operations happen in @ref tick on the main thread, from Readline's
 * event hook, where Readline's line-buffer and cursor state are safe to touch.
 */
struct ListResizeWatcher::Impl {
    explicit Impl(Repaint r) : repaint(std::move(r)), last(terminalSize()) {}

    Repaint           repaint;
    TermSize          last;                     // last dimensions we repainted for (main thread only)
    rl_hook_func_t* prevHook = nullptr;     // hook installed before ours (chained)

    /// Runs from Readline's event hook on the main thread. Repaints the list
    /// after a resize, updates Readline's screen dimensions, and restores the prompt.
    void tick();
};

namespace {

/// Atomically set from the signal handler, cleared on the main thread.
std::atomic<bool> g_winch{false};

/// Handler that was installed before ours; we forward to it so Readline's own
/// SIGWINCH handling (rl_catch_sigwinch) keeps working.
struct sigaction g_prevWinch {};
bool             g_winchInstalled = false;

/// Active watcher used by the Readline event hook. Accessed only on the
/// thread running Readline; set during construction and cleared by stop().
ListResizeWatcher::Impl* g_activeListWatcher = nullptr;

extern "C" void onWinch(int sig, siginfo_t* info, void* ctx) {
    g_winch.store(true, std::memory_order_relaxed);
    if (g_prevWinch.sa_handler == SIG_DFL || g_prevWinch.sa_handler == SIG_IGN) return;
    if (g_prevWinch.sa_flags & SA_SIGINFO) {
        if (g_prevWinch.sa_sigaction) g_prevWinch.sa_sigaction(sig, info, ctx);
    } else {
        g_prevWinch.sa_handler(sig);
    }
}

/// Handles pending resize repainting, then invokes the hook that was installed
/// before this watcher so other Readline event processing remains active.
int listResizeHook() {
    ListResizeWatcher::Impl* w = g_activeListWatcher;
    if (!w) return 0;
    w->tick();
    return w->prevHook ? w->prevHook() : 0;
}

} // namespace

void ListResizeWatcher::Impl::tick() {
    // Atomically check if true and reset to false in a single instruction.
    // If a signal arrives during processing, exchange() will leave/set it to true
    // so a subsequent tick catches it.
    if (!g_winch.exchange(false, std::memory_order_relaxed)) return;

    const TermSize sz = terminalSize();
    if (sz.cols == 0 || sz.rows == 0) return;
    if (sz.cols == last.cols && sz.rows == last.rows) return;
    last = sz;

    // Keep Readline's wrapping in sync with the dimensions used by the list renderer.
    rl_set_screen_size(static_cast<int>(sz.rows), static_cast<int>(sz.cols));

    // The repaint callback performs the normal list display step. Prevent
    // exceptions from propagating through Readline's C call stack.
    try { repaint(); } catch (...) {}

    // Leave a blank line after the list, then let Readline redraw the prompt
    // and the current input, including its cursor position.
    std::cout << '\n' << std::flush;
    rl_on_new_line();
    rl_redisplay();
}

ListResizeWatcher::ListResizeWatcher(Repaint repaint)
    : impl_(std::make_unique<Impl>(std::move(repaint))) {
    // Only one watcher may be active at a time: hook and signal state are process-global.
    assert(g_activeListWatcher == nullptr && "only one ListResizeWatcher may be active");

    impl_->prevHook = rl_event_hook;
    if (impl_->prevHook == listResizeHook) impl_->prevHook = nullptr;   // never chain to ourselves
    g_activeListWatcher = impl_.get();
    rl_event_hook = listResizeHook;

    struct sigaction sa {};
    sa.sa_sigaction = onWinch;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    g_winchInstalled = (sigaction(SIGWINCH, &sa, &g_prevWinch) == 0);
}

ListResizeWatcher::~ListResizeWatcher() { stop(); }

void ListResizeWatcher::stop() {
    if (!impl_ || g_activeListWatcher != impl_.get()) return;

    if (g_winchInstalled) {
        sigaction(SIGWINCH, &g_prevWinch, nullptr);
        g_winchInstalled = false;
    }

    g_activeListWatcher = nullptr;
    // Only undo our own installation; leave it alone if someone replaced the hook since.
    if (rl_event_hook == listResizeHook) rl_event_hook = impl_->prevHook;
}
