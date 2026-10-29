// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERING_H
#define FILTERING_H

// C++ Standard Library Headers
#include <cstddef>
#include <functional>
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
 * @brief A single search term within a query.
 *
 * Matching is "smart case" per term: if @c original contains an uppercase
 * letter, @c isCaseSensitive is true and the term is matched against the
 * entry's original text. Otherwise matching is case-insensitive and the
 * lowercased form (@c lower) is matched against the entry's lowercased text.
 * @c lower is populated only for case-insensitive terms.
 *
 * A negated term (leading unescaped '!' in the query, e.g. "!boot") inverts
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
 * @brief A single level of filter state, enabling nested (stacked) filtering.
 */
struct FilteringState {
    std::vector<size_t> originalIndices;
    std::string query;
    bool isFiltered;
    bool namesOnly = false;
};

/**
 * @brief All mutable state needed to perform a single filter operation.
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
    std::function<void()> repaintList = nullptr;

    // Live main-list rendering: when listType is non-empty and pendingIndices,
    // hasPendingProcess, and state are all set, the filter prompt repaints the
    // real printList output on every keystroke, narrowing/widening it as the
    // query changes. Leaving listType empty disables this and keeps the
    // Enter-only prompt.
    std::string listType{};       // e.g. "ISO_FILES", "IMAGE_FILES", "MOUNTED_ISOS"
    std::string listSubType{};
    std::vector<std::string>* pendingIndices = nullptr;
    bool* hasPendingProcess = nullptr;
    std::shared_ptr<RefreshState> state = nullptr;
};

/**
 * @brief Configuration for runSharedFilterFlow, describing one filter operation.
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
    std::function<void()> repaintList = nullptr;

    // Optional live rendering: populate all four to repaint the real list
    // while the user types (see FilterContext above).
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
 * @brief Stack of all active filter levels, in LIFO order.
 */
inline std::vector<FilteringState> filteringStack;


/**
 * FILTERING LOGIC & SYNC
 */

/**
 * @brief Returns the indices of @p files matching @p query, using
 * SIMD-accelerated substring search.
 *
 * @param precomputedLower Optional array parallel to @p files holding each
 *        entry already lowercased. Callers that run the same list through many
 *        queries in a row (e.g. live filter preview, once per keystroke) can
 *        pass this to lowercase each entry once instead of per call. When
 *        nullptr, entries are lowercased on every call.
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query,
                                        const std::vector<std::string>* precomputedLower = nullptr);

/**
 * @brief Refreshes an active filter after a list reload, preserving results.
 *
 * Entries already shown are kept if they still exist; newly discovered ISOs
 * are tested against every filter level, each using the text mode it was
 * committed with. Clears the filter if nothing remains.
 */
void refreshFilterKeepingResults(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered);

/**
 * @brief Rebuilds the source indices for the current filtered results.
 */
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles);

#endif // FILTERING_H
