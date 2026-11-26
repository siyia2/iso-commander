// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.tokenize;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * Parses a raw user input string into valid file indices.
 *
 * Takes a string (e.g., "1-5, 8, 10") and maps it against the provided
 * list of ISO files, tracking which indices were successfully processed
 * and logging any range or formatting errors.
 *
 * @param input The raw command string from the user.
 * @param isoFiles The current list of available files to index into.
 * @param uniqueErrorMessages Set to collect any parsing or range errors.
 * @param processedIndices Set to store the final validated integer indices.
 */
void tokenizeInput(
	const std::string& input,
	const std::vector<std::string>& isoFiles,
	std::unordered_set<int>& processedIndices
);

} // extern "C++"
