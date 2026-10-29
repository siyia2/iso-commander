// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONSOLERESIZEWATCHER_H
#define CONSOLERESIZEWATCHER_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include <sys/ioctl.h>
#include <unistd.h>

/**
 * @file consoleResizeWatcher.h
 * @brief Repaints a static screen when the terminal size changes while the
 *        main thread is blocked waiting for a key (no Readline involved).
 */

/**
 * @brief Repaints a static screen on terminal resize while the main thread
 *        blocks in a plain @c read() (not @c readline()).
 *
 * Construct it right before the blocking wait and let it go out of scope right
 * after. A thread polls the terminal size every 50 ms (a failed or zero-sized
 * query is ignored, so it is never mistaken for a resize). When the size
 * changes, the watcher waits until it has been stable for about 150 ms, then
 * repaints, and repaints once more after 250 ms. The second pass corrects the
 * cursor position for terminals that finish reflowing after the first draw.
 *
 * @par Repaint contract
 * The callback must redraw the WHOLE screen, including the "press Enter..."
 * message, and leave the cursor at the end of that message. It must be safe to
 * call twice in a row (idempotent). It runs on the watcher thread, must not
 * read input, and should not throw. The main thread must not write to the
 * terminal while the watcher exists.
 *
 * The callback runs under an internal mutex, and stop() takes that mutex, so
 * once stop() (or the destructor) returns, no repaint is running or pending.
 * The mutex is released during the delay between the two passes, so stop()
 * can interrupt it.
 *
 * @note Only one watcher may be active at a time. For Readline prompts use
 *       ListResizeWatcher / readlineWithResize() instead.
 * @see ListResizeWatcher
 */
class ConsoleResizeWatcher {
public:
    /// Redraws the whole screen (see the repaint contract above).
    using Repaint = std::function<void()>;

    /**
     * @brief Starts the polling thread (only if @p repaint is non-empty).
     * @param repaint Redraws the whole screen, including the wait message.
     */
    explicit ConsoleResizeWatcher(Repaint repaint)
        : repaint_(std::move(repaint)), lastSize_(querySize()) {
        if (repaint_) thread_ = std::thread([this] { loop(); });
    }

    /// Calls @ref stop.
    ~ConsoleResizeWatcher() { stop(); }

    ConsoleResizeWatcher(const ConsoleResizeWatcher&)            = delete;
    ConsoleResizeWatcher& operator=(const ConsoleResizeWatcher&) = delete;

    /**
     * @brief Stops the thread; after it returns no repaint is running or pending.
     *
     * Safe to call more than once. Blocks until a repaint in progress finishes.
     */
    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            stopRequested_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    /// Current terminal size packed as (cols << 32 | rows), or 0 if unknown.
    static std::uint64_t querySize() {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
            return (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row};
        return 0;
    }

    /// Runs the repaint callback, swallowing exceptions, and flushes stdout.
    void paint() {
        try { repaint_(); std::cout << std::flush; } catch (...) {}
    }

    /**
     * @brief Polls every 50 ms; once the size has been stable for 150 ms after
     *        a change, repaints, waits 250 ms and repaints again.
     */
    void loop() {
        constexpr auto kPoll        = std::chrono::milliseconds(50);
        constexpr auto kSecondPass  = std::chrono::milliseconds(250);
        constexpr int  kSettlePolls = 3;

        std::unique_lock<std::mutex> lk(m_);
        std::uint64_t seen    = lastSize_;
        bool          changed = false;
        int           stable  = 0;

        while (!stopRequested_) {
            cv_.wait_for(lk, kPoll, [&] { return stopRequested_; });
            if (stopRequested_) break;

            const std::uint64_t now = querySize();
            if (now == 0) continue;                       // query failed: keep last good
            if (seen == 0) { seen = now; continue; }      // first reading = baseline

            if (now != seen) {                            // still changing
                seen    = now;
                changed = true;
                stable  = 0;
            } else if (changed && ++stable >= kSettlePolls) {
                changed = false;
                stable  = 0;

                paint();

                // Some terminals finish their own reflow (moving the cursor)
                // after our first draw; repaint once more after a short delay.
                cv_.wait_for(lk, kSecondPass, [&] { return stopRequested_; });
                if (stopRequested_) break;
                if (querySize() != seen) continue;        // resized again: debounce it
                paint();
            }
        }
    }

    Repaint                 repaint_;
    std::uint64_t           lastSize_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stopRequested_ = false;
    std::thread             thread_;
};

#endif // CONSOLERESIZEWATCHER_H
