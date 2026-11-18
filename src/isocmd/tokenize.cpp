// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

// Project Headers
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Checks if a string has a leading zero.
 * @details Used to identify invalid numeric inputs where a leading zero might
 * imply octal or simply be an unsupported format for index selection.
 * @param str The string to check.
 * @return True if the string starts with '0'.
 */
static bool startsWithZero(const std::string& str) {
    return !str.empty() && str[0] == '0';
}

/**
 * @brief Validates if a string consists entirely of decimal digits.
 * @param str The string to check.
 * @return True if the string is non-empty and all characters are digits.
 */
static bool isNumeric(const std::string& str) {
    return !str.empty() && std::all_of(str.begin(), str.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

/// Result of strictly parsing one index (or one half of a range).
enum class ParseResult { Ok, Malformed, OutOfRange };

/**
 * @brief Strictly parses a positive decimal index.
 * @details Accepts digits only: no sign, no whitespace, no leading zero, no
 * trailing garbage, non-empty. Unlike std::stoi, "5abc" is rejected rather
 * than parsed as 5. Does not throw.
 * @param s   The text to parse.
 * @param out Receives the value when the result is Ok.
 * @return Ok, Malformed (bad format), or OutOfRange (too large for int).
 */
static ParseResult parseIndex(std::string_view s, int& out) {
    if (s.empty() || s.front() == '0') return ParseResult::Malformed;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) return ParseResult::Malformed;
    }
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec == std::errc::result_out_of_range) return ParseResult::OutOfRange;
    return ec == std::errc{} ? ParseResult::Ok : ParseResult::Malformed;
}

/**
 * @brief Parses and validates a range token (e.g., "1-10", "10-1") and updates processed indices or error collections.
 *
 * @param token The raw range token string.
 * @param isoFiles The vector of available files for size validation.
 * @param processedIndices Set of successfully parsed unique indices.
 * @param invalidInputs Set storing malformed or structurally invalid tokens.
 * @param invalidRanges Set storing range tokens that exceed file bounds or are out of range.
 */
static void processRangeToken(const std::string& token,
                                const std::vector<std::string>& isoFiles,
                                std::unordered_set<int>& processedIndices,
                                std::unordered_set<std::string>& invalidInputs,
                                std::unordered_set<std::string>& invalidRanges) {
    size_t dashPos = token.find('-');
    // More than one dash, e.g. "1-2-3"
    if (token.find('-', dashPos + 1) != std::string::npos) {
        invalidInputs.insert(token);
        return;
    }

    int start = 0, end = 0;
    std::string_view sv(token);
    ParseResult a = parseIndex(sv.substr(0, dashPos), start);
    ParseResult b = parseIndex(sv.substr(dashPos + 1), end);

    // "5abc-3", "1-2x", "+5-3", "-5", "5-", "0-3"
    if (a == ParseResult::Malformed || b == ParseResult::Malformed) {
        invalidInputs.insert(token);
        return;
    }
    // "1-99999999999", or "1-5000" when only 100 files exist
    if (a == ParseResult::OutOfRange || b == ParseResult::OutOfRange ||
        static_cast<size_t>(start) > isoFiles.size() ||
        static_cast<size_t>(end)   > isoFiles.size()) {
        invalidRanges.insert(token);
        return;
    }

    // start and end are guaranteed >= 1 here (digits only, no leading zero).
    // Ascending and descending ranges both work.
    int lo = std::min(start, end);
    int hi = std::max(start, end);
    for (int i = lo; i <= hi; ++i) {
        processedIndices.insert(i);
    }
}

/**
 * @brief Parses and validates a single numeric index token and updates processed indices or error collections.
 *
 * @param token The raw numeric token string.
 * @param isoFiles The vector of available files for size validation.
 * @param processedIndices Set of successfully parsed unique indices.
 * @param invalidInputs Set storing inputs that failed conversion (e.g. invalid arguments).
 * @param invalidIndices Set storing indices that are out of bounds, lead with zero, or out of range.
 */
static void processSingleToken(const std::string& token,
                                 const std::vector<std::string>& isoFiles,
                                 std::unordered_set<int>& processedIndices,
                                 std::unordered_set<std::string>& invalidInputs,
                                 std::unordered_set<std::string>& invalidIndices) {
    if (startsWithZero(token)) {
        invalidIndices.insert(token);
        return;
    }
    try {
        int num = std::stoi(token);
        if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
            processedIndices.insert(num);
        } else {
            invalidIndices.insert(token);
        }
    } catch (const std::invalid_argument&) {
        invalidInputs.insert(token);
    } catch (const std::out_of_range&) {
        invalidIndices.insert(token);
    }
}

/**
 * @brief Formats an error category collection with appropriate pluralization and ANSI colors.
 *
 * @param sc The semantic UI color theme configuration.
 * @param color The ANSI color code string.
 * @param singular The singular label for the error category.
 * @param plural The plural label for the error category.
 * @param container The container of error token strings.
 * @return std::string The formatted, colorized error message string.
 */
static std::string formatCategory(const SemanticUIColors& sc,
                                  std::string_view color,
                                  std::string_view singular,
                                  std::string_view plural,
                                  const std::unordered_set<std::string>& container) {
    if (container.empty()) return std::string();

    std::ostringstream oss;
    oss << color << (container.size() > 1 ? plural : singular) << ": '";

    bool first = true;
    for (const auto& item : container) {
        if (!first) oss << ", ";
        oss << item;
        first = false;
    }
    oss << "'" << sc.reset;
    return oss.str();
}

/**
 * @brief Parses user input strings into a set of unique file indices.
 * @details Supports individual indices (e.g., "5"), ranges (e.g., "1-10"),
 * and reverse ranges (e.g., "10-1"). Performs extensive validation against
 * the current file list size and handles various formatting errors.
 *
 * @param input The raw input string from the user.
 * @param isoFiles The vector of available files to validate indices against.
 * @param processedIndices Set to store the successfully parsed unique indices.
 */
void tokenizeInput(const std::string& input,
                   const std::vector<std::string>& isoFiles,
                   std::unordered_set<int>& processedIndices) {

    std::istringstream iss(input);
    std::string token;

    // Categorize errors to provide specific feedback
    std::unordered_set<std::string> invalidInputs;
    std::unordered_set<std::string> invalidIndices;
    std::unordered_set<std::string> invalidRanges;

    while (iss >> token) {
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            processRangeToken(token, isoFiles, processedIndices, invalidInputs, invalidRanges);
        } else if (isNumeric(token)) {
            processSingleToken(token, isoFiles, processedIndices, invalidInputs, invalidIndices);
        } else {
            invalidInputs.insert(token);
        }
    }

    SemanticUIColors sc = resolveVerboseTheme();

    std::string errInputMsg = formatCategory(sc, sc.error, "Invalid input", "Invalid inputs", invalidInputs);
    if (!errInputMsg.empty()) {
        verboseSets.uniqueErrorTokenMessages.insert(errInputMsg);
    }

    std::string errIndexMsg = formatCategory(sc, sc.error, "Invalid index", "Invalid indexes", invalidIndices);
    if (!errIndexMsg.empty()) {
        verboseSets.uniqueErrorTokenMessages.insert(errIndexMsg);
    }

    std::string errRangeMsg = formatCategory(sc, sc.error, "Invalid range", "Invalid ranges", invalidRanges);
    if (!errRangeMsg.empty()) {
        verboseSets.uniqueErrorTokenMessages.insert(errRangeMsg);
    }
}
