// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SELECT_H
#define SELECT_H

// C++ Standard Library Headers
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Shared display state for the ISO list view and its background refresh thread.
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
    std::mutex importMutex;
    std::condition_variable importCV;
    std::atomic<bool> isImportRunning{false};
    std::atomic<bool> isWatcherRunning{false};
    std::atomic<bool> stopImport{false};
    std::condition_variable workerCV;
    std::mutex workerMutex;
    std::atomic<size_t> activeWorkers{0};
};

// --- Data Loading & List Display ---

/**
 * Loads and displays the standard ISO list from the database/cache.
 */
bool loadAndDisplayIso(
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    const std::string &listSubType,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/**
 * Specialized loader for currently mounted ISOs.
 */
bool loadAndDisplayMountedISOs(
    std::vector<std::string> &isoDirs,
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/**
 * Loads and displays non-ISO image files (MDF, NRG, etc.) for conversion.
 */
void loadAndDisplayImageFiles(
    std::vector<std::string> &files,
    const std::string &fileType,
    bool &need2Sort,
    bool &isFiltered,
    bool &list,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    std::shared_ptr<RefreshState> state
);


// --- Input Processing & Action Handlers ---

/**
 * Processes user selection for Mount or Unmount operations.
 */
void processInputForMountOrUmount(
    const std::string &input,
    const std::vector<std::string> &files,
    bool &operationBreak,
    bool &verbose,
    bool isUnmount
);

/**
 * Processes user selection for Copy, Move, or Remove operations.
 */
void processInputForCpMvRm(
    const std::string &input,
    const std::vector<std::string> &isoFiles,
    const std::string &process,
    bool &umountMvRmBreak,
    bool &filterHistory,
    bool &verbose,
    std::atomic<bool> &newISOFound
);

/**
 * Processes user selection for converting images to ISO.
 */
void processInputForConversions(
    const std::string &input,
    std::vector<std::string> &fileList,
    const bool &modeMdf,
    const bool &modeNrg,
    const bool &modeChd,
    const bool &modeDaa,
    bool &verbose,
    bool &needsClrScrn,
    std::atomic<bool> &newISOFound
);

/**
 * Handles specialized logic for writing an ISO to a USB device.
 */
void writeToUsb(
    const std::string &input,
    const std::vector<std::string> &isoFiles
);


// --- Filtering & Pagination ---

/**
 * Manages pagination state and help text display.
 */
bool processPaginationHelpAndDisplay(
    const std::string &command,
    size_t &totalPages,
    size_t &currentPage,
    bool &isFiltered,
    bool &needsClrScrn,
    const bool isMount,
    const bool isUnmount,
    const bool isWrite,
    const bool isConversion,
    bool &need2Sort,
    std::atomic<bool> &isAtISOList
);

/**
 * Generic string filtering for the ISO list.
 */
bool handleFilteringForISO(
    const std::string &inputString,
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    bool &needsClrScrn,
    bool &filterHistory,
    const std::string &operation,
    const std::string &operationColor,
    const std::vector<std::string> &isoDirs,
    bool isUnmount,
    size_t &currentPage
);

/**
 * Filtering specifically for the conversion list.
 */
void handleFilteringConvert2ISO(
    const std::string &mainInputString,
    std::vector<std::string> &files,
    const std::string &operation,
    bool &isFiltered,
    bool &needsClrScrn,
    bool &filterHistory,
    bool &need2Sort,
    size_t &currentPage
);


// --- Utility & Cleanup ---

/**
 * Finalizes and displays the results of a selection session.
 */
void handleSelectIsoFilesResults(
    const std::string &operation,
    bool &verbose,
    bool isMount,
    bool &isFiltered,
    bool &umountMvRmBreak,
    bool isUnmount,
    bool &needsClrScrn
);

/**
 * Helper for case-insensitive string comparisons.
 */
void toLowerInPlace(std::string &str);

#endif // SELECT_H
