// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LISTRESIZEWATCHER_H
#define LISTRESIZEWATCHER_H

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <readline/readline.h>

/**
 * @brief readline() wrapped in a ListResizeWatcher, so @p repaint runs
 * (on the calling thread, from Readline's idle hook) when the terminal width
 * changes while the prompt waits. The watcher is stopped and the previous
 * rl_event_hook restored before this returns.
 *
 * @param prompt  Prompt passed to readline().
 * @param repaint Repaint callback (see ListResizeWatcher for the contract).
 *                Empty means a plain readline() with no watcher.
 * @return The line read (caller frees via the unique_ptr), or null on EOF.
 */
class ListResizeWatcher {
public:
    using Repaint = std::function<void()>;
    struct Impl;
    explicit ListResizeWatcher(Repaint repaint);
    ~ListResizeWatcher();
    ListResizeWatcher(const ListResizeWatcher&)            = delete;
    ListResizeWatcher& operator=(const ListResizeWatcher&) = delete;
    void stop();
private:
    std::unique_ptr<Impl> impl_;
};

using RlLine = std::unique_ptr<char, decltype(&std::free)>;

/// readline() with a resize watcher around it. An empty `repaint` means plain readline().
inline RlLine readlineWithResize(const std::string& prompt,
                                 ListResizeWatcher::Repaint repaint = nullptr) {
    RlLine line(nullptr, &std::free);
    if (!repaint) { line.reset(readline(prompt.c_str())); return line; }
    ListResizeWatcher watcher(std::move(repaint));   // stopped when this scope ends
    line.reset(readline(prompt.c_str()));
    return line;
}

#endif // LISTRESIZEWATCHER_H
