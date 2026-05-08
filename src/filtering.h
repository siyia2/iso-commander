// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERING_H
#define FILTERING_H

// C++ Standard Library Headers
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/**
 * DATA STRUCTURES
 */

/**
 * @brief Represents a single search token with precomputed Boyer-Moore tables.
 */
struct QueryToken {
    std::string original;
    std::string lower;
    bool isCaseSensitive;

    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;

    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};

/**
 * @brief Stores a single level of filter state for nested filtering support.
 */
struct FilteringState {
    std::vector<size_t> originalIndices;
    std::string query;
    bool isFiltered;
};

/**
 * @brief Binds all mutable state needed by a single filter operation.
 */
struct FilterContext {
    std::vector<std::string>& files;
    bool& isFiltered;
    bool& needsClrScrn;
    bool& filterHistory;
    size_t& currentPage;
    const std::vector<std::string>* sourceOverride = nullptr;
    bool isUnmount = false;
    bool toggleFullListUmount = false;
};

/**
 * @brief Configuration passed to runSharedFilterFlow to drive a filter operation.
 */
struct FilterCallConfig {
    std::vector<std::string>* files = nullptr;
    const std::vector<std::string>* sourceOverride = nullptr;
    std::string operation;
    std::string_view operationColor;
    bool* isFiltered = nullptr;
    bool* needsClrScrn = nullptr;
    bool* filterHistory = nullptr;
    bool* need2Sort = nullptr;
    size_t* currentPage = nullptr;
    bool isUnmount = false;
    bool toggleFullList = false;
};

/**
 * GLOBAL STATE
 */

/**
 * @brief Global stack tracking all active filter levels in LIFO order.
 */
inline std::vector<FilteringState> filteringStack;


/**
 * FILTERING LOGIC & SYNC
 */

/**
 * @brief Filters file indices based on a search query using the Boyer-Moore algorithm.
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query);

/**
 * @brief Synchronizes the filtered results by iteratively applying the filtering stack.
 */
void syncFilteringStackForIso(
    const std::vector<std::string>& globalIsoFileList, 
    std::vector<FilteringState>& filteringStack, 
    std::vector<std::string>& filteredFiles, 
    bool& isFiltered
);

#endif // FILTERING_H
