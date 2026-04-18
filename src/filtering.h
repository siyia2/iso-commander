// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERING_H
#define FILTERING_H

/**
 * @brief Stores a single level of filter state for nested filtering support.
 * @details Each entry on the filtering stack captures a snapshot of which items
 * were visible at that filter level, mapping them back to their positions in the
 * original unfiltered dataset. Popping an entry restores the previous filter level.
 */
struct FilteringState {
    /** @brief Indices into the original unfiltered dataset for each currently visible item. */
    std::vector<size_t> originalIndices;
    /** @brief True if a filter is active at this stack level, false if showing all items. */
    bool isFiltered;
};

/**
 * @brief Global stack tracking all active filter levels in LIFO order.
 * @details Pushed when a new filter is applied, popped when the user clears or
 * narrows a filter. An empty stack means no filtering is active.
 */
inline std::vector<FilteringState> filteringStack;

/**
 * @brief Binds all mutable state needed by a single filter operation.
 * @details Passed by reference into filter loop functions so they can update
 * the caller's state directly without returning multiple values. The optional
 * fields are only meaningful in unmount mode.
 */
struct FilterContext {
    /** @brief The list of file paths currently visible to the user. */
    std::vector<std::string>& files;
    /** @brief True if a filter is currently applied to @p files. */
    bool&                      isFiltered;
    /** @brief Set to true when the display needs a full redraw after filtering. */
    bool&                      needsClrScrn;
    /** @brief Set to true when the current filter term should be saved to history. */
    bool&                      filterHistory;
    /** @brief The page index to reset or update after a filter is applied. */
    size_t&                    currentPage;
    /** @brief If non-null, overrides the default source list used as the filter input. */
    const std::vector<std::string>* sourceOverride = nullptr;
    /** @brief True when operating in unmount mode, affecting source list selection. */
    bool isUnmount            = false;
    /** @brief Mirrors @c displayConfig::toggleFullListUmount for unmount list display. */
    bool toggleFullListUmount = false;
};

/**
 * @brief Configuration passed to @c runSharedFilterFlow to drive a filter operation.
 * @details Decouples the two filter entry points (@c handleFilteringForISO and
 * @c handleFilteringConvert2ISO) from the shared implementation. All pointer fields
 * must be non-null except @p sourceOverride and @p need2Sort, which are optional.
 */
struct FilterCallConfig {
    /** @brief The file list to filter in place. Must not be null. */
    std::vector<std::string>*        files          = nullptr;
    /** @brief If non-null, used as the filter source instead of the default global list. */
    const std::vector<std::string>*  sourceOverride = nullptr;
    /** @brief Human-readable name of the operation shown in the filter prompt. */
    std::string                      operation;
    /** @brief Raw ANSI color code applied to @p operation text in the prompt. */
    std::string_view                 operationColor;
    /** @brief Pointer to the caller's isFiltered flag. Must not be null. */
    bool*                            isFiltered     = nullptr;
    /** @brief Pointer to the caller's needsClrScrn flag. Must not be null. */
    bool*                            needsClrScrn   = nullptr;
    /** @brief Pointer to the caller's filterHistory flag. Must not be null. */
    bool*                            filterHistory  = nullptr;
    /** @brief If non-null, set to true when the result list needs resorting. */
    bool*                            need2Sort      = nullptr;
    /** @brief Pointer to the caller's current page index. Must not be null. */
    size_t*                          currentPage    = nullptr;
    /** @brief Passed through to @c FilterContext::isUnmount. */
    bool                             isUnmount      = false;
    /** @brief Passed through to @c FilterContext::toggleFullListUmount. */
    bool                             toggleFullList = false;
};

/**
 * @brief Represents a single search token with precomputed Boyer-Moore tables
 * 
 * Stores both case-sensitive and case-insensitive versions of the pattern
 * with their corresponding heuristic tables for efficient searching.
 */
struct QueryToken {
    std::string original;
    std::string lower;
    bool        isCaseSensitive;

    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;

    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};

#endif // FILTERING_H
