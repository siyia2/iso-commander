// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Project Headers
#include "../themes.h"
#include "../tokenize.h"
#include "../verbose.h"

/**
 * @brief Checks if a string has a leading zero.
 * @details Used to identify invalid numeric inputs where a leading zero might
 * imply octal or simply be an unsupported format for index selection.
 * @param str The string to check.
 * @return True if the string starts with '0'.
 */
bool startsWithZero(const std::string& str) {
    return !str.empty() && str[0] == '0';
}

/**
 * @brief Validates if a string consists entirely of decimal digits.
 * @param str The string to check.
 * @return True if all characters are digits.
 */
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}

/**
 * @brief Parses user input strings into a set of unique file indices.
 * @details Supports individual indices (e.g., "5"), ranges (e.g., "1-10"),
 * and reverse ranges (e.g., "10-1"). Performs extensive validation against
 * the current file list size and handles various formatting errors.
 * * @param input The raw input string from the user.
 * @param isoFiles The vector of available files to validate indices against.
 * @param uniqueErrorTokenMessages Set to store color-coded, categorized error strings.
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
		// Check for range format (start-end)
		size_t dashCount = std::count(token.begin(), token.end(), '-');
		if (dashCount > 1) {
			invalidInputs.insert(token);
			continue;
		}

		size_t dashPos = token.find('-');
		if (dashPos != std::string::npos) {
			std::string startStr = token.substr(0, dashPos);
			std::string endStr = token.substr(dashPos + 1);
			if (startsWithZero(startStr) || startsWithZero(endStr)) {
				invalidInputs.insert(token);
				continue;
			}
			int start, end;
			try {
				start = std::stoi(startStr);
				end = std::stoi(endStr);
			} catch (const std::invalid_argument&) {
				invalidInputs.insert(token);
				continue;
			} catch (const std::out_of_range&) {
				invalidRanges.insert(token);
				continue;
			}
			// Validate range bounds
			bool boundsInvalid = (start < 1 || static_cast<size_t>(start) > isoFiles.size() ||
								  end < 1 || static_cast<size_t>(end) > isoFiles.size());
			if (boundsInvalid) {
				invalidRanges.insert(token);
				continue;
			}
			// Process the range (supports both ascending and descending)
			int step = (start <= end) ? 1 : -1;
			for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
				processedIndices.insert(i);
			}
		}
		// Handle single numeric index
		else if (isNumeric(token)) {
			if (startsWithZero(token)) {
				invalidIndices.insert(token);
				continue;
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
		// Non-numeric, non-range tokens are garbage
		else {
			invalidInputs.insert(token);
		}
	}

    SemanticUIColors sc = resolveVerboseTheme();

    /**
	* @brief Formats error categories with appropriate pluralization and ANSI colors.
	*/
	auto formatCategory = [&sc](std::string_view color, std::string_view singular,
							 std::string_view plural, const auto& container) {
		if (container.empty()) return std::string();

		std::ostringstream oss;
		// Use the specific singular or plural strings passed in
		oss << color << (container.size() > 1 ? plural : singular) << ": '";

		bool first = true;
		for (const auto& item : container) {
			if (!first) oss << ", ";
			oss << item;
			first = false;
		}
		oss << "'" << sc.reset;
		return oss.str();
	};

	if (!invalidInputs.empty()) {
		verboseSets.uniqueErrorTokenMessages.insert(formatCategory(sc.error, "Invalid input", "Invalid inputs", invalidInputs));
	}

	if (!invalidIndices.empty()) {
		verboseSets.uniqueErrorTokenMessages.insert(formatCategory(sc.error, "Invalid index", "Invalid indexes", invalidIndices));
	}

	if (!invalidRanges.empty()) {
		verboseSets.uniqueErrorTokenMessages.insert(formatCategory(sc.error, "Invalid range", "Invalid ranges", invalidRanges));
	}
}
