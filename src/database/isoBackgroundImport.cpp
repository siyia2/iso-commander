// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/core/state.h"
#include "isocmd/core/threadpool.h"
#include "isocmd/database/databaseOps.h"

namespace {

    /**
     * @brief Extracts and normalizes absolute paths from semicolon-delimited entries.
     *
     * Splits each input string on ';' and retains only non-empty entries starting
     * with '/', ensuring every kept path ends with a trailing '/'.
     *
     * @param paths Vector of semicolon-delimited path strings.
     * @return Vector of normalized absolute path strings ending with '/'.
     */
    std::vector<std::string> collectNormalizedPaths(
        const std::vector<std::string>& paths
    ) {
        std::vector<std::string> normalized;

        for (const auto& pathEntry : paths) {
            std::istringstream iss(pathEntry);
            std::string path;

            while (std::getline(iss, path, ';')) {
                if (path.empty() || path.front() != '/') {
                    continue;
                }

                if (path.back() != '/') {
                    path += '/';
                }

                normalized.push_back(std::move(path));
            }
        }

        return normalized;
    }

    /**
     * @brief Computes the grouping key up to the third '/' character in a path.
     *
     * Scans past index 0 for up to three '/' characters. Returns the prefix
     * through the third '/' if at least three are found; otherwise returns the
     * full path.
     *
     * @param path Normalized path starting with '/'.
     * @return Prefix key through the third '/' or the full path.
     */
    std::string thirdSlashPrefix(const std::string& path) {
        std::size_t slashCount = 0;
        std::size_t pos = 0;

        for (std::size_t i = 1; i < path.length() && slashCount < 3; ++i) {
            if (path[i] == '/') {
                pos = i;
                ++slashCount;
            }
        }

        return (slashCount >= 3) ? path.substr(0, pos + 1) : path;
    }

    /**
     * @brief Groups normalized paths by their third-slash prefix and selects representatives.
     *
     * Emits the shared prefix when a group contains more than one path, or the
     * sole path when a group has a single entry.
     *
     * @param paths Normalized absolute paths ending with '/'.
     * @return Representative path strings per prefix group.
     */
    std::vector<std::string> groupByPrefix(
        const std::vector<std::string>& paths
    ) {
        std::map<std::string, std::vector<std::string>> pathGroups;

        for (const auto& path : paths) {
            pathGroups[thirdSlashPrefix(path)].push_back(path);
        }

        std::vector<std::string> grouped;
        grouped.reserve(pathGroups.size());

        for (const auto& [prefix, groupPaths] : pathGroups) {
            grouped.push_back(
                (groupPaths.size() > 1) ? prefix : groupPaths.front()
            );
        }

        return grouped;
    }

    /**
     * @brief Checks whether a candidate path is a redundant shallow prefix.
     *
     * A path with two or fewer '/' characters after index 0 is considered
     * redundant if another distinct path in the set starts with it.
     *
     * @param path Candidate path to evaluate.
     * @param candidates All sorted candidate paths after grouping.
     * @return true if the path should be pruned, false otherwise.
     */
    bool isRedundantShallowPath(
        const std::string& path,
        const std::vector<std::string>& candidates
    ) {
        const int levels = static_cast<int>(
            std::count(path.begin() + 1, path.end(), '/')
        );

        if (levels > 2) {
            return false;
        }

        return std::any_of(
            candidates.begin(),
            candidates.end(),
            [&path](const std::string& other) {
                return other != path && other.starts_with(path);
            }
        );
    }

    /**
     * @brief Reduces hierarchical paths by grouping related directories.
     *
     * Splits each input string on ';' and keeps only absolute paths (those whose
     * first character is '/'), appending a trailing '/' if missing. Groups the
     * resulting paths by the prefix up to their third '/' (or the whole path if
     * fewer than three slashes are present). For each group, emits the shared
     * prefix if the group contains more than one path, otherwise the single path.
     * Sorts the grouped result, then removes any path with two or fewer directory
     * levels that is a strict prefix of another path.
     *
     * Non-absolute paths and empty segments are silently discarded.
     *
     * @param paths Vector of semicolon-delimited path strings.
     * @return Vector of reduced/optimized absolute path strings.
     */
    std::vector<std::string> hierarchicalPathReduction(
        const std::vector<std::string>& paths
    ) {
        const std::vector<std::string> normalized =
            collectNormalizedPaths(paths);

        std::vector<std::string> finalPaths = groupByPrefix(normalized);
        std::sort(finalPaths.begin(), finalPaths.end());

        std::vector<std::string> result;
        result.reserve(finalPaths.size());

        for (const auto& path : finalPaths) {
            if (!isRedundantShallowPath(path, finalPaths)) {
                result.push_back(path);
            }
        }

        return result;
    }

    /**
     * @brief Signals completion of the background import process.
     *
     * If cancellation was not requested via stopImport, introduces a 500ms
     * delay before signaling. Updates the running status under printMutex
     * to ensure thread-safe synchronization visibility, and notifies importCV.
     *
     * @param state Shared refresh state holding synchronization primitives and status flags.
     */
    void signalImportDone(const std::shared_ptr<RefreshState>& state) {
        if (!state) return;

        if (!state->stopImport.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        {
            std::lock_guard<std::mutex> lock(state->printMutex);
            state->isImportRunning.store(false, std::memory_order_relaxed);
        }

        state->importCV.notify_all();
    }

    /**
     * @brief Parses semicolon-delimited absolute paths from the history file.
     *
     * Reads paths line by line, validating that they begin with '/' and
     * normalizing trailing slashes. Deduplicates paths and periodically
     * checks the stopImport flag for early cancellation.
     *
     * @param state Shared refresh state used to check for cancellation signals.
     * @param paths Output vector to populate with parsed and normalized absolute paths.
     * @return true If the history file was successfully read and parsed.
     * @return false If the file could not be opened or import was stopped.
     */
    bool parseHistoryFile(
        const std::shared_ptr<RefreshState>& state,
        std::vector<std::string>& paths
    ) {
        std::ifstream file(GlobalState::historyFilePath);
        if (!file.is_open()) return false;

        std::string line;

        while (std::getline(file, line)) {
            if (state->stopImport.load()) {
                return false;
            }

            std::istringstream iss(line);
            std::string path;

            while (std::getline(iss, path, ';')) {
                if (!path.empty() && path[0] == '/') {
                    if (path.back() != '/') {
                        path += '/';
                    }

                    if (std::find(paths.begin(), paths.end(), path) ==
                        paths.end()) {
                        paths.push_back(path);
                    }
                }
            }
        }

        return true;
    }

    /**
     * @brief Cleans and removes redundant root paths.
     *
     * Removes the root path "/" from the collection if multiple paths
     * are present, avoiding unnecessary exhaustive root-level scans.
     *
     * @param paths Vector of path strings to clean and reduce.
     */
    void cleanAndReducePaths(std::vector<std::string>& paths) {
        if (paths.size() > 1) {
            auto it = std::find(paths.begin(), paths.end(), "/");

            if (it != paths.end()) {
                paths.erase(it);
            }
        }
    }

    /**
     * @brief Traverses directory paths in concurrent thread-pool-sized groups.
     *
     * Spawns worker threads up to the static thread pool count. Validates
     * directories before processing, tracks active workers, handles thread
     * construction safety, and supports mid-group cancellation via stopImport.
     *
     * @param finalPaths Vector of hierarchical path reduction results to traverse.
     * @param state Shared refresh state containing synchronization variables.
     * @param allIsoFiles Output container accumulating discovered ISO file paths.
     * @param uniqueErrorMessages Output container accumulating unique traversal errors.
     * @param totalFiles Atomic counter tracking total discovered files.
     * @param processMutex Mutex protecting shared collection writes during traversal.
     * @param traverseErrorMutex Mutex protecting unique error message insertion.
     * @param localMaxDepth Maximum recursion depth limit (-1 for unlimited).
     * @param localPromptFlag Flag controlling internal prompt behavior during traversal.
     * @return true If traversal completed successfully for all groups.
     * @return false If execution was aborted early due to cancellation.
     */
    bool executeDirectoryTraversal(
        const std::vector<std::string>& finalPaths,
        const std::shared_ptr<RefreshState>& state,
        std::vector<std::string>& allIsoFiles,
        std::unordered_set<std::string>& uniqueErrorMessages,
        std::atomic<std::size_t>& totalFiles,
        std::mutex& processMutex,
        std::mutex& traverseErrorMutex,
        int localMaxDepth,
        bool localPromptFlag
    ) {
        const std::size_t groupSize = std::max<std::size_t>(
            1,
            getStaticThreadPool().threadCount()
        );

        for (std::size_t i = 0; i < finalPaths.size(); i += groupSize) {
            if (state->stopImport.load()) {
                return false;
            }

            const std::size_t end = std::min(i + groupSize, finalPaths.size());

            std::vector<std::thread> threads;
            threads.reserve(end - i);

            state->activeWorkers.store(0, std::memory_order_relaxed);

            for (std::size_t j = i; j < end; ++j) {
                if (state->stopImport.load()) {
                    break;
                }

                if (!isValidDirectory(finalPaths[j])) {
                    continue;
                }

                state->activeWorkers.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

                try {
                    threads.emplace_back([&, path = finalPaths[j]] {
                        traverse(
                            path,
                            allIsoFiles,
                            uniqueErrorMessages,
                            totalFiles,
                            processMutex,
                            traverseErrorMutex,
                            localMaxDepth,
                            localPromptFlag
                        );

                        if (
                            state->activeWorkers.fetch_sub(
                                1,
                                std::memory_order_acq_rel
                            ) == 1
                        ) {
                            std::lock_guard<std::mutex> lock(
                                state->workerMutex
                            );

                            state->workerCV.notify_all();
                        }
                    });
                } catch (...) {
                    state->activeWorkers.fetch_sub(
                        1,
                        std::memory_order_relaxed
                    );
                }
            }

            if (!threads.empty()) {
                std::unique_lock<std::mutex> lock(state->workerMutex);

                state->workerCV.wait(lock, [&] {
                    return state->activeWorkers.load(
                               std::memory_order_acquire
                           ) == 0 ||
                           state->stopImport.load();
                });
            }

            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }

        return !state->stopImport.load();
    }

} // namespace

/**
 * @brief Performs background ISO file import without blocking the UI.
 *
 * Reads semicolon-delimited paths from the history file, deduplicates and
 * hierarchically reduces them, traverses directories concurrently in
 * groups sized to the thread pool, and saves results to the database upon
 * successful completion. Continuously monitors cancellation state
 * (stopImport) across stages and handles proper synchronization and cleanup.
 *
 * @param state Shared state holding isImportRunning, stopImport, synchronization
 *              condition variables, and execution mutexes.
 */
void backgroundDatabaseImport(std::shared_ptr<RefreshState> state) {
    auto signalDone = [&] {
        signalImportDone(state);
    };

    std::vector<std::string> paths;

    if (!parseHistoryFile(state, paths)) {
        signalDone();
        return;
    }

    if (state->stopImport.load()) {
        signalDone();
        return;
    }

    cleanAndReducePaths(paths);

    std::vector<std::string> finalPaths =
        hierarchicalPathReduction(paths);

    if (finalPaths.empty() || state->stopImport.load()) {
        signalDone();
        return;
    }

    std::vector<std::string> allIsoFiles;
    std::atomic<std::size_t> totalFiles{0};
    std::unordered_set<std::string> uniqueErrorMessages;

    std::mutex processMutex;
    std::mutex traverseErrorMutex;

    int localMaxDepth = -1;
    bool localPromptFlag = false;

    if (!executeDirectoryTraversal(
            finalPaths,
            state,
            allIsoFiles,
            uniqueErrorMessages,
            totalFiles,
            processMutex,
            traverseErrorMutex,
            localMaxDepth,
            localPromptFlag
        )) {
        signalDone();
        return;
    }

    if (state->stopImport.load()) {
        signalDone();
        return;
    }

    saveToDatabase(allIsoFiles, nullptr);
    signalDone();
}
