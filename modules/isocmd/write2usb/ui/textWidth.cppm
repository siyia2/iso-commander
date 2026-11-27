// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.ui.textWidth;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

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
 * @return Code-point count excluding ANSI CSI sequences (approximate display width).
 */
std::size_t visibleWidth(const std::string& s);

/**
 * @brief Clips text to at most @p maxCols non-CSI UTF-8 code points.
 *
 * ANSI CSI escape sequences are copied through untouched and do not count
 * toward the budget, so colours opened before the cut remain effective past
 * it (the caller handles any reset it needs). UTF-8 multi-byte sequences
 * are not split. CSI sequences encountered before the next over-budget
 * code point are retained, including any trailing CSI sequences.
 *
 * @param s       Input string, possibly with ANSI escapes and UTF-8.
 * @param maxCols Maximum code points to keep. 0 keeps only any leading
 *                CSI sequences.
 * @return Prefix of @p s within the approximate width budget.
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
 * MIN_STEM is 4 code points, so the returned string can exceed @p maxCols
 * on narrow terminals: up to 9 code points with an ISO extension, or
 * 5 without it. A stem of 4 or fewer code points is returned unchanged.
 * Callers should clip the containing line with clipToWidth.
 *
 * @param name    Filename to shorten (may be empty, with or without a ".iso"
 *                extension).
 * @param maxCols Approximate column budget; 0 uses the minimum stem length.
 * @return @p name unchanged if it fits or the stem is too short to truncate;
 *         otherwise the shortened form, preserving the original extension case.
 */
std::string truncateIsoName(const std::string& name, std::size_t maxCols);

} // extern "C++"
