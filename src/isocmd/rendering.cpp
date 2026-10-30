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
 * @brief State shared by the watcher thread and the Readline event hook.
 *
 * The watcher thread only sets @ref pending. Everything that touches the terminal
 * or Readline (@ref tick) runs on the main thread, from the hook, where Readline's
 * globals (rl_line_buffer, rl_point, ...) can be used without a data race.
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

    /// Polls the width every 100 ms and flags a change. A failed query keeps the
    /// last good size (see terminalSize()), so it can never look like a resize.
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

    /// Main thread: if a resize is pending, repaint the list and restore the prompt.
    void tick() {
        if (!pending.exchange(false, std::memory_order_acquire)) return;

        const TermSize sz = terminalSize();
        if (sz.cols == 0 || sz.rows == 0) return;

        // Keep Readline's own wrapping in step with the layout painted below.
        rl_set_screen_size(static_cast<int>(sz.rows), static_cast<int>(sz.cols));

        // The callback re-runs the normal list display step (it must not throw
        // through Readline's C frames).
        try { repaint(); } catch (...) {}

        // Normal layout: the list ends, one blank line, then the prompt line. The
        // callback leaves the cursor where the list display step does; this newline
        // is the blank line. Readline then redraws the prompt's last line plus
        // whatever the user had typed, cursor position included.
        std::cout << '\n' << std::flush;
        rl_on_new_line();
        rl_redisplay();
    }
};

namespace {

/// The watcher the event hook serves. Main thread only (set in the constructor,
/// cleared in stop(), read from the hook, all on the thread that runs readline()).
ListResizeWatcher::Impl* g_activeListWatcher = nullptr;

/// `rl_event_hook`: repaint if a resize is pending, then chain to the previous
/// hook (e.g. the pending-refresh hook) so it keeps working while a watcher is
/// active.
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
