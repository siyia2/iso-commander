// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VERBOSE_H
#define VERBOSE_H

// C++ Standard Library Headers
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
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
 * All concurrent writes must be protected by GlobalMutexes::globalSetsMutex.
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
 * Summarizes the results of an image file search operation.
 *
 * Prints the total number of files processed (@p totalFiles), the captured
 * verboseFind() output (invalid paths, traversal errors, interruption
 * message), the found/cached summary and the elapsed time.
 *
 * The whole block is produced by an internal render lambda that clears the
 * screen and prints it. pressEnterToContinueWithRepaint() invokes the
 * lambda once at entry and again after clearing the screen on every
 * SIGWINCH, so a resize during the wait redraws the block from a known
 * top instead of leaving a stale frame. No ListResizeWatcher is
 * installed; the prompt owns resize handling.
 *
 * If @p repaintCallerHeader is non-empty, the render lambda calls it
 * before printing the block. Use it to reprint the caller's prompt line
 * (the question the user just answered) so it survives a resize.
 *
 * verboseFind() mutates its set arguments (clears them), so it runs once,
 * before the wait, with its std::cout/std::cerr writes redirected into an
 * ostringstream; the captured text is replayed by the render lambda.
 *
 * @param totalFiles           Aggregate count of regular files processed
 *                             by the scan, as filled in by findFiles().
 * @param repaintCallerHeader  Optional callback that reprints the caller's
 *                             screen header (e.g. the FolderPaths prompt
 *                             line) on a resize during the wait. When
 *                             empty, the block starts from a cleared
 *                             screen with no caller context.
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
    const std::vector<std::string>& directoryPaths,
    const std::atomic<size_t>& totalFiles,
    const std::function<void()>& repaintCallerHeader = {}
);


// --- Database ---

/**
 * Finalizes an ISO database refresh: reports results and syncs global state.
 *
 * Last stage of the database update pipeline. Reconciles globalIsoFileList
 * with disk if @p newISOFound was set by the caller, persists the new list
 * via saveToDatabase(), computes the delta count, and prints a color-coded
 * outcome from one of five branches: Cancelled; save failed with files and
 * delta; no valid paths; files present but no delta (save not attempted or
 * failed); no ISOs found; or successful import with the delta count.
 *
 * All one-shot side effects (loadFromDatabase, saveToDatabase, elapsed
 * time, delta count) run before the wait. Only the on-screen block is
 * produced by an internal render lambda, which clears the screen first and
 * then prints everything (including the "Total files processed" line the
 * live progress counter would otherwise leave behind). The caller must
 * therefore NOT print any part of the block itself — the lambda is the
 * sole producer of the screen. pressEnterToContinueWithRepaint() invokes
 * the lambda once at entry and again after clearing the screen on every
 * SIGWINCH, so a resize during the wait redraws the block from a known
 * top. No ListResizeWatcher is installed; the prompt owns resize handling.
 *
 * If @p repaintCallerHeader is non-empty, the render lambda calls it
 * before printing the block. Use it to reprint the caller's prompt line
 * (the question the user just answered) so it survives a resize. Both the
 * early-save path (all paths invalid) and the post-scan path in
 * refreshForDatabase() pass a callback.
 *
 * @param allIsoFiles          Discovered ISO paths to be saved.
 * @param totalFiles           Atomic count of files scanned; reprinted
 *                             after the screen clear, and gates the
 *                             zero-case line.
 * @param validPaths           Base directories successfully traversed;
 *                             empty triggers the "Lack of valid paths"
 *                             branch.
 * @param invalidPaths         Directories skipped due to permissions or
 *                             existence errors.
 * @param uniqueErrorMessages  Deduplicated system-level I/O errors.
 * @param newISOFound          On entry, whether the scan produced delta
 *                             changes (gates the loadFromDatabase call).
 *                             Passed by pointer to saveToDatabase, which
 *                             may modify it.
 * @param start_time           Timestamp of the refresh origin, used to
 *                             compute elapsed time.
 * @param repaintCallerHeader  Optional callback that reprints the caller's
 *                             screen header (e.g. the FolderPaths prompt
 *                             line) on a resize during the wait. When
 *                             empty, the block starts from a cleared
 *                             screen with no caller context.
 */
void saveAndReportResultsForDatabase(
    std::vector<std::string>& allIsoFiles,
    std::atomic<size_t>& totalFiles,
    std::vector<std::string>& validPaths,
    std::unordered_set<std::string>& invalidPaths,
    std::unordered_set<std::string>& uniqueErrorMessages,
    bool& newISOFound,
    const std::chrono::high_resolution_clock::time_point& start_time,
    const std::function<void()>& repaintCallerHeader = {}
);

#endif // VERBOSE_H
