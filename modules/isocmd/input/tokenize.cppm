// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.tokenize;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * Parses a raw user input string into valid file indices.
 *
 * Parses whitespace-separated indices and inclusive ranges (e.g., "1-5 8 10")
 * against the provided ISO list. Valid one-based indices are deduplicated;
 * malformed and out-of-range tokens are recorded in the verbose error set.
 *
 * @param input The raw command string from the user.
 * @param isoFiles The current list of available files to index into.
 * @param processedIndices Set to store the final validated integer indices.
 */
void tokenizeInput(
	const std::string& input,
	const std::vector<std::string>& isoFiles,
	std::unordered_set<int>& processedIndices
);

} // extern "C++"
