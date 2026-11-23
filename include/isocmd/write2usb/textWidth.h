// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE_TEXTWIDTH_H
#define WRITE_TEXTWIDTH_H

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

// --- UTF-8 / ANSI-escape width helpers (defined in write2usbUIRendering.cpp) ---

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
inline size_t utf8Width(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++w;
    return w;
}

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
inline size_t visibleWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ) {
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
inline std::string clipToWidth(const std::string& s, size_t maxCols) {
    std::string out;
    out.reserve(s.size());
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ) {
        if (static_cast<unsigned char>(s[i]) == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j;
            if (j < s.size()) ++j;
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        size_t len = 1;
        while (i + len < s.size() &&
            (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
        if (w >= maxCols) break;
        out.append(s, i, len);
        i += len;
        ++w;
    }
    return out;
}

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
inline std::string truncateIsoName(const std::string& name, size_t maxCols) {
    if (utf8Width(name) <= maxCols) return name;

    std::string ext;
    std::string stem = name;
    if (name.size() >= 4) {
        const std::string tail = name.substr(name.size() - 4);
        if (tail[0] == '.' &&
            std::tolower(static_cast<unsigned char>(tail[1])) == 'i' &&
            std::tolower(static_cast<unsigned char>(tail[2])) == 's' &&
            std::tolower(static_cast<unsigned char>(tail[3])) == 'o') {
            ext  = tail;
            stem = name.substr(0, name.size() - 4);
        }
    }

    constexpr size_t MIN_STEM = 4;
    const size_t overhead = 1 + utf8Width(ext);
    const size_t keep = std::max(MIN_STEM, maxCols > overhead ? maxCols - overhead : 0);

    std::string out;
    size_t cols = 0, i = 0;
    while (i < stem.size() && cols < keep) {
        size_t len = 1;
        while (i + len < stem.size() &&
               (static_cast<unsigned char>(stem[i + len]) & 0xC0) == 0x80) ++len;
        out.append(stem, i, len);
        i += len;
        ++cols;
    }
    if (i >= stem.size()) return name;

    out += "\u2026";
    out += ext;
    return out;
}

#endif // WRITE_TEXTWIDTH_H
