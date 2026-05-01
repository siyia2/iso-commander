// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VERBOSE_H
#define VERBOSE_H

// C++ Standard Library Headers
#include <csignal>
#include <functional>
#include <unordered_set>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

// Third-Party Library Headers
#include <readline/history.h>

// C / System Headers
#include <termios.h>

// --- Output & Logging Functions ---

/**
 * Prints set contents based on the provided verbosity level.
 */
void verbosePrint(
    std::unordered_set<std::string>& primarySet,
    std::unordered_set<std::string>& secondarySet,
    std::unordered_set<std::string>& tertiarySet,
    std::unordered_set<std::string>& errorSet,
    int verboseLevel
);

/**
 * Resets all tracking sets to clear state for the next operation.
 */
void resetVerboseSets(
    std::unordered_set<std::string>& processedErrors,
    std::unordered_set<std::string>& successOuts,
    std::unordered_set<std::string>& skippedOuts,
    std::unordered_set<std::string>& failedOuts
);

/**
 * Outputs a list of unique error messages collected during execution.
 */
void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages);


// --- Search & Discovery ---

/**
 * Logs directory-specific errors found during a scan.
 */
void verboseFind(
    std::unordered_set<std::string>& invalidDirectoryPaths,
    const std::vector<std::string>& directoryPaths,
    std::unordered_set<std::string>& processedErrorsFind
);

/**
 * Summarizes the results of a file search operation.
 */
void verboseSearchResults(
    const std::string& fileExtension,
    std::unordered_set<std::string>& fileNames,
    std::unordered_set<std::string>& invalidDirectoryPaths,
    bool newFilesFound,
    bool list,
    int currentCacheOld,
    const std::vector<std::string>& files,
    const std::chrono::high_resolution_clock::time_point& start_time,
    std::unordered_set<std::string>& processedErrorsFind,
    std::vector<std::string>& directoryPaths
);


// --- Filesystem Operations & Database ---

/**
 * Specific error reporting for Copy (Cp), Move (Mv), and Remove (Rm) operations.
 */
void reportErrorCpMvRm(
    const std::string& errorType,
    const std::string& srcDir,
    const std::string& srcFile,
    const std::string& destDir,
    const std::string& errorDetail,
    const std::string& operation,
    std::vector<std::string>& verboseErrors,
    std::atomic<size_t>* failedTasks,
    std::atomic<bool>& operationSuccessful,
    const std::function<void()>& batchInsertFunc
);

/**
 * Finalizes the session, saves state, and reports results to the database layer.
 */
void saveAndReportResultsForDatabase(
    std::vector<std::string>& allIsoFiles,
    std::atomic<size_t>& totalFiles,
    std::vector<std::string>& validPaths,
    std::unordered_set<std::string>& invalidPaths,
    std::unordered_set<std::string>& uniqueErrorMessages,
    bool& promptFlag,
    int& maxDepth,
    bool& filterHistory,
    const std::chrono::high_resolution_clock::time_point& start_time,
    std::atomic<bool>& newISOFound
);

#endif // VERBOSE_H
