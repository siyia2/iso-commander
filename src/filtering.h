// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERING_H
#define FILTERING_H

// C++ Standard Library Headers
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declaration of shared state (needed for live main-list rendering)
struct RefreshState;

/**
 * DATA STRUCTURES
 */

/**
 * @brief Represents a single search term within a query: its original text,
 * its match-case mode, and whether it is negated.
 *
 * Matching is "smart case" per term. If @c original contains an uppercase
 * letter, @c isCaseSensitive is true and the term is searched against the
 * entry's original text. Otherwise the term is case-insensitive and its
 * lowercased form (@c lower) is searched against the entry's lowercased text.
 * @c lower is populated only for case-insensitive terms.
 *
 * A negated term (query syntax: a leading unescaped '!', e.g. "!boot") inverts
 * the match: the term is satisfied when it does NOT occur in the entry.
 * @c original and @c lower hold the term text without the '!' prefix, and
 * smart-case applies to negated terms the same way.
 */
struct QueryToken {
    std::string original;            ///< Term text as typed (escapes resolved, '!' prefix removed).
    std::string lower;               ///< Lowercased @c original; set only when !isCaseSensitive.
    bool isCaseSensitive = false;    ///< True if @c original contains an uppercase letter.
    bool negated = false;            ///< True if the term must NOT occur in the entry.
};

/**
 * @brief Stores a single level of filter state for nested filtering support.
 */
struct FilteringState {
    std::vector<size_t> originalIndices;
    std::string query;
    bool isFiltered;
    bool namesOnly = false;
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

    // --- Live main-list rendering (optional) ---
    // When listType is non-empty (and pendingIndices/hasPendingProcess/state
    // are all set), the FilterTerms prompt repaints the real printList
    // output on every keystroke, narrowing/widening it live as the query
    // changes, instead of leaving the list static until Enter. Leaving
    // listType empty disables this and keeps the classic Enter-only prompt.
    std::string listType{};       // e.g. "ISO_FILES", "IMAGE_FILES", "MOUNTED_ISOS"
    std::string listSubType{};
    std::vector<std::string>* pendingIndices = nullptr;
    bool* hasPendingProcess = nullptr;
    std::shared_ptr<RefreshState> state = nullptr;
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

    // Optional: see FilterContext above. Populate all four to enable live
    // repainting of the real list while the user types.
    std::string listType{};
    std::string listSubType{};
    std::vector<std::string>* pendingIndices = nullptr;
    bool* hasPendingProcess = nullptr;
    std::shared_ptr<RefreshState> state = nullptr;
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
 * @brief Filters file indices based on a search query using SIMD-accelerated
 * substring search.
 *
 * @param precomputedLower Optional parallel array, same size as @p files, holding
 *        each entry of @p files already lowercased. When a caller runs the same
 *        @p files list through many queries in a row (e.g. the live filter
 *        preview, once per keystroke), passing this lets it lowercase each
 *        entry once instead of on every call. Defaults to nullptr, which
 *        preserves the original per-call lowercasing behavior.
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query,
                                        const std::vector<std::string>* precomputedLower = nullptr);

/**
 * @brief Refreshes the active filter after a list reload, keeping existing results.
 *
 * Entries already shown are kept if they still exist; only newly discovered ISOs
 * are tested against every level, each with the text mode it was committed with.
 * Clears the filter if nothing remains.
 */
void refreshFilterKeepingResults(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered);

/**
 * @brief Refreshes source indices for the current filtered results.
 */
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles);

#endif // FILTERING_H
