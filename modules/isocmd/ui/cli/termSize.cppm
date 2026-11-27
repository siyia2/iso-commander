// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.cli.termSize;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

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
std::size_t visibleWidthFrom(
    const std::string& s,
    std::size_t from);

/**
 * @brief Visible columns of a plain UTF-8 string.
 *
 * Counts UTF-8 code points as 1 column each, skipping continuation bytes.
 *
 * @param s Plain UTF-8 string.
 * @return Number of visible columns.
 */
std::size_t utf8Cols(std::string_view s) noexcept;

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
std::string_view tailColumns(
    std::string_view s,
    std::size_t cols) noexcept;

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
void fitRowToWidth(
    std::string& out,
    std::size_t from,
    std::size_t maxCols);

/**
 * @brief Visible text of every prompt line except the last.
 *
 * Removes the line editor's \001/\002 invisible-text markers and returns the
 * text before the last newline without that newline.
 *
 * @param prompt Full prompt string passed to @c editLine().
 * @return Header text, or empty if the prompt contains no newline.
 */
std::string promptHeaderForRepaint(const std::string& prompt);

} // extern "C++"
