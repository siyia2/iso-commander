// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERING_H
#define FILTERING_H

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Manages the state of filtered list views, allowing for nested filtering 
 * levels by mapping visible items back to their original data positions.
 */
struct FilteringState {
    /** @brief Collection of indices pointing to the original, unfiltered dataset. */
    std::vector<size_t> originalIndices;

    /** @brief Flag indicating if a filter is currently applied to this state. */
    bool isFiltered;
};

/** @brief Global stack used to track and revert through multiple levels of filtering. */
extern std::vector<FilteringState> filteringStack;

#endif // FILTERING_H
