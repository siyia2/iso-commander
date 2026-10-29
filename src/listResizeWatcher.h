// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LISTRESIZEWATCHER_H
#define LISTRESIZEWATCHER_H

// C++ Standard Library Headers
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// Third-Party Library Headers
#include <readline/readline.h>

/**
 * @file listResizeWatcher.h
 * @brief Repaints a screen when the terminal width changes while a Readline
 *        prompt is waiting.
 */

/**
 * @brief Repaints a screen when the terminal width changes while a Readline prompt waits.
 *
 * Construct it immediately before @c readline() and let it go out of scope immediately
 * after (or use @ref readlineWithResize, which does exactly that). While it exists:
 * - a background thread polls the terminal width every 100 ms (a failed or zero-sized
 *   query keeps the last known good size, so it is never mistaken for a resize) and only
 *   raises a flag;
 * - an @c rl_event_hook, which Readline calls from its idle loop on the calling thread,
 *   sees the flag, wipes and redraws the screen through the callback, then calls
 *   @c rl_on_new_line() + @c rl_redisplay() so the prompt and any half-typed input come
 *   back. All terminal and Readline access therefore stays on that one thread.
 *
 * The previously installed @c rl_event_hook (for example @c checkPendingRefresh) is
 * chained, not replaced, and is restored when the watcher stops.
 *
 * @par Repaint callback contract
 * The callback must redraw the screen exactly like the normal display step before the
 * prompt does (clearing the screen first) and leave the cursor on the line after the
 * last content line, ONE LINE SHORT of the blank line that precedes the prompt's last
 * line: the watcher prints that '\n' itself. Concretely:
 * - prompt starts with "\n": the callback ends after the last content line;
 * - prompt has no leading "\n" but the normal draw prints a blank line: the callback
 *   omits that blank line (the normal path prints it after the same draw);
 * - prompt carries its own header line(s) before a final '\n': the callback re-prints
 *   them without the trailing '\n' (see @ref promptHeaderForRepaint).
 *
 * It runs on the Readline thread, must not call @c readline(), must have no side effects
 * beyond drawing (no thread spawns, no state advance), and should not throw.
 *
 * @par Example
 * @code
 * auto repaint = [&] { drawScreen(); };
 * drawScreen();
 * std::cout << "\n";                                  // blank line before the prompt
 * RlLine line = readlineWithResize(prompt, repaint);   // watcher lives only for this call
 * @endcode
 *
 * @note Only one watcher may be active at a time. The implementation lives in
 *       printList.cpp because it uses that file's @c terminalSize() helper.
 * @see readlineWithResize, ConsoleResizeWatcher
 */
class ListResizeWatcher {
public:
    /// Redraws the screen (see the callback contract above).
    using Repaint = std::function<void()>;

    /// Implementation detail; public only so the event-hook trampoline can name it.
    struct Impl;

    /**
     * @brief Starts the polling thread and installs the chained event hook.
     * @param repaint Repaint callback (see the contract above).
     */
    explicit ListResizeWatcher(Repaint repaint);

    /// Calls @ref stop.
    ~ListResizeWatcher();

    ListResizeWatcher(const ListResizeWatcher&)            = delete;
    ListResizeWatcher& operator=(const ListResizeWatcher&) = delete;

    /**
     * @brief Stops the thread and uninstalls the hook, restoring the previous one.
     *
     * Safe to call more than once. The hook is only restored if it is still ours,
     * so a hook installed by someone else in the meantime is left alone.
     */
    void stop();

private:
    std::unique_ptr<Impl> impl_;
};

/// Owning pointer for a line returned by @c readline() (freed with @c std::free).
using RlLine = std::unique_ptr<char, decltype(&std::free)>;

/**
 * @brief @c readline() wrapped in a @ref ListResizeWatcher.
 *
 * Runs @p repaint (on the calling thread, from Readline's idle hook) whenever the
 * terminal width changes while the prompt waits. The watcher is stopped and the
 * previous @c rl_event_hook restored before this returns.
 *
 * @param prompt  Prompt passed to @c readline() (may contain \001/\002 markers).
 * @param repaint Repaint callback (see @ref ListResizeWatcher for the contract).
 *                Empty means a plain @c readline() with no watcher and no thread.
 * @return The line read (freed by the returned pointer), or null on EOF (Ctrl+D).
 */
inline RlLine readlineWithResize(const std::string& prompt,
                                 ListResizeWatcher::Repaint repaint = nullptr) {
    RlLine line(nullptr, &std::free);
    if (!repaint) {
        line.reset(readline(prompt.c_str()));
        return line;
    }
    ListResizeWatcher watcher(std::move(repaint));   // stopped when this scope ends
    line.reset(readline(prompt.c_str()));
    return line;
}

/**
 * @brief Visible text of every prompt line except the last.
 *
 * For prompts that carry their own header line(s) before a final '\n' (for example
 * "FolderPaths ↵ to scan ... :\n"), so a repaint callback can re-print the header.
 * Readline's \001/\002 invisible-text markers are removed, because they must not be
 * written to the terminal directly. The text before the last '\n' is returned
 * WITHOUT that '\n': the watcher supplies it, and Readline redraws the last line.
 *
 * @param prompt The full prompt string passed to @c readline().
 * @return The header text, or an empty string if @p prompt contains no '\n'.
 */
inline std::string promptHeaderForRepaint(const std::string& prompt) {
    const std::size_t nl = prompt.rfind('\n');
    if (nl == std::string::npos) return {};
    std::string out;
    out.reserve(nl);
    for (std::size_t i = 0; i < nl; ++i) {
        const char c = prompt[i];
        if (c != '\001' && c != '\002') out.push_back(c);
    }
    return out;
}

#endif // LISTRESIZEWATCHER_H
