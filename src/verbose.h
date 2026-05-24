// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VERBOSE_H
#define VERBOSE_H

// C++ Standard Library Headers
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * @brief Holds verbose output sets for operation results and user input.
 *
 * @details Aggregates messages produced during batch operations into four
 * categorised sets, which are consumed by verbosePrint() at the end of
 * each operation.
 *
 *   - operationCompleted        Success messages.
 *   - operationFailed           Failure messages.
 *   - operationSkipped          Skip messages.
 *   - uniqueErrorTokenMessages  Deduplicated user-input validation error tokens.
 *
 * Declared inline so the single definition lives in the header.
 * All concurrent writes must be protected by GlobalConcurrency::globalSetsMutex.
 */
struct VerboseSets {
    std::unordered_set<std::string> operationCompleted;
    std::unordered_set<std::string> operationFailed;
    std::unordered_set<std::string> operationSkipped;
    std::unordered_set<std::string> uniqueErrorTokenMessages;
};

inline VerboseSets verboseSets;

/**
 * @brief Resets the global verbose tracking state.
 *
 * Clears all entries from the global verboseSets instance, including
 * completed, failed, and skipped operations, plus unique error messages.
 */
inline void clearGlobalVerboseSets() {
    verboseSets.operationCompleted.clear();
    verboseSets.operationFailed.clear();
    verboseSets.operationSkipped.clear();
    verboseSets.uniqueErrorTokenMessages.clear();
}

/**
 * @brief Shared reporting context for file operations (copy/move/delete).
 *
 * Collects verbose success/error messages, updates atomic task counters,
 * tracks overall success state, and triggers batched message flushing.
 *
 * @note Non-owning references/pointers; caller must ensure lifetime.
 */
struct OperationReporter {
    std::vector<std::string>& verboseIsos;
    std::vector<std::string>& verboseErrors;
    std::atomic<size_t>* completedTasks;
    std::atomic<size_t>* failedTasks;
    std::atomic<bool>& operationSuccessful;
    const std::function<void()>& batchInsertMessages;
};

// --- Output & Logging Functions ---

/**
 * Prints set contents based on the provided verbosity level.
 */
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet,
std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int printType);

/**
 * Outputs a list of unique error messages collected during execution.
 */
void displayErrors();


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
 * Summarizes the results of image file search operation.
 */
void verboseImageSearchResults(
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
    std::string_view errorType,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDir,
    std::string_view errorDetail,
    std::string_view operation,
    OperationReporter& reporter);
/**
 * Finalizes the session, saves state, and reports results to the database layer.
 */
void saveAndReportResultsForDatabase(
    std::vector<std::string>& allIsoFiles,
    std::atomic<size_t>& totalFiles,
    std::vector<std::string>& validPaths,
    std::unordered_set<std::string>& invalidPaths,
    std::unordered_set<std::string>& uniqueErrorMessages,
    bool& newISOFound,
    const std::chrono::high_resolution_clock::time_point& start_time
);

#endif // VERBOSE_H
