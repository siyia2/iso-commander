// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RENDERING_H
#define RENDERING_H

// C++ Standard Library Headers
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// C / System Headers
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

/// Synchronized-output (DEC mode 2026) control sequences, kept as named
/// constants so callers that write the sequences by hand stay in sync
/// with @ref SyncOutput.
inline constexpr bool             kUseSynchronizedOutput = true;
inline constexpr std::string_view kSyncBegin             = "\033[?2026h";
inline constexpr std::string_view kSyncEnd               = "\033[?2026l";

/**
 * @brief RAII guard that brackets a terminal repaint in DEC mode 2026
 *        (synchronized output).
 *
 * Writes @ref kSyncBegin (`CSI ? 2026 h`) on construction and @ref kSyncEnd
 * (`CSI ? 2026 l`) on destruction, so every `std::cout` write between the two
 * is held by the terminal emulator and presented as a single atomic frame.
 * Terminals that do not implement mode 2026 typically ignore the sequences.
 *
 * The destructor flushes, because the commit only happens when the closing
 * sequence actually reaches the terminal; a buffered `?2026l` is not a commit.
 *
 * Non-copyable and non-movable: one guard owns one envelope. Do not nest two
 * `SyncOutput` objects around code that mixes `std::cout` and `std::cerr`.
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
struct TermSize {
    std::size_t cols;
    std::size_t rows;
};

/**
 * @brief Current terminal size in columns and rows.
 *
 * Queries @c TIOCGWINSZ on stdout and remembers every successful reading.
 *
 * @par Caching and fallback
 * On success (a positive @c ws_col and @c ws_row), the new size is stored and
 * returned. If the query fails, or reports a zero dimension — which happens
 * when stdout is not a tty, is detached, or the pty is transiently resized —
 * the last known good size is returned instead.
 *
 * Before the first successful query the result is @c {0, 0}. Callers treat
 * that as "no limit": rows are printed unclipped rather than assumed to fit
 * an arbitrary default.
 *
 * @par Thread safety
 * The remembered size is one packed @c std::atomic<uint64_t>
 * (`cols << 32 | rows`), so concurrent readers never observe a torn pair.
 *
 * @return The current size as @c {cols, rows}, or the last known good size if
 *         the query fails; @c {0, 0} if no successful query has happened yet.
 */
inline TermSize terminalSize() {
    static std::atomic<std::uint64_t> lastGood{0};

    struct winsize ws{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        lastGood.store(
            (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row},
            std::memory_order_relaxed);

        return {ws.ws_col, ws.ws_row};
    }

    const std::uint64_t v = lastGood.load(std::memory_order_relaxed);

    return {
        static_cast<std::size_t>(v >> 32),
        static_cast<std::size_t>(v & 0xFFFFFFFFu)
    };
}

/**
 * @brief Terminal width in columns.
 *
 * Convenience wrapper around @ref terminalSize.
 *
 * @return The terminal width in columns, or @c 0 if it has never been known.
 */
inline std::size_t terminalColumns() {
    return terminalSize().cols;
}

// ------------------------------------------------------------------------- //
// Progress-bar terminal resize watcher                                      //
// ------------------------------------------------------------------------- //

namespace rendering_detail {

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

} // namespace rendering_detail

/**
 * @brief Tracks terminal dimensions and wakes the progress render loop
 *        immediately when SIGWINCH is received.
 *
 * Installs a SIGWINCH handler that marks the size dirty and writes to a
 * non-blocking self-pipe. The progress render loop polls the pipe together
 * with stdin, avoiding the need for a background polling thread.
 *
 * Terminal dimensions are re-read only when the dirty flag is set, unless
 * fallback polling is active. The dimensions are stored in a packed atomic
 * value so readers never observe a torn columns/rows pair.
 *
 * @par Hardening
 * Failed queries and zero dimensions never overwrite the last known good
 * size. Before the first successful query, the fallback is 80x24.
 *
 * @par Fallback
 * If the pipe or signal handler cannot be installed, or another progress
 * watcher is already installed, get() re-reads the dimensions on every call.
 * The render loop's normal timeout still detects terminal changes.
 *
 * @par Threading
 * No background thread is created. All terminal drawing remains on the
 * calling thread. Only one ProgressBarSizeWatcher may be installed at a time.
 *
 * @par Close/write ordering
 * stop() restores the previous SIGWINCH handler, disarms the pipe descriptor,
 * waits for an in-flight handler write to finish, and then closes the pipe.
 */
class ProgressBarSizeWatcher {
public:
    /**
     * @brief Creates the watcher and installs the SIGWINCH handler.
     */
    ProgressBarSizeWatcher() {
        update();

        if (rendering_detail::progressWinchFd.load(
                std::memory_order_acquire) >= 0) {
            polling_ = true;
            return;
        }

        if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
            wake_[0] = wake_[1] = -1;
            polling_ = true;
            return;
        }

        rendering_detail::progressWinchFd.store(
            wake_[1], std::memory_order_release);

        struct sigaction sa {};
        sa.sa_sigaction = rendering_detail::onProgressWinch;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);

        if (::sigaction(
                SIGWINCH,
                &sa,
                &rendering_detail::previousProgressWinch) != 0) {
            closeWake();
            polling_ = true;
            return;
        }

        installed_ = true;
    }

    ~ProgressBarSizeWatcher() {
        stop();
    }

    ProgressBarSizeWatcher(const ProgressBarSizeWatcher&) = delete;
    ProgressBarSizeWatcher& operator=(const ProgressBarSizeWatcher&) = delete;

    /**
     * @brief Returns the last known good terminal dimensions.
     *
     * Re-reads the terminal only when SIGWINCH has marked the size dirty
     * or fallback polling is active.
     */
    TermSize get() {
        if (polling_ ||
            rendering_detail::progressWinchDirty.exchange(
                false, std::memory_order_acquire)) {
            update();
        }

        const std::uint64_t v = packed_.load(std::memory_order_relaxed);

        return {
            static_cast<std::size_t>(v >> 32),
            static_cast<std::size_t>(v & 0xFFFFFFFFu)
        };
    }

    /**
     * @brief Forces a synchronous terminal-size query.
     *
     * Useful immediately before printing the final report.
     */
    TermSize refresh() {
        rendering_detail::progressWinchDirty.store(
            false, std::memory_order_release);

        update();

        const std::uint64_t v = packed_.load(std::memory_order_relaxed);

        return {
            static_cast<std::size_t>(v >> 32),
            static_cast<std::size_t>(v & 0xFFFFFFFFu)
        };
    }

    /// Read end of the self-pipe for poll(), or -1 if unavailable.
    int wakeFd() const {
        return wake_[0];
    }

    /// Empties the self-pipe after poll() reports it readable.
    void drainWake() const {
        if (wake_[0] < 0) {
            return;
        }

        char buf[64];

        while (::read(wake_[0], buf, sizeof buf) > 0) {
        }
    }

    /**
     * @brief Restores the previous SIGWINCH handler and closes the pipe.
     *
     * Safe to call more than once.
     */
    void stop() {
        if (!installed_) {
            return;
        }

        installed_ = false;

        // Restore the previous handler.
        ::sigaction(
            SIGWINCH,
            &rendering_detail::previousProgressWinch,
            nullptr);

        // Disarm the descriptor and wait for any in-flight handler write.
        closeWake();

        polling_ = true;
    }

private:
    /**
     * @brief Queries the terminal and stores a valid size.
     *
     * Does not overwrite the stored size if the query fails or returns
     * zero dimensions.
     */
    bool update() {
        struct winsize ws {};

        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 ||
            ws.ws_col == 0 || ws.ws_row == 0) {
            return false;
        }

        const std::uint64_t v =
            (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row};

        return packed_.exchange(v, std::memory_order_relaxed) != v;
    }

    /**
     * @brief Disarms and closes the self-pipe.
     *
     * Waits for any handler already inside its write() to finish before
     * closing either descriptor.
     */
    void closeWake() {
        std::lock_guard<std::mutex> lk(
            rendering_detail::progressWinchCloseMutex);

        rendering_detail::progressWinchFd.store(
            -1, std::memory_order_release);

        while (rendering_detail::progressWinchInHandler.load(
            std::memory_order_seq_cst)) {
            std::this_thread::yield();
        }

        for (int& fd : wake_) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    }

    std::atomic<std::uint64_t> packed_{(std::uint64_t{80} << 32) | 24u};

    int wake_[2] = {-1, -1};

    bool installed_ = false;
    bool polling_ = false;
};

// ------------------------------------------------------------------------- //
// Visible-width and UTF-8 helpers                                           //
// ------------------------------------------------------------------------- //

/**
 * @brief Visible columns of a string that may contain ANSI CSI escapes.
 *
 * Counts UTF-8 code points as 1 column each and ANSI CSI escape sequences
 * (`ESC [ … final byte in 0x40..0x7E`) as 0 columns.
 *
 * This is a code-point count, not full Unicode display width: East-Asian
 * wide characters and emoji are counted as 1 column even though terminals
 * usually render them as 2.
 *
 * Only CSI sequences are recognised. Other escape sequences are not parsed.
 *
 * @param s    String to measure, possibly containing ANSI CSI escapes and UTF-8.
 * @param from Byte offset at which to start measuring.
 * @return Number of visible columns occupied by s[from..].
 */
inline std::size_t visibleWidthFrom(
    const std::string& s,
    std::size_t from) {

    std::size_t w = 0;

    for (std::size_t i = from; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;

            while (i < s.size() &&
                   !(s[i] >= 0x40 && s[i] <= 0x7E)) {
                ++i;
            }

            if (i < s.size()) {
                ++i;
            }

            continue;
        }

        if ((c & 0xC0) != 0x80) {
            ++w;
        }

        ++i;
    }

    return w;
}

/**
 * @brief Visible columns of a plain UTF-8 string.
 *
 * Counts UTF-8 code points as 1 column each, skipping continuation bytes.
 *
 * @param s Plain UTF-8 string.
 * @return Number of visible columns.
 */
inline std::size_t utf8Cols(std::string_view s) noexcept {
    std::size_t w = 0;

    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++w;
        }
    }

    return w;
}

/**
 * @brief The last @p cols code points of @p s.
 *
 * Returns the suffix of @p s that occupies the last @p cols visible columns,
 * counted by UTF-8 code point.
 *
 * @param s    Plain UTF-8 string to take the tail of.
 * @param cols Number of trailing code points to keep.
 * @return A view into @p s covering its last @p cols code points.
 */
inline std::string_view tailColumns(
    std::string_view s,
    std::size_t cols) noexcept {

    if (cols == 0) {
        return {};
    }

    std::size_t seen = 0;

    for (std::size_t i = s.size(); i > 0;) {
        --i;

        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 &&
            ++seen == cols) {
            return s.substr(i);
        }
    }

    return s;
}

/**
 * @brief Cuts the row that starts at @c out[from] to at most @p maxCols
 *        visible columns.
 *
 * If the row is wider than @p maxCols it is cut in place so that it ends
 * with "…" and occupies exactly @p maxCols columns.
 *
 * ANSI CSI escapes before the cut are kept. A multi-byte character is
 * never split.
 */
inline void fitRowToWidth(
    std::string& out,
    std::size_t from,
    std::size_t maxCols) {

    if (maxCols < 2) {
        return;
    }

    std::size_t w = 0;
    std::size_t cutAt = std::string::npos;

    for (std::size_t i = from; i < out.size();) {
        const unsigned char c = static_cast<unsigned char>(out[i]);

        if (c == 0x1B && i + 1 < out.size() && out[i + 1] == '[') {
            i += 2;

            while (i < out.size() &&
                   !(out[i] >= 0x40 && out[i] <= 0x7E)) {
                ++i;
            }

            if (i < out.size()) {
                ++i;
            }

            continue;
        }

        if ((c & 0xC0) != 0x80) {
            if (w == maxCols - 1) {
                cutAt = i;
            }

            ++w;
        }

        ++i;
    }

    if (w > maxCols && cutAt != std::string::npos) {
        out.resize(cutAt);
        out.append("\u2026");
    }
}

// ------------------------------------------------------------------------- //
// Readline terminal resize handling                                         //
// ------------------------------------------------------------------------- //

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

/**
 * @brief Visible text of every prompt line except the last.
 *
 * Removes Readline's \001/\002 invisible-text markers and returns the
 * text before the last newline without that newline.
 *
 * @param prompt Full prompt string passed to @c readline().
 * @return Header text, or empty if the prompt contains no newline.
 */
inline std::string promptHeaderForRepaint(const std::string& prompt) {
    const std::size_t nl = prompt.rfind('\n');

    if (nl == std::string::npos) {
        return {};
    }

    std::string out;
    out.reserve(nl);

    for (std::size_t i = 0; i < nl; ++i) {
        const char c = prompt[i];

        if (c != '\001' && c != '\002') {
            out.push_back(c);
        }
    }

    return out;
}

#endif // RENDERING_H
