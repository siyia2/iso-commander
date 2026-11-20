// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Project Headers
#include "../caches.h"
#include "../concurrency.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../rendering.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../globalMutexes.h"
#include "../pausePrompts.h"
#include "../sharedRefreshState.h"
#include "../state.h"
#include "../themes.h"
#include "../threadpool.h"

// Local ISO Database mutex
namespace {
    std::mutex dbFileMutex;
}

/**
 * @brief Checks the existence of cached database paths in parallel using the static thread pool.
 *
 * Divides the workload into chunks across available worker threads (up to
 * GlobalConcurrency::CLEAN_THREAD_CAP) and checks each path using non-blocking file access checks.
 *
 * @param[in]  cache       Vector containing all path strings read from the database.
 * @param[out] pathExists  Reference to a vector of integers set to 1 if a path exists, or 0 if missing.
 * @return size_t          The total count of surviving (existing) paths found.
 */
static size_t checkPathsParallel(const std::vector<std::string>& cache, std::vector<int>& pathExists)
{
    ThreadPool& pool = getStaticThreadPool();
    std::atomic<size_t> existingCount{0};

    const size_t numThread = std::min({
        pool.threadCount(),
        static_cast<size_t>(GlobalConcurrency::CLEAN_THREAD_CAP),
        cache.size()
    });
    const size_t chunkSize = (cache.size() + numThread - 1) / numThread;
    std::vector<std::future<void>> futures;
    futures.reserve(numThread);

    for (size_t i = 0; i < numThread; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(cache.size(), start + chunkSize);
        if (start >= end) break;
        futures.emplace_back(
            pool.enqueue([&cache, &pathExists, &existingCount, start, end] {
                for (size_t j = start; j < end; ++j) {
                    if (access(cache[j].c_str(), F_OK) == 0) {
                        pathExists[j] = 1;
                        existingCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }));
    }
    for (auto& f : futures) f.get();
    return existingCount.load();
}

/**
 * @brief Filters surviving entries, serializes them, and atomically replaces the database file.
 *
 * Writes the surviving paths to a secure temporary file using `mkstemp`, sets permissions (0644),
 * flushes buffers using `fsync`, and performs an atomic rename over the target database file
 * to guarantee read consistency.
 *
 * @param[in]  dbPath       File system path to the target database file.
 * @param[in,out] cache     Full list of paths originally loaded from the database.
 *                          Surviving entries are moved out of it, so its contents
 *                          are unspecified on return.
 * @param[in]  pathExists   Vector flags denoting which cache items survived.
 * @param[in]  survivingCount Number of set flags in pathExists (used to reserve).
 * @param[out] retained     Vector populated exclusively with the surviving path entries.
 * @return true             If the temp file creation, writing, syncing, and atomic rename succeed.
 * @return false            If any I/O, permissions, or system call fails.
 */
static bool writeAndRenameDatabase(const std::string& dbPath,
                                   std::vector<std::string>& cache,
                                   const std::vector<int>& pathExists,
                                   size_t survivingCount,
                                   std::vector<std::string>& retained)
{
    retained.clear();
    retained.reserve(survivingCount);
    size_t totalBufferSize = 0;
    for (size_t i = 0; i < cache.size(); ++i) {
        if (pathExists[i]) {
            totalBufferSize += cache[i].size() + 1;
            retained.push_back(std::move(cache[i]));
        }
    }

    std::string buf;
    buf.reserve(totalBufferSize);
    for (const auto& path : retained) {
        buf += path;
        buf += '\n';
    }

    std::string tmpPath = (std::filesystem::path(dbPath).parent_path() / "iso_commander_database_saved_XXXXXX").string();
    int tmpFd = mkstemp(tmpPath.data());
    if (tmpFd == -1) return false;

    auto cleanupTmp = [&]() {
        close(tmpFd);
        ::unlink(tmpPath.c_str());
    };

    if (fchmod(tmpFd, 0644) == -1) { cleanupTmp(); return false; }
    if (::write(tmpFd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) { cleanupTmp(); return false; }
    if (fsync(tmpFd) == -1) { cleanupTmp(); return false; }
    close(tmpFd);

    if (::rename(tmpPath.c_str(), dbPath.c_str()) == -1) {
        ::unlink(tmpPath.c_str());
        return false;
    }
    return true;
}

/**
 * @brief Removes non-existent ISO paths from the persistent database and the in-memory cache.
 *
 * Acquires @p dbFileMutex, opens the database with `O_RDONLY`, and applies a shared file lock (`LOCK_SH`).
 * - If the database file is missing (`ENOENT`), clears @p globalIsoFileList under
 *   `GlobalMutexes::updateListMutex` and returns.
 * - Reads all valid lines, validates path existence in parallel via the static thread pool, and if
 *   any paths are missing, atomically replaces the database file.
 * - Sets `GlobalState::isoListDirty` to `true` upon a successful rewrite and updates
 *   @p globalIsoFileList asynchronously under lock if any entries were removed.
 *
 * Early returns (due to lock failure, empty cache, all paths surviving, or system call failures)
 * leave both the database file and the in-memory cache completely untouched.
 *
 * @param[out] globalIsoFileList Reference to the in-memory list of ISO files to update if changes occur.
 */
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList)
{
    std::vector<std::string> retained;
    bool anyRemoved = false;
    {
        std::lock_guard<std::mutex> fileLock(dbFileMutex);
        int fd = open(GlobalState::databaseFilePath.c_str(), O_RDONLY);
        if (fd == -1) {
            if (errno == ENOENT) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                globalIsoFileList.clear();
            }
            return;
        }
        struct FdGuard {
            int fd;
            ~FdGuard() { if (fd != -1) { flock(fd, LOCK_UN); close(fd); } }
            void release() { fd = -1; }
        } fdGuard{fd};

        if (flock(fd, LOCK_SH) == -1) return;

        int dupFd = dup(fd);
        if (dupFd == -1) return;
        FILE* file = fdopen(dupFd, "r");
        if (!file) { close(dupFd); return; }

        std::vector<std::string> cache;
        char* linePtr = nullptr;
        size_t len    = 0;
        while (getline(&linePtr, &len, file) != -1) {
            std::string line(linePtr);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (!line.empty()) cache.push_back(std::move(line));
        }
        free(linePtr);
        fclose(file);

        fdGuard.release();
        flock(fd, LOCK_UN);
        close(fd);

        if (cache.empty()) return;

        std::vector<int> pathExists(cache.size(), 0);
        size_t existingCount = checkPathsParallel(cache, pathExists);

        if (existingCount == cache.size()) return;

        if (!writeAndRenameDatabase(GlobalState::databaseFilePath, cache, pathExists,
                                    existingCount, retained)) {
            return;
        }

        anyRemoved = true;
        GlobalState::isoListDirty.store(true);
    }
    if (anyRemoved) {
        std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
        globalIsoFileList = std::move(retained);
    }
}

/**
 * @brief Counts non-empty lines in a file.
 *
 * A line is considered non-empty if it contains at least one character other
 * than space, tab, newline, carriage return, form feed, or vertical tab.
 *
 * @param filePath Path to the file to analyze
 * @return Number of non-empty lines, or -1 if the file cannot be opened
 *         (in which case an error is written to std::cerr)
 */
static int countNonEmptyLines(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Unable to open file: " << filePath << std::endl;
        return -1;
    }

    int nonEmptyLineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.find_first_not_of(" \t\n\r\f\v") != std::string::npos) {
            ++nonEmptyLineCount;
        }
    }

    file.close();
    return nonEmptyLineCount;
}

/**
 * @brief Gets the user's home directory path.
 *
 * @return Contents of the HOME environment variable, or an empty string if
 *         HOME is not set
 */
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}

namespace {

    /**
     * @brief Ensures the database directory exists and is a valid directory.
     *
     * Checks if the directory exists. If not, attempts to create it along with any
     * necessary parent directories. Verifies that the resulting path is a valid directory.
     *
     * @param dir Const reference to the filesystem path of the database directory.
     * @return true  if the directory exists and is valid, or was successfully created.
     * @return false if creation failed or the path exists but is not a directory.
     */
    bool ensureDatabaseDirectory(const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir) && !std::filesystem::create_directories(dir)) {
            return false;
        }
        return std::filesystem::is_directory(dir);
    }

    /**
     * @brief Reads and parses existing ISO entries from the database file.
     *
     * Opens the database file in read-only mode, acquires a shared file lock (flock),
     * and reads all lines into a vector of strings, stripping any carriage return characters.
     *
     * @param cachePath Const reference to the full filesystem path of the database file.
     * @return A std::vector<std::string> containing the cached ISO paths, or empty if the file doesn't exist or is empty.
     */
    std::vector<std::string> readExistingCache(const std::filesystem::path& cachePath) {
        std::vector<std::string> cache;
        int fd = open(cachePath.c_str(), O_RDONLY);
        if (fd == -1) return cache;

        if (flock(fd, LOCK_SH) == 0) {
            struct stat fileStat;
            if (fstat(fd, &fileStat) == 0 && fileStat.st_size > 0) {
                std::vector<char> buffer(fileStat.st_size);
                ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);
                if (bytesRead > 0 && bytesRead <= fileStat.st_size) {
                    char* start = buffer.data();
                    char* end = buffer.data() + bytesRead;
                    while (start < end) {
                        char* lineEnd = std::find(start, end, '\n');
                        std::string line(start, lineEnd);
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        if (!line.empty()) {
                            cache.push_back(std::move(line));
                        }
                        start = (lineEnd < end) ? lineEnd + 1 : end;
                    }
                }
            }
            flock(fd, LOCK_UN);
        }
        close(fd);
        return cache;
    }

    /**
     * @brief Atomically replaces the database cache using a temporary file and rename.
     *
     * Trims the cache to GlobalState::maxDatabaseSize if necessary, serializes entries
     * with newline separators, and writes them to a uniquely created temporary file
     * in the destination directory using mkstemp with permissions 0644.
     *
     * Handles partial writes and EINTR, synchronizes the temporary file with fsync,
     * checks close errors, and atomically replaces the destination using rename.
     * Finally, synchronizes the parent directory to improve crash durability.
     *
     * @param cachePath Const reference to the destination cache file path.
     * @param cache     Reference to the cache strings to be written and trimmed.
     * @return true  if the replacement and parent-directory synchronization succeed.
     * @return false if any operation fails. Note that a parent-directory
     *               synchronization failure occurs after rename, so the replacement
     *               may already have succeeded despite a false return value.
     */
    bool atomicWriteCache(const std::filesystem::path& cachePath,
                          std::vector<std::string>& cache) {
        if (cache.size() > GlobalState::maxDatabaseSize) {
            cache.erase(cache.begin(),
                        cache.begin() + (cache.size() - GlobalState::maxDatabaseSize));
        }

        std::string output;
        output.reserve(cache.size() * 64);

        for (const auto& entry : cache) {
            output += entry;
            output += '\n';
        }

        const std::filesystem::path parentPath = cachePath.parent_path();

        std::string tmpPath =
            (parentPath / "iso_commander_database_saved_XXXXXX").string();

        int tmpFd = mkstemp(tmpPath.data());
        if (tmpFd == -1) return false;

        auto cleanupTmp = [&]() {
            if (tmpFd != -1) {
                close(tmpFd);
                tmpFd = -1;
            }
            unlink(tmpPath.c_str());
        };

        if (fchmod(tmpFd, 0644) == -1) {
            cleanupTmp();
            return false;
        }

        size_t written = 0;
        while (written < output.size()) {
            ssize_t n = write(tmpFd, output.data() + written,
                              output.size() - written);

            if (n == -1 && errno == EINTR) continue;

            if (n <= 0) {
                cleanupTmp();
                return false;
            }

            written += static_cast<size_t>(n);
        }

        if (fsync(tmpFd) == -1) {
            cleanupTmp();
            return false;
        }

        if (close(tmpFd) == -1) {
            tmpFd = -1;
            unlink(tmpPath.c_str());
            return false;
        }
        tmpFd = -1;

        if (rename(tmpPath.c_str(), cachePath.c_str()) == -1) {
            unlink(tmpPath.c_str());
            return false;
        }

        int dirFd = open(parentPath.c_str(), O_RDONLY | O_DIRECTORY);
        if (dirFd == -1) return false;

        bool syncSuccess = fsync(dirFd) == 0;
        close(dirFd);

        return syncSuccess;
    }

} // namespace

/**
 * @brief Saves new ISO file paths to the database, merging with existing entries.
 *
 * Creates GlobalState::databaseDirectory if absent (returning false if
 * creation fails or the path exists but is not a directory), then acquires
 * dbFileMutex and reads existing entries from the database under a shared
 * flock. Deduplicates incoming paths against the existing cache and against
 * each other via an unordered_set of string_views (no string copies; the views
 * point into the existing cache and the input vector); appends new entries
 * directly to the cache; trims to GlobalState::maxDatabaseSize
 * by dropping the oldest entries (front erase) if needed.
 * Writes the merged contents to a mkstemp temp file (fchmod 0644,
 * write, fsync) and atomically renames it into place, so readers always see
 * either the complete old file or the complete new one. Sets
 * GlobalState::isoListDirty to true on success.
 *
 * Returns false if no new entries were found (and sets *newISOFound to false),
 * or if any I/O step fails (create_directories, mkstemp, fchmod, write, fsync,
 * rename); in the I/O-failure case *newISOFound is left at false.
 *
 * @param discoveredISO  Const reference to vector of ISO file paths to add.
 * @param newISOFound    Set to true if at least one new entry was added,
 *                       false if all paths already existed in the cache or
 *                       an I/O error occurred. May be nullptr.
 * @return true  if new entries were written successfully.
 * @return false if no new entries were found, or if an I/O error occurred.
 */
bool saveToDatabase(const std::vector<std::string>& discoveredISO, bool* newISOFound) {
    if (newISOFound) *newISOFound = false;

    std::filesystem::path cachePath = GlobalState::databaseDirectory;
    cachePath /= GlobalState::databaseFilename;

    if (!ensureDatabaseDirectory(GlobalState::databaseDirectory)) {
        return false;
    }

    std::lock_guard<std::mutex> fileLock(dbFileMutex);

    std::vector<std::string> combinedCache = readExistingCache(cachePath);
    const size_t oldSize = combinedCache.size();

    // Reserve up front: no reallocation, so the string_views below stay valid
    // (also covers SSO strings, whose characters live inside the vector).
    combinedCache.reserve(oldSize + discoveredISO.size());

    // Dedupe by view: no per-path string copies, no rehashing.
    std::unordered_set<std::string_view> seen;
    seen.reserve(oldSize + discoveredISO.size());
    for (const auto& s : combinedCache) seen.insert(s);

    // Append new paths straight into combinedCache.
    for (const auto& iso : discoveredISO) {
        if (seen.insert(iso).second) {
            combinedCache.push_back(iso);
        }
    }

    if (combinedCache.size() == oldSize) {
        return false;
    }

    if (!atomicWriteCache(cachePath, combinedCache)) {
        return false;
    }

    if (newISOFound) {
        *newISOFound = true;
    }
    GlobalState::isoListDirty.store(true);
    return true;
}

/**
 * @brief Passes successfully operated ISO file paths to the database.
 *
 * Accepts a semicolon-separated list of destination paths produced by a
 * completed copy or move operation, splits it into individual paths, and
 * forwards them to saveToDatabase() in a single batch call. The return value
 * of saveToDatabase() is ignored.
 *
 * @param filePathsStr  Semicolon-delimited string of destination ISO paths.
 *
 * @note Paths are assumed valid — they were written successfully by the
 *       preceding operation. No filesystem validation is performed.
 */
void updateDatabaseAfterOperations(const std::string& filePathsStr) {
    std::vector<std::string> allIsoFiles;
    std::istringstream iss(filePathsStr);
    std::string path;

    while (std::getline(iss, path, ';')) {
        if (!path.empty())
            allIsoFiles.push_back(std::move(path));
    }
    if (!allIsoFiles.empty()) {
        saveToDatabase(allIsoFiles, nullptr);
	}
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
 * levels that is a strict prefix of another path, eliminating redundant
 * parent entries.
 *
 * Non-absolute paths and empty segments are silently discarded.
 *
 * @param paths Vector of semicolon-delimited path strings
 * @return Vector of reduced/optimized absolute path strings
 */
static std::vector<std::string> hierarchicalPathReduction(const std::vector<std::string>& paths) {
    std::map<std::string, std::vector<std::string>> pathGroups;
    std::vector<std::string> allPaths;

    for (const auto& pathEntry : paths) {
        std::istringstream iss(pathEntry);
        std::string path;
        while (std::getline(iss, path, ';')) {
            if (!path.empty() && path[0] == '/') {
                if (path.back() != '/') path += '/';
                allPaths.push_back(path);
            }
        }
    }

    for (const auto& path : allPaths) {
        size_t slashCount = 0, pos = 0;

        for (size_t i = 1; i < path.length() && slashCount < 3; ++i) {
            if (path[i] == '/') {
                pos = i;
                slashCount++;
            }
        }

        std::string key = (slashCount >= 3) ? path.substr(0, pos + 1) : path;
        pathGroups[key].push_back(path);
    }

    std::vector<std::string> finalPaths;
    for (const auto& [prefix, groupPaths] : pathGroups) {
        finalPaths.push_back((groupPaths.size() > 1) ? prefix : groupPaths[0]);
    }

    std::sort(finalPaths.begin(), finalPaths.end());
    std::vector<std::string> result;

    for (const auto& path : finalPaths) {
        int levels = std::count(path.begin() + 1, path.end(), '/');

        bool isRedundant = (levels <= 2) &&
            std::any_of(finalPaths.begin(), finalPaths.end(),
                [&path](const std::string& other) {
                    return other != path && other.starts_with(path);
                });

        if (!isRedundant) {
            result.push_back(path);
        }
    }

    return result;
}

bool isValidDirectory(const std::string& path);

namespace {

    /**
     * @brief Signals completion of the background import process.
     *
     * @details If cancellation was not requested via stopImport, introduces a 500ms
     *          delay before signaling. Updates the running status under printMutex
     *          to ensure thread-safe synchronization visibility, and notifies importCV.
     *
     * @param state Shared refresh state holding synchronization primitives and status flags.
     */
    void signalImportDone(const std::shared_ptr<RefreshState>& state) {
        if (!state) return;
        if (!state->stopImport.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        {
            std::lock_guard<std::mutex> lk(state->printMutex);
            state->isImportRunning.store(false, std::memory_order_relaxed);
        }
        state->importCV.notify_all();
    }

    /**
     * @brief Parses semicolon-delimited absolute paths from the history file.
     *
     * @details Reads paths line by line, validating that they begin with '/' and
     *          normalizing trailing slashes. Deduplicates paths and periodically
     *          checks the stopImport flag for early cancellation.
     *
     * @param state Shared refresh state used to check for cancellation signals.
     * @param paths Output vector to populate with parsed and normalized absolute paths.
     * @return true If the history file was successfully read and parsed.
     * @return false If the file could not be opened or import was stopped.
     */
    bool parseHistoryFile(const std::shared_ptr<RefreshState>& state, std::vector<std::string>& paths) {
        std::ifstream file(GlobalState::historyFilePath);
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (state->stopImport.load()) return false;
            std::istringstream iss(line);
            std::string path;
            while (std::getline(iss, path, ';')) {
                if (!path.empty() && path[0] == '/') {
                    if (path.back() != '/') path += '/';
                    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
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
     * @details Removes the root path "/" from the collection if multiple paths
     *          are present, avoiding unnecessary exhaustive root-level scans.
     *
     * @param paths Vector of path strings to clean and reduce.
     */
    void cleanAndReducePaths(std::vector<std::string>& paths) {
        if (paths.size() > 1) {
            auto it = std::find(paths.begin(), paths.end(), "/");
            if (it != paths.end()) paths.erase(it);
        }
    }

    /**
     * @brief Traverses directory paths in concurrent thread-pool-sized groups.
     *
     * @details Spawns worker threads up to the static thread pool count. Validates
     *          directories before processing, tracks active workers, handles thread
     *          construction safety, and supports mid-group cancellation via stopImport.
     *
     * @param finalPaths           Vector of hierarchical path reduction results to traverse.
     * @param state                Shared refresh state containing synchronization variables.
     * @param allIsoFiles          Output container accumulating discovered ISO file paths.
     * @param uniqueErrorMessages  Output container accumulating unique traversal errors.
     * @param totalFiles           Atomic counter tracking total discovered files.
     * @param processMutex         Mutex protecting shared collection writes during traversal.
     * @param traverseErrorMutex   Mutex protecting unique error message insertion.
     * @param localMaxDepth        Maximum recursion depth limit (-1 for unlimited).
     * @param localPromptFlag      Flag controlling internal prompt behavior during traversal.
     * @return true If traversal completed successfully for all groups.
     * @return false If execution was aborted early due to cancellation.
     */
    bool executeDirectoryTraversal(
        const std::vector<std::string>& finalPaths,
        const std::shared_ptr<RefreshState>& state,
        std::vector<std::string>& allIsoFiles,
        std::unordered_set<std::string>& uniqueErrorMessages,
        std::atomic<size_t>& totalFiles,
        std::mutex& processMutex,
        std::mutex& traverseErrorMutex,
        int localMaxDepth,
        bool localPromptFlag
    ) {
        const size_t groupSize = std::max<size_t>(1, getStaticThreadPool().threadCount());

        for (size_t i = 0; i < finalPaths.size(); i += groupSize) {
            if (state->stopImport.load()) return false;

            const size_t end = std::min(i + groupSize, finalPaths.size());
            std::vector<std::thread> threads;
            threads.reserve(end - i);

            state->activeWorkers.store(0, std::memory_order_relaxed);

            for (size_t j = i; j < end; ++j) {
                if (state->stopImport.load()) break;
                if (!isValidDirectory(finalPaths[j])) continue;

                state->activeWorkers.fetch_add(1, std::memory_order_relaxed);

                try {
                    threads.emplace_back([&, path = finalPaths[j]]() {
                        traverse(path, allIsoFiles, uniqueErrorMessages,
                                 totalFiles, processMutex, traverseErrorMutex,
                                 localMaxDepth, localPromptFlag);
                        if (state->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            std::lock_guard<std::mutex> lk(state->workerMutex);
                            state->workerCV.notify_all();
                        }
                    });
                } catch (...) {
                    state->activeWorkers.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            if (!threads.empty()) {
                std::unique_lock<std::mutex> lk(state->workerMutex);
                state->workerCV.wait(lk, [&] {
                    return state->activeWorkers.load(std::memory_order_acquire) == 0
                        || state->stopImport.load();
                });
            }

            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
        }
        return !state->stopImport.load();
    }

} // namespace

/**
 * @brief Performs background ISO file import without blocking the UI.
 *
 * @details Reads semicolon-delimited paths from the history file, deduplicates and
 *          hierarchically reduces them, traverses directories concurrently in
 *          groups sized to the thread pool, and saves results to the database upon
 *          successful completion. Continuously monitors cancellation state
 *          (stopImport) across stages and handles proper synchronization and cleanup.
 *
 * @param state Shared state holding isImportRunning, stopImport, synchronization
 *              condition variables, and execution mutexes.
 */
void backgroundDatabaseImport(std::shared_ptr<RefreshState> state) {
    auto signalDone = [&] { signalImportDone(state); };

    std::vector<std::string> paths;
    if (!parseHistoryFile(state, paths)) {
        signalDone();
        return;
    }

    if (state->stopImport.load()) { signalDone(); return; }

    cleanAndReducePaths(paths);
    std::vector<std::string> finalPaths = hierarchicalPathReduction(paths);
    if (finalPaths.empty() || state->stopImport.load()) {
        signalDone();
        return;
    }

    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> uniqueErrorMessages;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;

    int localMaxDepth = -1;
    bool localPromptFlag = false;

    if (!executeDirectoryTraversal(finalPaths, state, allIsoFiles, uniqueErrorMessages,
                                   totalFiles, processMutex, traverseErrorMutex,
                                   localMaxDepth, localPromptFlag)) {
        signalDone();
        return;
    }

    if (state->stopImport.load()) { signalDone(); return; }

    saveToDatabase(allIsoFiles, nullptr);
    signalDone();
}

/**
 * @brief Loads the ISO database from file into memory.
 *
 * Acquires dbFileMutex, opens the database with O_RDONLY and takes a shared
 * flock. If the open or flock fails, returns without modifying outList. If
 * fstat fails or the file is empty, clears outList and returns. Otherwise
 * reads the whole file, splits on '\n', strips trailing '\r', skips empty
 * lines, and replaces outList wholesale (move-assign) with the parsed entries.
 * If read() returns <= 0 or more bytes than the reported file size, returns
 * without modifying outList.
 *
 * @param outList Reference to vector that will receive the loaded ISO paths
 */
void loadFromDatabase(std::vector<std::string>& outList) {
    std::lock_guard<std::mutex> fileLock(dbFileMutex);

    int fd = open(GlobalState::databaseFilePath.c_str(), O_RDONLY);
    if (fd == -1) return;

    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }

    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1 || fileStat.st_size == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        outList.clear();
        return;
    }

    std::vector<char> buffer(fileStat.st_size);
    ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);

    flock(fd, LOCK_UN);
    close(fd);

    if (bytesRead <= 0 || bytesRead > fileStat.st_size) return;

    std::vector<std::string> loadedFiles;
    char* start = buffer.data();
    char* end = buffer.data() + bytesRead;

    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) loadedFiles.push_back(std::move(line));
        start = (lineEnd < end) ? lineEnd + 1 : end;
    }
    outList = std::move(loadedFiles);
}

/**
 * @brief Displays database statistics including on-disk and RAM usage.
 *
 * Ignores SIGINT, disables Ctrl-D handling, and creates the ISO database,
 * history, and filter-history files if absent. Then prints capacity, entry
 * counts, and locations for the ISO and history databases, followed by
 * RAM-buffered entry counts for ISO, STR, BIN/IMG, DAA/GBI, CHD, MDF, and NRG
 * caches sourced from GlobalCaches and GlobalState. Catches
 * std::filesystem::filesystem_error and prints an error referencing
 * GlobalState::configPath.
 *
 * The screen content is generated by a `render` lambda so it can be replayed
 * by the resize watcher and by the "press Enter" prompt, keeping the display
 * consistent across SIGWINCH events.
 *
 * @param databaseFilePath Path to the ISO database file.
 * @param maxDatabaseSize  Maximum allowed database size, in bytes (displayed as KB).
 */
void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    auto render = [&] {
        SyncOutput sync;
        clearScrollBuffer();

        auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveDatabaseTheme();
        (void)path;
        (void)highlight;

        try {
            for (const auto& path : {GlobalState::databaseFilePath, GlobalState::historyFilePath, GlobalState::filterHistoryFilePath}) {
                if (!std::filesystem::exists(path)) {
                    std::ofstream createFile(path);
                }
            }

            std::cout << "\n" << accent << "=== ISO Database ===" << reset << "\n";

            std::uintmax_t fileSizeInBytes = std::filesystem::file_size(databaseFilePath);
            double fileSizeInKB = fileSizeInBytes / 1024.0;
            double cachesizeInKb = maxDatabaseSize / 1024.0;
            double usagePercentage = (fileSizeInBytes * 100.0) / maxDatabaseSize;

            std::cout << "\n" << label << "Capacity: " << data << std::fixed << std::setprecision(0)
                      << fileSizeInKB << "KB/" << cachesizeInKb << "KB (" << std::setprecision(1) << usagePercentage << "%)"
                      << "\n" << label << "Entries: " << data << countNonEmptyLines(GlobalState::databaseFilePath)
                      << "\n" << label << "Location: " << data << "'" << databaseFilePath << "'" << reset << "\n";

            std::cout << "\n" << accent << "=== History Database ===" << reset << "\n"
                      << "\n" << label << "FolderPath Entries: " << data << countNonEmptyLines(GlobalState::historyFilePath) << "/" << GlobalState::MAX_HISTORY_LINES
                      << "\n" << label << "Location: " << data << "'" << GlobalState::historyFilePath << "'"
                      << "\n\n" << label << "FilterTerm Entries: " << data << countNonEmptyLines(GlobalState::filterHistoryFilePath) << "/" << GlobalState::MAX_HISTORY_PATTERN_LINES
                      << "\n" << label << "Location: " << data << "'" << GlobalState::filterHistoryFilePath << "'" << std::endl;

            std::cout << "\n" << accent << "=== Buffered Entries ===" << reset << "\n";

            std::cout << str << "\nSTR → RAM: " << data
                      << (GlobalCaches::transformationCache.size() + GlobalCaches::cachedParsesForUmount.size()) << "\n";

            std::cout << "\n" << label << "ISO → RAM: " << data << GlobalState::globalIsoFileList.size() << "\n";

            std::cout << "\n" << path << "BIN/IMG → RAM: " << data << GlobalState::binImgFilesCache.size() << "\n"
                      << path << "DAA/GBI → RAM: " << data << GlobalState::daaGbiFilesCache.size() << "\n"
                      << path << "CHD → RAM: " << data << GlobalState::chdFilesCache.size() << "\n"
                      << path << "MDF → RAM: " << data << GlobalState::mdfMdsFilesCache.size() << "\n"
                      << path << "NRG → RAM: " << data << GlobalState::nrgFilesCache.size() << "\n";

        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "\n" << error << "Error: Unable to access configuration file: "
                      << warning << "'" << GlobalState::configPath << "'"
                      << error << ".\n" << reset;
        }
    };

    pressEnterToReturnWithRepaint(render);
}

/**
 * @brief Handles database management commands.
 *
 * Ignores SIGINT, disables Ctrl-D handling, then dispatches on inputSearch:
 *   *stats                   — calls displayDatabaseStatistics()
 *   !clr                     — truncates the ISO database file; on success
 *                              clears GlobalCaches::transformationCache and
 *                              (after the "press Enter" prompt) swaps
 *                              GlobalState::globalIsoFileList for an empty
 *                              vector; on truncation failure shows an error
 *                              and clears nothing
 *   !clr_paths / !clr_filter — delegates to clearHistory()
 *
 * repaintHeader is used to restore the screen header after clearing and is
 * forwarded to clearHistory().
 *
 * @param inputSearch   Command string to process.
 * @param repaintHeader Callback invoked to repaint the screen header when needed.
 */
void databaseSwitches(std::string& inputSearch, const std::function<void()>& repaintHeader) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    auto db = resolveDatabaseTheme();

    if (inputSearch == "*stats") {
        displayDatabaseStatistics(GlobalState::databaseFilePath, GlobalState::maxDatabaseSize);
    } else if (inputSearch == "!clr") {
        std::ofstream ofs(GlobalState::databaseFilePath, std::ofstream::out | std::ofstream::trunc);
        if (!ofs) {
            auto render = [&] {
                SyncOutput sync;
                clearScrollBuffer();
                // Screen was just cleared, so the header is always needed here
                if (repaintHeader) repaintHeader();

                std::cerr << "\n" << db.error << "Error clearing ISO database: "
                          << db.warning << "File missing or inaccessible" << db.error << "." << "\033[J" << std::endl;
            };
            pressEnterToContinueWithRepaint(render);
        } else {
            ofs.close();
            GlobalCaches::transformationCache.clear();

            bool firstDraw = true;

            auto render = [&] {
                SyncOutput sync;
                if (!firstDraw && repaintHeader) repaintHeader();
                firstDraw = false;

                std::cout << "\n" << db.highlight << "ISO database cleared successfully."
                          << "\033[J" << std::endl;
            };
            pressEnterToContinueWithRepaint(render);

            std::vector<std::string>().swap(GlobalState::globalIsoFileList);
        }
    } else if (inputSearch == "!clr_paths" || inputSearch == "!clr_filter") {
        clearHistory(inputSearch, repaintHeader);
    }
    return;
}
