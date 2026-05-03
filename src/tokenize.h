// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TOKENIZE_H
#define TOKENIZE_H

// C++ Standard Library Headers
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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

#endif // TOKENIZE_H
