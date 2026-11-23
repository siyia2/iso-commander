// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TERMSIZE_H
#define TERMSIZE_H

// C++ Standard Library Headers
#include <cstddef>
#include <string>
#include <string_view>

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
TermSize terminalSize();

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

#endif // TERMSIZE_H
