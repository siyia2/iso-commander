// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RENDERING_H
#define RENDERING_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// C / System Headers
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

namespace {
    /// Synchronized-output (DEC mode 2026) control sequences, kept as named
    /// constants so callers that write the sequences by hand stay in sync
    /// with @ref SyncOutput.
    constexpr bool             kUseSynchronizedOutput = true;
    constexpr std::string_view kSyncBegin             = "\033[?2026h";
    constexpr std::string_view kSyncEnd               = "\033[?2026l";
}

/**
 * @brief RAII guard that brackets a terminal repaint in DEC mode 2026
 *        (synchronized output).
 *
 * Writes @ref kSyncBegin (`CSI ? 2026 h`) on construction and @ref kSyncEnd
 * (`CSI ? 2026 l`) on destruction, so every `std::cout` write between the two
 * is held by the terminal emulator and presented as a single atomic frame.
 * Terminals that do not implement mode 2026 typically ignore the sequences
 * (or, on a few, print them literally); every emulator that does implement it
 * has a watchdog (~1 s) that auto-commits if the matching `l` never arrives,
 * so a throwing code path between construction and destruction cannot leave
 * the terminal permanently frozen — only briefly stalled.
 *
 * The destructor flushes, because the commit only happens when the closing
 * sequence actually reaches the terminal; a buffered `?2026l` is not a commit.
 *
 * Non-copyable and non-movable: one guard owns one envelope. Do not nest two
 * `SyncOutput` objects around code that mixes `std::cout` and `std::cerr`:
 * `std::cerr` is `unitbuf` and tied to `std::cout`, so a nested envelope can
 * split the frame and reorder the two streams.
 */
struct SyncOutput {
    SyncOutput()  { std::cout << kSyncBegin; }
    ~SyncOutput() { std::cout << kSyncEnd << std::flush; }

    SyncOutput(const SyncOutput&)            = delete;
    SyncOutput& operator=(const SyncOutput&) = delete;
};

// ------------------------------------------------------------------------- //
// Terminal width helpers                                                     //
// ------------------------------------------------------------------------- //

/**
 * @brief Terminal size in columns and rows; {0, 0} if it has never been known.
 *
 * Produced by @ref terminalSize. @c cols and @c rows are both @c std::size_t
 * copies of the @c winsize fields (`unsigned short` on POSIX), so each always
 * fits in 32 bits and the packed representation described there is lossless.
 */
struct TermSize { std::size_t cols; std::size_t rows; };

/**
 * @brief Current terminal size in columns and rows.
 *
 * Queries @c TIOCGWINSZ on stdout and remembers every successful reading.
 *
 * @par Caching and fallback
 * On success (a positive @c ws_col and @c ws_row), the new size is stored and
 * returned. If the query fails, or reports a zero dimension — which happens
 * when stdout is not a tty, is detached, or the pty is transiently resized —
 * the last known good size is returned instead. A transient failure therefore
 * never changes the layout and is never mistaken for a resize.
 *
 * Before the first successful query the result is @c {0, 0}. Callers treat
 * that as "no limit": rows are printed unclipped rather than assumed to fit
 * an arbitrary default.
 *
 * @par Thread safety
 * The remembered size is one packed @c std::atomic<uint64_t>
 * (`cols << 32 | rows`), so concurrent readers never observe a torn pair
 * (cols from one query, rows from another). The function is safe to call from
 * the main thread, from the Readline event hook, and from the
 * @ref ListResizeWatcher polling thread.
 *
 * @return The current size as @c {cols, rows}, or the last known good size if
 *         the query fails; @c {0, 0} if no successful query has happened yet.
 */
inline TermSize terminalSize() {
    static std::atomic<std::uint64_t> lastGood{0};

    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        lastGood.store((std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row},
                       std::memory_order_relaxed);
        return { ws.ws_col, ws.ws_row };
    }
    const std::uint64_t v = lastGood.load(std::memory_order_relaxed);
    return { static_cast<std::size_t>(v >> 32), static_cast<std::size_t>(v & 0xFFFFFFFFu) };
}

/**
 * @brief Terminal width in columns.
 *
 * Convenience wrapper around @ref terminalSize.
 *
 * @return The terminal width in columns, or @c 0 if it has never been known
 *         (see @ref terminalSize for the fallback rules).
 */
inline std::size_t terminalColumns() { return terminalSize().cols; }

/**
 * @brief Visible columns of a string that may contain ANSI CSI escapes.
 *
 * Counts UTF-8 code points as 1 column each and ANSI CSI escape sequences
 * (`ESC [ … final byte in 0x40..0x7E`) as 0 columns, so the result matches
 * what a terminal would actually render. Continuation bytes (top two bits
 * @c 10) are skipped, so a multi-byte code point such as "…" counts once.
 *
 * This is a code-point count, not full Unicode display width: East-Asian
 * wide characters (CJK, emoji) are counted as 1 column even though terminals
 * usually render them as 2. It is correct for the ASCII and common Latin-1
 * text this file measures.
 *
 * Only CSI sequences are recognised. Other escape sequences — for example
 * OSC (`ESC ] … BEL`), two-character escapes such as `ESC c`, or SGR runs
 * split across writes — are not parsed: their bytes are counted as ordinary
 * visible characters. Keep input to CSI-only text if exact widths matter.
 *
 * @param s    String to measure, possibly containing ANSI CSI escapes and UTF-8.
 * @param from Byte offset at which to start measuring (e.g. after a prefix
 *             that must not count). Bytes before @p from are ignored.
 * @return Number of visible columns @c s[from..] occupies.
 */
inline std::size_t visibleWidthFrom(const std::string& s, std::size_t from) {
    std::size_t w = 0;
    for (std::size_t i = from; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            if (i < s.size()) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++w;
        ++i;
    }
    return w;
}

/**
 * @brief Visible columns of a plain UTF-8 string.
 *
 * Counts UTF-8 code points as 1 column each, skipping continuation bytes
 * (top two bits @c 10). No escape parsing is performed: the input is assumed
 * to be plain text (e.g. a prompt or a filename), so an embedded `ESC` byte
 * would be counted as an ordinary character.
 *
 * For strings that may contain ANSI CSI escapes, use @ref visibleWidthFrom.
 *
 * @param s Plain UTF-8 string.
 * @return Number of visible columns @p s occupies.
 */
inline std::size_t utf8Cols(std::string_view s) noexcept {
    std::size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++w;
    return w;
}

/**
 * @brief The last @p cols code points of @p s.
 *
 * Returns the suffix of @p s that occupies the last @p cols visible columns,
 * counted by UTF-8 code point. If @p s is shorter than @p cols, all of @p s
 * is returned. Multi-byte sequences are never split: the boundary is always
 * a code-point boundary.
 *
 * No escape parsing is performed, so this is intended for plain text (e.g.
 * trimming an overlong right-aligned field). For strings with ANSI CSI
 * escapes, the escape bytes would be counted as ordinary code points.
 *
 * @param s    Plain UTF-8 string to take the tail of.
 * @param cols Number of trailing code points to keep; @c 0 returns an empty
 *             view.
 * @return A view into @p s covering its last @p cols code points (or all of
 *         @p s if it is shorter). The view aliases @p s, which must outlive it.
 */
inline std::string_view tailColumns(std::string_view s, std::size_t cols) noexcept {
    if (cols == 0) return {};
    std::size_t seen = 0;
    for (std::size_t i = s.size(); i > 0; ) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++seen == cols)
            return s.substr(i);
    }
    return s;
}

/**
 * @brief Cuts the row that starts at @c out[from] to at most @p maxCols
 *        visible columns.
 *
 * Last line of defence against wrapping. If the row is wider than @p maxCols
 * it is cut in place so that it ends with "…" and occupies exactly
 * @p maxCols columns. ANSI CSI escapes before the cut are kept; any escape
 * sequence that would have appeared after the cut is dropped along with the
 * text it belonged to (callers append their own reset afterwards). A
 * multi-byte character is never split. A row that already fits is left
 * untouched. @p maxCols below 2 leaves the row alone (there is no room for
 * even one character plus "…").
 */
inline void fitRowToWidth(std::string& out, std::size_t from, std::size_t maxCols) {
    if (maxCols < 2) return;

    std::size_t w = 0;
    std::size_t cutAt = std::string::npos;   // offset just before the (maxCols)th visible char

    for (std::size_t i = from; i < out.size(); ) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 0x1B && i + 1 < out.size() && out[i + 1] == '[') {
            i += 2;
            while (i < out.size() && !(out[i] >= 0x40 && out[i] <= 0x7E)) ++i;
            if (i < out.size()) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) {
            if (w == maxCols - 1) cutAt = i;
            ++w;
        }
        ++i;
    }

    if (w > maxCols && cutAt != std::string::npos) {
        out.resize(cutAt);
        out.append("\u2026");
    }
}

/**
 * @brief Repaints a screen when the terminal width changes while a Readline
 *        prompt waits.
 *
 * Construct it immediately before @c readline() and let it go out of scope
 * immediately after (or use @ref readlineWithResize, which does exactly that).
 * While it exists:
 * - a background thread polls the terminal width every 100 ms (a failed or
 *   zero-sized query keeps the last known good size, so it is never mistaken
 *   for a resize) and only raises a flag;
 * - an @c rl_event_hook, which Readline calls from its idle loop on the
 *   calling thread, sees the flag, wipes and redraws the screen through the
 *   callback, then calls @c rl_on_new_line() + @c rl_redisplay() so the
 *   prompt and any half-typed input come back. All terminal and Readline
 *   access therefore stays on that one thread.
 *
 * The previously installed @c rl_event_hook (for example
 * @c checkPendingRefresh) is chained, not replaced, and is restored when the
 * watcher stops.
 *
 * @note The watcher itself does not touch Readline's @c rl_catch_sigwinch.
 *       If you construct it directly and want to avoid the double redraw
 *       described under @ref readlineWithResize, disable @c rl_catch_sigwinch
 *       yourself around the watcher's lifetime. @ref readlineWithResize does
 *       this for you.
 *
 * @par Repaint callback contract
 * The callback must redraw the screen exactly like the normal display step
 * before the prompt does (clearing the screen first) and leave the cursor on
 * the line after the last content line, ONE LINE SHORT of the blank line that
 * precedes the prompt's last line: the watcher prints that '\n' itself.
 * Concretely:
 * - prompt starts with "\n": the callback ends after the last content line;
 * - prompt has no leading "\n" but the normal draw prints a blank line: the
 *   callback omits that blank line (the normal path prints it after the same
 *   draw);
 * - prompt carries its own header line(s) before a final '\n': the callback
 *   re-prints them without the trailing '\n' (see @ref promptHeaderForRepaint).
 *
 * It runs on the Readline thread, must not call @c readline(), must have no
 * side effects beyond drawing (no thread spawns, no state advance), and
 * should not throw.
 *
 * @par Example
 * @code
 * auto repaint = [&] { drawScreen(); };
 * drawScreen();
 * std::cout << "\n";                                  // blank line before the prompt
 * RlLine line = readlineWithResize(prompt, repaint);   // watcher lives only for this call
 * @endcode
 *
 * @note Only one watcher may be active at a time.
 * @see readlineWithResize
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
     * Safe to call more than once. The hook is only restored if it is still
     * ours, so a hook installed by someone else in the meantime is left alone.
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
 * Runs @p repaint (on the calling thread, from Readline's idle hook) whenever
 * the terminal width changes while the prompt waits. The watcher is stopped
 * and the previous @c rl_event_hook restored before this returns.
 *
 * Readline's own SIGWINCH handling (@c rl_catch_sigwinch) is DISABLED for the
 * watcher's lifetime and restored before this returns. The watcher already
 * redraws on resize; leaving Readline's built-in SIGWINCH handler enabled
 * makes it call @c rl_resize_terminal() -> @c rl_forced_update_display() ->
 * @c rl_redisplay_function() on the same event, racing the watcher's redraw
 * and painting the prompt twice (most visibly when the repaint callback does
 * direct terminal I/O that leaves Readline's internal cursor bookkeeping
 * stale, e.g. a full @c printList() repaint). The previous value is restored
 * on every exit path, including exceptions from the watcher constructor (the
 * @c SigwinchGuard RAII object below).
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

    // Suppress Readline's own SIGWINCH redraw for the watcher's lifetime so a
    // resize triggers exactly one redraw (the watcher's). Restore on every
    // exit path, including if the watcher constructor throws.
    struct SigwinchGuard {
        int saved;
        SigwinchGuard() : saved(rl_catch_sigwinch) { rl_catch_sigwinch = 0; }
        ~SigwinchGuard() { rl_catch_sigwinch = saved; }
        SigwinchGuard(const SigwinchGuard&)            = delete;
        SigwinchGuard& operator=(const SigwinchGuard&) = delete;
    } guard;

    ListResizeWatcher watcher(std::move(repaint));   // stopped when this scope ends
    line.reset(readline(prompt.c_str()));
    return line;
}

/**
 * @brief Visible text of every prompt line except the last.
 *
 * For prompts that carry their own header line(s) before a final '\n' (for
 * example "FolderPaths ↵ to scan ... :\n"), so a repaint callback can re-print
 * the header. Readline's \001/\002 invisible-text markers are removed, because
 * they must not be written to the terminal directly. The text before the last
 * '\n' is returned WITHOUT that '\n': the watcher supplies it, and Readline
 * redraws the last line.
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

#endif // RENDERING_H
