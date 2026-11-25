// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.display.readlineResize;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ui/display/readlineResize ----

/**
 * @brief Repaints a screen when the terminal size changes while a Readline
 *        prompt waits.
 *
 * Construct it immediately before @c readline() and let it go out of scope
 * immediately after (or use @ref readlineWithResize).
 *
 * While it exists:
 * - a @c SIGWINCH handler is installed;
 * - an @c rl_event_hook checks for terminal-size changes;
 * - Readline's screen dimensions are updated;
 * - the repaint callback redraws the screen;
 * - Readline restores the prompt and any half-typed input.
 *
 * All terminal and Readline access stays on the calling thread.
 *
 * @note Only one watcher may be active at a time: the event hook and
 *       the @c SIGWINCH handler are process-global.
 *
 * @see readlineWithResize
 */
class ListResizeWatcher {
public:
    /// Redraws the screen (see the callback contract above).
    using Repaint = std::function<void()>;

    /// Implementation detail; public only so the event-hook trampoline can name it.
    struct Impl;

    /**
     * @brief Installs the @c SIGWINCH handler and the chained event hook.
     * @param repaint Repaint callback.
     */
    explicit ListResizeWatcher(Repaint repaint);

    /// Calls @ref stop.
    ~ListResizeWatcher();

    ListResizeWatcher(const ListResizeWatcher&) = delete;
    ListResizeWatcher& operator=(const ListResizeWatcher&) = delete;

    /**
     * @brief Restores the previous SIGWINCH handler and event hook.
     *
     * Safe to call more than once.
     */
    void stop();

private:
    std::unique_ptr<Impl> impl_;
};

/// Owning pointer for a line returned by @c readline() (freed with @c std::free).
using RlLine = std::unique_ptr<char, decltype(&std::free)>;

/**
 * @brief Wraps @c readline() with terminal resize handling and optional
 *        Tab-completion control.
 *
 * @param prompt               Prompt passed to @c readline().
 * @param repaint              Callback invoked on terminal resize; pass
 *                             nullptr to disable resize handling.
 * @param disableTabCompletion When true, suppresses Tab completion for the
 *                             duration of the call.
 *
 * @return The line read, or null on EOF (Ctrl+D).
 */
RlLine readlineWithResize(
    const std::string& prompt,
    ListResizeWatcher::Repaint repaint = nullptr,
    bool disableTabCompletion = false);

} // extern "C++"
