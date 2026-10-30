// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
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
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../rendering.h"

// ------------------------------------------------------------------------- //
// ListResizeWatcher                                                          //
// ------------------------------------------------------------------------- //

/**
 * @brief State shared by the resize-monitoring thread and Readline's event hook.
 *
 * The worker thread only detects width changes and sets @ref pending. Terminal
 * output and Readline operations are handled by @ref tick on the main thread,
 * where Readline's line-buffer and cursor state can be accessed safely.
 */
struct ListResizeWatcher::Impl {
    explicit Impl(Repaint r) : repaint(std::move(r)), lastCols(terminalSize().cols) {}

    Repaint            repaint;
    std::size_t        lastCols;            // watcher thread only after construction
    std::atomic<bool>  pending{false};
    rl_hook_func_t*    prevHook = nullptr;  // hook installed before ours (chained)

    std::mutex              m;
    std::condition_variable cv;
    bool                    stopRequested = false;
    std::thread             thread;

    /// Polls terminal width every 100 ms and marks changes for the main thread.
    /// Failed size queries retain the last known dimensions.
    void loop() {
        constexpr auto kPoll = std::chrono::milliseconds(100);
        std::unique_lock<std::mutex> lk(m);
        while (!stopRequested) {
            cv.wait_for(lk, kPoll, [&] { return stopRequested; });
            if (stopRequested) break;

            const std::size_t cols = terminalSize().cols;
            if (cols == 0) continue;                            // size never known
            if (lastCols == 0) { lastCols = cols; continue; }   // first reading = baseline
            if (cols != lastCols) {
                lastCols = cols;
                pending.store(true, std::memory_order_release);
            }
        }
    }

    /// Runs from Readline's event hook on the main thread. Repaints the list
    /// after a resize, updates Readline's screen dimensions, and restores the prompt.
    void tick() {
        if (!pending.exchange(false, std::memory_order_acquire)) return;

        const TermSize sz = terminalSize();
        if (sz.cols == 0 || sz.rows == 0) return;

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
};

namespace {

/// Active watcher used by the Readline event hook. Accessed only on the
/// thread running Readline; set during construction and cleared by stop().
ListResizeWatcher::Impl* g_activeListWatcher = nullptr;

/// Handles pending resize repainting, then invokes the hook that was installed
/// before this watcher so other Readline event processing remains active.
int listResizeHook() {
    ListResizeWatcher::Impl* w = g_activeListWatcher;
    if (!w) return 0;
    w->tick();
    return w->prevHook ? w->prevHook() : 0;
}

} // namespace

ListResizeWatcher::ListResizeWatcher(Repaint repaint)
    : impl_(std::make_unique<Impl>(std::move(repaint))) {
    impl_->prevHook = rl_event_hook;
    if (impl_->prevHook == listResizeHook) impl_->prevHook = nullptr;   // never chain to ourselves
    g_activeListWatcher = impl_.get();
    rl_event_hook = listResizeHook;
    impl_->thread = std::thread([p = impl_.get()] { p->loop(); });
}

ListResizeWatcher::~ListResizeWatcher() { stop(); }

void ListResizeWatcher::stop() {
    if (!impl_ || !impl_->thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->stopRequested = true;
    }
    impl_->cv.notify_all();
    impl_->thread.join();

    if (g_activeListWatcher == impl_.get()) {
        g_activeListWatcher = nullptr;
        // Only undo our own installation; leave it alone if someone replaced the hook since.
        if (rl_event_hook == listResizeHook) rl_event_hook = impl_->prevHook;
    }
}
