// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SELECT_H
#define SELECT_H

/**
 * @brief Shared display state for the ISO list view and its background refresh thread.
 *
 * Owned via shared_ptr by selectForIsoFiles and any detached refreshListAfterAutoUpdate
 * threads, ensuring the data outlives the caller without dangling references.
 */
struct RefreshState {
    std::vector<std::string> filteredFiles;
    std::vector<std::string> pendingIndices;
    bool isFiltered;
    bool hasPendingProcess;
    bool umountMvRmBreak;
    std::string listSubtype;
    size_t currentPage;
    size_t originalPage;
};

#endif // SELECT_H
