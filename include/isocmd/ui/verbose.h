// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VERBOSE_H
#define VERBOSE_H

// C++ Standard Library Headers
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// Outcome category of one mount result.
enum class MountRecKind : uint8_t { Completed, Skipped, Failed };

/**
 * @brief Compact result of one mount attempt.
 *
 * Mount results used to be stored as fully formatted, colour-coded lines (about
 * 300 bytes each). A record keeps only what is needed to rebuild the line:
 * the image path and a code. The mount point is derived from the path, so it is
 * not stored. The line is formatted only when it is printed
 * (see forEachMountLine()).
 *
 * @c code is a pointer to a string with static storage duration: the failure
 * reason for Failed records, the filesystem type for Completed records
 * (interned), and unused for Skipped records.
 */
struct MountRecord {
    std::string  isoPath;
    const char*  code = nullptr;
    MountRecKind kind = MountRecKind::Completed;
};

/**
 * @brief Compact result of one unmount attempt (same idea as MountRecord).
 *
 * @c messageKey is one of the string literals "success", "error", "cancel",
 * "root_error"; the line is built from it and @c mountPoint when printed.
 */
struct UmountRecord {
    std::string mountPoint;
    const char* messageKey = nullptr;
    bool        completed  = false;
};

/// Number of stored unmount records (completed or failed).
size_t countUmountRecords(bool completed);

/// Like forEachMountLine(), for unmount results. Defined in umount.cpp.
void forEachUmountLine(bool completed, const std::function<void(std::string_view)>& fn);

/// Number of stored mount records of the given kind.
size_t countMountRecords(MountRecKind kind);

/**
 * @brief Calls @p fn once per distinct mount result of @p kind, as a formatted line.
 *
 * Lines are produced in the same order verbosePrint() uses for string sets
 * (case-insensitive, by displayed path), duplicates are dropped, and only one
 * line exists in memory at a time. Defined in mount.cpp.
 */
void forEachMountLine(MountRecKind kind, const std::function<void(std::string_view)>& fn);

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
    std::vector<MountRecord>        mountRecords; ///< Mount results; formatted at print time.
    std::vector<UmountRecord>       umountRecords; ///< Unmount results; formatted at print time.
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
    std::vector<MountRecord>().swap(verboseSets.mountRecords); // also release capacity
    std::vector<UmountRecord>().swap(verboseSets.umountRecords);
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
 * @brief Inputs to a completed image-file search.
 *
 * Groups the values describing *what was searched* and *what was found*.
 * `verboseImageSearchResults` reads these and (via `verboseFind`) clears
 * the two error/path sets on completion, so the struct is passed by
 * non-const reference.
 *
 * @warning The two `std::unordered_set` members are cleared by
 *          `verboseImageSearchResults`. Treat the struct as
 *          "consumed" after the call.
 */
struct ImageSearchRequest {
    std::string fileExtension;                                    ///< Target format label (e.g. ".bin/.img").
    std::unordered_set<std::string>& fileNames;                   ///< Files discovered this run.
    std::unordered_set<std::string>& invalidDirectoryPaths;       ///< Invalid paths; cleared by the call.
    std::unordered_set<std::string>& processedErrorsFind;         ///< Deduplicated traversal errors; cleared by the call.
    const std::vector<std::string>& directoryPaths;               ///< Base directories traversed.
    const std::vector<std::string>& files;                        ///< Current working cache.
    bool newFilesFound;                                           ///< Whether the scan produced new files.
    bool list;                                                    ///< Suppresses the "no new files" / "cache empty" branches.
    int currentCacheOld;                                          ///< Cache count before this search.
    const std::atomic<size_t>& totalFiles;                        ///< Aggregate regular-file count from findFiles().
    std::chrono::high_resolution_clock::time_point start_time;    ///< Search start; used for elapsed time.
};

struct DatabaseRefreshReport {
    size_t totalProcessed = 0;
    int deltaCount = 0;
    double totalElapsed = 0.0;

    bool hasValidPaths = false;
    bool hasIsoFiles = false;
    bool newISOFound = false;
    bool saveSuccess = false;
    bool cancelled = false;

    std::unordered_set<std::string> invalidPaths;
    std::unordered_set<std::string> uniqueErrorMessages;
};

/**
 * Summarizes the results of an image file search operation.
 *
 * Prints the total number of files processed, the captured `verboseFind()`
 * output (invalid paths, traversal errors, interruption message), the
 * found/cached summary, and the elapsed time.
 *
 * The whole block is produced by an internal render lambda that clears
 * the screen and prints it. `pressEnterToContinueWithRepaint()` invokes
 * the lambda once at entry and again after clearing the screen on every
 * SIGWINCH, so a resize during the wait redraws the block from a known
 * top. No `ListResizeWatcher` is installed; the prompt owns resize handling.
 *
 * If `repaintCallerHeader` is non-empty, the render lambda calls it before
 * printing the block. Use it to reprint the caller's prompt line so it
 * survives a resize.
 *
 * `verboseFind()` mutates its set arguments (clears them), so it runs once,
 * before the wait, with its `std::cout`/`std::cerr` writes redirected into
 * an `ostringstream`; the captured text is replayed by the render lambda.
 *
 * @param req                 Search inputs and in/out sets (see struct docs).
 * @param repaintCallerHeader Optional callback that reprints the caller's
 *                            screen header on a resize during the wait.
 *                            When empty, the block starts from a cleared
 *                            screen with no caller context.
 *
 * @warning `req.invalidDirectoryPaths` and `req.processedErrorsFind` are
 *          cleared unconditionally before this function returns.
 */
void verboseImageSearchResults(
    ImageSearchRequest& req,
    const std::function<void()>& repaintCallerHeader = {}
);


// --- Database ---

/**
 * @brief Inputs to a completed ISO database refresh.
 *
 * Bundles the scan outputs that `saveAndReportResultsForDatabase` needs to
 * persist, reconcile, and report. `newISOFound` is passed by pointer to
 * `saveToDatabase()` and may be mutated.
 */
struct DatabaseRefreshResult {
    std::vector<std::string>& allIsoFiles;                     ///< Discovered ISO paths to be saved.
    std::vector<std::string>& validPaths;                      ///< Base directories successfully traversed.
    std::unordered_set<std::string>& invalidPaths;             ///< Directories skipped due to permissions or existence errors.
    std::unordered_set<std::string>& uniqueErrorMessages;      ///< Deduplicated system-level I/O errors.
    std::atomic<size_t>& totalFiles;                           ///< Atomic count of files scanned.
    bool& newISOFound;                                         ///< On entry, whether the scan produced delta changes; may be mutated by the save step.
    std::chrono::high_resolution_clock::time_point start_time; ///< Refresh origin timestamp; used for elapsed time.
};

/**
 * Finalizes an ISO database refresh: reports results and syncs global state.
 *
 * Last stage of the database update pipeline. Reconciles
 * `GlobalState::globalIsoFileList` with disk if `result.newISOFound` was
 * set by the caller, persists the new list via `saveToDatabase()`, computes
 * the delta count, and prints a color-coded outcome from one of five
 * branches: Cancelled; save failed with files and delta; no valid paths;
 * files present but no delta; no ISOs found; or successful import with the
 * delta count.
 *
 * All one-shot side effects (`loadFromDatabase`, `saveToDatabase`, elapsed
 * time, delta count) run before the wait. Only the on-screen block is
 * produced by an internal render lambda, which clears the screen first and
 * then prints everything. The caller must therefore NOT print any part of
 * the block itself — the lambda is the sole producer of the screen.
 *
 * If `repaintCallerHeader` is non-empty, the render lambda calls it before
 * printing the block. Both the early-save path (all paths invalid) and the
 * post-scan path in `refreshForDatabase()` pass a callback.
 *
 * @param result              Refresh inputs and scan outputs (see struct docs).
 * @param repaintCallerHeader Optional callback that reprints the caller's
 *                            screen header on a resize during the wait.
 *                            When empty, the block starts from a cleared
 *                            screen with no caller context.
 */
void saveAndReportResultsForDatabase(
    DatabaseRefreshResult& result,
    const std::function<void()>& repaintCallerHeader = {}
);

#endif // VERBOSE_H
