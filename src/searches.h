// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SEARCHES_H
#define SEARCHES_H

// C++ Standard Library Headers
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

/**
 * Displays usage help for search operations.
 * @param isCpMv Whether the current context is Copy/Move.
 * @param import2ISO Whether the context is ISO conversion.
 */
void helpSearches(bool isCpMv, bool import2ISO);

/**
 * Retrieves and prints statistics about the SQLite database.
 */
void displayDatabaseStatistics(
    const std::string& databaseFilePath, 
    std::uintmax_t maxDatabaseSize
);

/**
 * Processes special CLI switches/commands related to database management.
 */
void databaseSwitches(
    std::string& inputSearch, 
    const bool& promptFlag, 
    const int& maxDepth, 
    const bool& filterHistory, 
    std::atomic<bool>& newISOFound
);

#endif // SEARCHES_H
