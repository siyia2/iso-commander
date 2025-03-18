// SPDX-License-Identifier: GNU General Public License v2.0

#ifndef FILTERING_H
#define FILTERING_H


// Structure to holdfor global filtering state for lists
struct FilteringState {
    std::vector<size_t> originalIndices;  // Current mapping to original indices
    bool isFiltered;                      // Whether filtering is active
};

extern std::vector<FilteringState> filteringStack;

#endif // FILTERING_H
