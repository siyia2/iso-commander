// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SEARCHES_H
#define SEARCHES_H

// C++ Standard Library Headers
#include <string>
#include <atomic>
#include <cstdint>

/**
 * SEARCH DOCUMENTATION & HELP
 */

/**
 * @brief Displays usage help for search operations.
 * @param isCpMv Whether the current context is Copy/Move.
 * @param import2ISO Whether the context is ISO conversion.
 */
void helpSearches(bool isCpMv, bool import2ISO);


/**
 * DATABASE MANAGEMENT & TELEMETRY
 */

/**
 * @brief Retrieves and prints statistics about the SQLite database.
 * @param databaseFilePath Path to the .db file.
 * @param maxDatabaseSize Size limit for warning thresholds.
 */
void displayDatabaseStatistics(
    const std::string& databaseFilePath, 
    std::uintmax_t maxDatabaseSize
);

/**
 * @brief Processes special CLI switches related to database maintenance.
 * 
 * Handles internal commands like --refresh or --cleanup passed via the
 * search input field.
 */
void databaseSwitches(
    std::string& inputSearch, 
    const bool& promptFlag, 
    const int& maxDepth, 
    const bool& filterHistory, 
    std::atomic<bool>& newISOFound
);

#endif // SEARCHES_H
