// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.ui.textWidth;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/ui/textWidth ----

// --- UTF-8 / ANSI-escape width helpers (defined in src/write2usb/ui/textWidth.cpp) ---

/**
 * @brief Returns the number of UTF-8 code points (characters) in a string.
 *
 * Counts bytes that are not UTF-8 continuation bytes (i.e., bytes where
 * the top two bits are not 10, or `(c & 0xC0) != 0x80`). Assuming valid
 * UTF-8 input, this gives the total number of characters, serving as an
 * approximation of the string's display column width for standard text.
 *
 * @param s Input string containing UTF-8 encoded text.
 * @return The number of UTF-8 code points in @p s.
 */
std::size_t utf8Width(const std::string& s);

/**
 * @brief Visible width of a string that may contain ANSI CSI escapes.
 *
 * Like utf8Width, but skips ANSI CSI sequences (ESC [ … final byte in
 * 0x40..0x7E), which the terminal interprets rather than renders. The
 * escapes count as 0 columns; every other UTF-8 code point counts as 1.
 *
 * @param s String that may contain colour/formatting escapes.
 * @return Visible columns @p s occupies when printed.
 */
std::size_t visibleWidth(const std::string& s);

/**
 * @brief Cuts a string to at most @p maxCols visible columns.
 *
 * ANSI CSI escape sequences are copied through untouched and do not count
 * toward the budget, so colours opened before the cut remain effective past
 * it (the caller handles any reset it needs). UTF-8 multi-byte sequences
 * are never split: an escape or code point that would cross the limit is
 * dropped whole.
 *
 * @param s       Input string, possibly with ANSI escapes and UTF-8.
 * @param maxCols Maximum visible columns to keep. 0 returns an empty string;
 *                escapes appearing before the first visible character are
 *                still emitted.
 * @return The longest prefix of @p s whose visible width is <= @p maxCols.
 */
std::string clipToWidth(const std::string& s, std::size_t maxCols);

/**
 * @brief Shortens an ISO filename to fit a column budget, keeping ".iso".
 *
 * If @p name already fits in @p maxCols columns it is returned unchanged.
 * Otherwise a trailing ".iso" extension (case-insensitive) is separated off,
 * the stem is cut to maxCols minus (width of "…" + ".iso") columns (floored
 * at 4), and the result is reconstructed as "stem….iso". Truncation walks
 * whole UTF-8 code points, so a multi-byte sequence is never split.
 *
 * @par Stem floor
 * MIN_STEM is 4 columns, so on very narrow terminals the returned string
 * can exceed @p maxCols by up to 5 columns (4 stem + 1 "…") when an
 * extension is present, or by 1 column when it is not. Callers should clip
 * the containing line with clipToWidth to prevent wrap.
 *
 * @param name    Filename to shorten (may be empty, with or without a ".iso"
 *                extension).
 * @param maxCols Column budget the result should fit within; 0 forces the
 *                stem floor and returns "….iso" or "…".
 * @return @p name unchanged if it fits, otherwise the shortened form.
 */
std::string truncateIsoName(const std::string& name, std::size_t maxCols);

} // extern "C++"
