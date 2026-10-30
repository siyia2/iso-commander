// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <signal.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../concurrency.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../globalMutexes.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../listResizeWatcher.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../search.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../verbose.h"

namespace fs = std::filesystem;

/**
 * @brief Database operations and file scanning for ISO and disc image files.
 *
 * Scans directories for ISO files, manages the ISO database, handles RAM
 * caches for BIN/IMG/MDF/NRG files, and drives the interactive file-selection
 * UIs.
 */

/**
 * @brief Returns true if @p path exists and is a directory.
 */
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}

/**
 * @brief Prompts for directory paths and imports discovered ISO files into
 * the local database.
 *
 * Reads a semicolon-delimited list of directory paths, validates and trims
 * each path, deduplicates by normalized form, sorts, then eliminates
 * subdirectories of already-included paths (the prefix check requires the
 * prior sort). Remaining paths are scanned in parallel via the static thread
 * pool up to @p maxDepth. Results are forwarded to
 * saveAndReportResultsForDatabase(), which calls saveToDatabase() and may set
 * @p newISOFound. Invalid paths omitted from the search are stripped from the
 * history entry for the submitted input.
 *
 * @par Control flow
 * - Exit: ESC input ("\x1b", typically bound to '<' via
 *   setup_custom_keybindingsForSearches) or a null return from readline
 *   (Ctrl+D) — both return directly.
 * - Continue: '?' (help via helpSearches), switch commands (*stats, !clr,
 *   !clr_paths, !clr_filter via databaseSwitches), empty or whitespace-only
 *   input, empty validPaths with no invalid paths, and exceptions (prints
 *   error, waits for Enter, re-arms and retries).
 * - Early save path: if all paths are invalid (validPaths empty,
 *   invalidPaths non-empty), resets g_operationCancelled and calls
 *   saveAndReportResultsForDatabase without scanning.
 * - Cancellation guarding: g_operationCancelled is reset to false inside
 *   rearmSetup (top of every iteration), before the early-save path, and
 *   before the parallel scan block. This prevents a spurious cancellation
 *   from a premature Ctrl+C before input is processed.
 * - Task joining: every scan task is always waited on, even after a
 *   cancellation. The tasks capture references to locals of this iteration
 *   (allIsoFiles, uniqueErrorMessages, totalFiles, the mutexes); returning
 *   before they finish would leave them with dangling references. Tasks
 *   observe g_operationCancelled and exit quickly.
 * - History: loadHistory(filterHistory) is called unconditionally each
 *   iteration. After parsing, add_history is called with only the valid
 *   tokens, trimmed but not normalized, rejoined with ';' (e.g. the input
 *   "/home/;/var/;asd" is stored as "/home/;/var/"). If no token was valid,
 *   nothing is added. saveHistory(filterHistory) is called only when both
 *   validPaths and input are non-empty after a scan completes.
 * - RAII keybinding management: a SearchReadlineGuard is constructed at the
 *   start of each loop iteration (after rearmSetup). Its destructor restores
 *   the saved rl_attempted_completion_function, rebinds '\t' to
 *   prevent_readline_keybindings, rebinds '\f' to rl_insert, clears the
 *   history buffer, resets readline pagination, and calls
 *   reset_custom_keybindingsForSearches(). Cleanup runs on all loop-body
 *   exit paths — continue, return, and exceptions. The guard is recreated
 *   each iteration so its saved state always reflects the state just after
 *   rearmSetup runs.
 * - Resize repaint (prompt): the prompt carries its own header line, so
 *   readline() is wrapped by readlineWithResize() with a callback that clears
 *   the screen and re-prints the header (via promptHeaderForRepaint). The
 *   watcher adds the newline and Readline redraws the last prompt line and
 *   the typed input.
 * - Resize repaint (results screen): after the scan, the results block is
 *   shown by saveAndReportResultsForDatabase, which owns its own resize
 *   handling via pressEnterToContinueWithRepaint. This function supplies
 *   repaintCallerPrompt — a callback that reprints the FolderPaths prompt
 *   line — so that a resize while the user reads the results keeps the
 *   question the user just answered visible above the block. The callback is
 *   invoked inside saveAndReportResultsForDatabase's render, before the
 *   block is printed. Both the early-save call site (all paths invalid) and
 *   the post-scan call site pass it.
 * - Resize repaint (exception screen): the catch block prints the error and
 *   waits via pressEnterToContinueWithRepaint, which owns SIGWINCH. No
 *   ListResizeWatcher is installed there; it would race the prompt's own
 *   handler (and cannot fire during the raw read anyway), and a second
 *   watcher would stack copies of the error block on each resize.
 *
 * @param promptFlag    Passed to traverse() to control scan behaviour.
 * @param maxDepth      Maximum directory recursion depth (-1 for unlimited).
 * @param filterHistory If true, readline history is loaded at prompt time and
 *                      saved after a successful scan via loadHistory /
 *                      saveHistory.
 * @param newISOFound   Passed by reference into saveAndReportResultsForDatabase,
 *                      which forwards it to saveToDatabase as a pointer;
 *                      saveToDatabase is responsible for setting it.
 */
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, bool& newISOFound) {
    PendingReadlineTextClearGuard pendingTextGuard;

    auto rearmSetup = [&]() {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSearches();
        rl_attempted_completion_function = my_special_completion_entry;
        GlobalState::g_operationCancelled.store(false);
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 1;
    };

    while (true) {
        rearmSetup();
        clearScrollBuffer();
        SearchReadlineGuard guard;
        try {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();

            loadHistory(filterHistory);

            RestoreReadlineBuffer();

            std::string prompt;
            prompt.reserve(512);
            prompt.append("\001").append(dt.green).append("\002FolderPaths")
                  .append("\001").append(dt.blue).append("\002 ↵ to scan for ")
                  .append("\001").append(dt.green).append("\002.iso")
                  .append("\001").append(dt.blue).append("\002 entries and import into ")
                  .append("\001").append(dt.green).append("\002local")
                  .append("\001").append(dt.blue).append("\002 DB, ? for help:\n")
                  .append("\001").append(dt.reset).append("\002");

            const auto repaintPrompt = [&] {
                SyncOutput sync;
                clearScrollBuffer();
                std::cout << promptHeaderForRepaint(prompt);
            };

            const auto repaintCallerPrompt = [&] {
                std::cout << promptHeaderForRepaint(prompt) << "\n";
            };

            RlLine searchQuery = readlineWithResize(prompt, repaintPrompt);
            if (!searchQuery) return;

            armPendingInputRestore();
            rl_startup_hook = nullptr;

            std::string input = trimWhitespace(searchQuery.get());

            if (input == "\x1b") return;

            if (input == "?") {
                bool isCpMv = false, import2ISO = true;
                helpSearches(isCpMv, import2ISO, false);
                continue;
            }

            if (input == "*stats" || input == "!clr" || input == "!clr_paths" || input == "!clr_filter") {
                databaseSwitches(input);
                continue;
            }

            if (input.find_first_not_of(" \t\n\r") == std::string::npos) {
                continue;
            }

            std::vector<std::string> validPaths;
            std::unordered_set<std::string> invalidPaths;
            std::unordered_set<std::string> uniqueErrorMessages;
            std::vector<std::string> allIsoFiles;
            std::atomic<size_t> totalFiles{0};

            std::cout << "\033[3H\033[J\n";
            disableInput();

            auto start_time = std::chrono::high_resolution_clock::now();
            std::istringstream iss(input);
            std::string path;

            auto normalizePath = [](const std::string& p) {
                std::string n = fs::path(p).lexically_normal().string();
                if (n.size() > 1 && n.back() == '/')
                    n.pop_back();
                return n;
            };

            std::unordered_set<std::string> uniqueNormalizedPaths;
            std::string cleanedInput;   // only the tokens that passed validation, as typed

            while (std::getline(iss, path, ';')) {
                path = trimWhitespace(path);
                if (path.empty()) continue;
                if (!isValidDirectory(path)) {
                    invalidPaths.insert(path);
                    continue;
                }

                if (!cleanedInput.empty()) cleanedInput += ';';
                cleanedInput += path;

                std::string normPath = normalizePath(path);
                if (uniqueNormalizedPaths.insert(normPath).second) {
                    validPaths.push_back(normPath);
                }
            }

            if (!cleanedInput.empty()) {
                add_history(cleanedInput.c_str());
            }

            std::sort(validPaths.begin(), validPaths.end());

            std::vector<std::string> filteredPaths;
            for (const auto& p : validPaths) {
                if (filteredPaths.empty() || !p.starts_with(filteredPaths.back() + "/")) {
                    filteredPaths.push_back(p);
                }
            }
            validPaths = std::move(filteredPaths);

            if (validPaths.empty()) {
                flushStdin();
                restoreInput();
                resetReadlinePagination();
                if (!invalidPaths.empty()) {
                    GlobalState::g_operationCancelled.store(false);
                    saveAndReportResultsForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths,
                                                    uniqueErrorMessages, newISOFound, start_time,
                                                    repaintCallerPrompt);
                }
                continue;
            }

            {
                auto& pool = getStaticThreadPool();
                std::vector<std::future<void>> futures;
                std::mutex processMutex;
                std::mutex traverseErrorMutex;
                GlobalState::g_operationCancelled.store(false);

                for (const auto& validPath : validPaths) {
                    futures.emplace_back(
                        pool.enqueue([path = validPath, &allIsoFiles, &uniqueErrorMessages, &totalFiles,
                                      &processMutex, &traverseErrorMutex, &maxDepth, &promptFlag]() {
                            traverse(path, allIsoFiles, uniqueErrorMessages, totalFiles,
                                     processMutex, traverseErrorMutex, maxDepth, promptFlag);
                        })
                    );
                }

                for (auto& future : futures) {
                    future.wait();
                }
            }

            flushStdin();
            restoreInput();
            resetReadlinePagination();

            std::cout << "\r" << color << "Total files processed: " << totalFiles;

            if (!invalidPaths.empty() || !validPaths.empty()) {
                std::cout << "\n";
            }

            if (!validPaths.empty() && !input.empty()) {
                saveHistory(filterHistory);
            }
            saveAndReportResultsForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, newISOFound, start_time, repaintCallerPrompt);

        } catch (const std::exception& e) {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();

            auto render = [&] {
                SyncOutput sync;
                clearScrollBuffer();
                std::cerr << "\n" << dt.red << "Unable to access ISO database: "
                          << e.what() << dt.reset << std::endl;
            };
            pressEnterToContinueWithRepaint(render);
        }
    }
}

/**
 * @brief Recursively traverses a directory to find ISO files.
 *
 * Depth-first walk of the filesystem from @p path, collecting .iso files and
 * aggregating errors. Results are batch-appended to the shared output under
 * @p traverseFilesMutex.
 *
 * @param path               Starting directory.
 * @param isoFiles           Output vector for discovered ISO paths.
 * @param uniqueErrorMessages Set of unique error messages.
 * @param totalFiles         Atomic counter of files processed (for progress).
 * @param traverseFilesMutex Protects @p isoFiles.
 * @param traverseErrorsMutex Protects @p uniqueErrorMessages.
 * @param maxDepth           Max recursion depth (-1 for unlimited).
 * @param promptFlag         If true, displays progress updates.
 */
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles,
              std::unordered_set<std::string>& uniqueErrorMessages,
              std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex,
              std::mutex& traverseErrorsMutex, int maxDepth, bool promptFlag) {

    const size_t BATCH_SIZE = 100;
    std::vector<std::string> localIsoFiles;
    std::atomic<bool> g_CancelledMessageAdded{false};

    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    auto iequals = [](const std::string_view& a, const std::string_view& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                         [](unsigned char a, unsigned char b) {
                             return std::tolower(a) == std::tolower(b);
                         });
    };

    try {
        auto options = std::filesystem::directory_options::none;
        for (auto it = std::filesystem::recursive_directory_iterator(path, options);
             it != std::filesystem::recursive_directory_iterator(); ++it) {

            if (GlobalState::g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(traverseErrorsMutex);
                    uniqueErrorMessages.clear();

                    std::string msg = "\n" + dt.yellow + "ISO search interrupted by user" + dt.reset;
                    uniqueErrorMessages.insert(msg);
                }
                break;
            }

            if (maxDepth >= 0 && it.depth() > maxDepth) {
                it.disable_recursion_pending();
                continue;
            }

            const auto& entry = *it;

            if (promptFlag && entry.is_regular_file()) {
                uint64_t val = totalFiles.fetch_add(1, std::memory_order_relaxed) + 1;
                if (val % 100 == 0) {
                    std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
                    std::cout << "\r" << color << "Total files processed: " << val << std::flush;
                }
            }

            if (!entry.is_regular_file()) continue;

            const auto& filePath = entry.path();

            if (!iequals(filePath.extension().string(), ".iso")) continue;

            localIsoFiles.push_back(filePath.string());

            if (localIsoFiles.size() >= BATCH_SIZE) {
                std::lock_guard<std::mutex> lock(traverseFilesMutex);
                isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
                localIsoFiles.clear();
            }
        }

        if (!localIsoFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
        }

    } catch (const std::filesystem::filesystem_error& e) {
        std::string formattedError = "\n" + dt.red + "Error: " + path.string() + " - " +
                                     e.what() + dt.reset;

        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
            uniqueErrorMessages.insert(formattedError);
        }
    }
}

/**
 * @brief Prepares and optionally displays the contents of a disc-image RAM
 * cache.
 *
 * Copies the active cache (BIN/IMG, MDF, NRG, CHD, or DAA/GBI) into the
 * output vector when listing is enabled. If the cache is empty, prints a
 * status message and waits for user input. Handles terminal state (signal
 * ignoring, input control, scroll-buffer clearing).
 *
 * @param files             Output vector populated with cached file paths when
 *                          available.
 * @param list              Enables display/listing mode and user interaction.
 * @param fileExtension     Display-only string used in status messages.
 * @param binImgFilesCache  Snapshot of BIN/IMG cache.
 * @param mdfMdsFilesCache  Snapshot of MDF cache.
 * @param nrgFilesCache     Snapshot of NRG cache.
 * @param chdFilesCache     Snapshot of CHD cache.
 * @param daaGbiFilesCache  Snapshot of DAA/GBI cache.
 * @param modeMdf           Select MDF cache mode.
 * @param modeNrg           Select NRG cache mode.
 * @param modeChd           Select CHD cache mode.
 * @param modeDaa           Select DAA/GBI cache mode.
 */
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension,
                  const std::vector<std::string>& binImgFilesCache,
                  const std::vector<std::string>& mdfMdsFilesCache,
                  const std::vector<std::string>& nrgFilesCache,
                  const std::vector<std::string>& chdFilesCache,
                  const std::vector<std::string>& daaGbiFilesCache,
                  bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa) {

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    bool isEmpty = false;
    if (modeDaa) {
        isEmpty = daaGbiFilesCache.empty();
    } else if (modeChd) {
        isEmpty = chdFilesCache.empty();
    } else if (modeMdf) {
        isEmpty = mdfMdsFilesCache.empty();
    } else if (modeNrg) {
        isEmpty = nrgFilesCache.empty();
    } else {
        isEmpty = binImgFilesCache.empty();
    }

    if (isEmpty && list) {
        auto render = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << "\n" << dt.yellow << "No cached " << fileExtension
                      << " entries to display.\033[J"
                      << dt.reset << "\n";
        };

        pressEnterToContinueWithRepaint(render);

        files.clear();

        clearScrollBuffer();
        return;

    } else if (list) {
        if (modeDaa) {
            files = daaGbiFilesCache;
        } else if (modeChd) {
            files = chdFilesCache;
        } else if (modeMdf) {
            files = mdfMdsFilesCache;
        } else if (modeNrg) {
            files = nrgFilesCache;
        } else {
            files = binImgFilesCache;
        }
    }
}

/**
 * @brief Clears the active disc-image cache and related transformation entries.
 *
 * Clears exactly one cache based on the selected mode (CHD, DAA/GBI, MDF,
 * NRG, or BIN/IMG by default). Handles terminal/UI state (signal reset, input
 * control) and prints a status message.
 *
 * @param modeMdf Select MDF cache mode (mutually exclusive).
 * @param modeNrg Select NRG cache mode (mutually exclusive).
 * @param modeChd Select CHD cache mode (mutually exclusive).
 * @param modeDaa Select DAA/GBI cache mode (mutually exclusive).
 */
void clearRamCache(bool& modeMdf, bool& modeNrg, bool& modeChd, bool& modeDaa) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    std::vector<std::string> extensions;
    std::string cacheType;
    bool cacheIsEmpty = false;

    if (modeDaa) {
        extensions = {".daa", ".gbi"};
        cacheType = "DAA/GBI";
        cacheIsEmpty = GlobalState::daaGbiFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::daaGbiFilesCache);
    } else if (modeChd) {
        extensions = {".chd"};
        cacheType = "CHD";
        cacheIsEmpty = GlobalState::chdFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::chdFilesCache);
    } else if (modeMdf) {
        extensions = {".mdf"};
        cacheType = "MDF";
        cacheIsEmpty = GlobalState::mdfMdsFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::mdfMdsFilesCache);
    } else if (modeNrg) {
        extensions = {".nrg"};
        cacheType = "NRG";
        cacheIsEmpty = GlobalState::nrgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::nrgFilesCache);
    } else {
        extensions = {".bin", ".img"};
        cacheType = "BIN/IMG";
        cacheIsEmpty = GlobalState::binImgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::binImgFilesCache);
    }

    auto render = [&, cacheType, cacheIsEmpty] {
        SyncOutput sync;
        clearScrollBuffer();

        if (cacheIsEmpty) {
            std::cout << "\n" << dt.yellow << cacheType
                      << " cache is empty. Nothing to clear.\033[J"
                      << dt.reset << "\n";
        } else {
            std::cout << "\n" << dt.green << cacheType
                      << " cache cleared.\033[J"
                      << dt.reset << "\n";
        }
    };

    render();
    pressEnterToContinueWithRepaint(render);

    clearScrollBuffer();
}

void toLowerInPlace(std::string& str);

/**
 * @brief Mode-based extension filter for disc-image files.
 *
 * Allows exactly one category at a time:
 * - BIN/IMG (default)
 * - MDF
 * - NRG
 * - CHD
 * - DAA/GBI
 *
 * A keyword-based exclusion pass is present but currently has an empty
 * keyword set.
 *
 * @return true if @p entry matches the active mode filter, false otherwise.
 */
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg, const bool& blacklistChd, const bool& blacklistDaa) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();
    std::string extLower = ext;
    toLowerInPlace(extLower);

    if (blacklistDaa) {
        if (!(extLower == ".daa" || extLower == ".gbi")) {
            return false;
        }
    }
    else if (blacklistChd) {
        if (extLower != ".chd") {
            return false;
        }
    }
    else if (blacklistMdf) {
        if (extLower != ".mdf") {
            return false;
        }
    }
    else if (blacklistNrg) {
        if (extLower != ".nrg") {
            return false;
        }
    }
    else { // default BIN/IMG mode
        if (!(extLower == ".bin" || extLower == ".img")) {
            return false;
        }
    }

    std::unordered_set<std::string> blacklistKeywords = {};

    std::string filenameLowerNoExt = filenameLower;
    filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size());

    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Recursively scans one directory for disc-image files as part of a
 * parallel search.
 *
 * Walks the directory tree, filters by mode (BIN/IMG, MDF, NRG, CHD, or
 * DAA/GBI), applies cache checks and blacklist filtering, and invokes the
 * callback for newly discovered files. Supports cancellation, progress
 * reporting, and error aggregation for the parent search.
 *
 * @par Cache check
 * The target cache is copied once into a hash set at the start of the task,
 * so each candidate is an O(1) lookup instead of a linear scan. This is safe
 * because findFiles only modifies the caches after every task has been
 * joined.
 *
 * @par Cancellation
 * The flag is read here but never reset; findFiles resets it once before
 * submitting the tasks. Resetting it per task would let late-starting tasks
 * swallow a Ctrl+C.
 *
 * @param totalFiles Shared atomic counter of regular files seen, owned by the
 *                   caller and shared by every parallel task so the live
 *                   progress line and the final summary show the combined
 *                   total. This function only increments it; the final
 *                   "Total files processed" line is printed by the caller.
 * @return Set of newly discovered file paths from this directory.
 */
std::unordered_set<std::string> processPaths(const std::string& path, const std::string& mode,
                                            const std::function<void(const std::string&, const std::string&)>& callback,
                                            std::unordered_set<std::string>& processedErrorsFind,
                                            std::atomic<size_t>& totalFiles) {
    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    std::unordered_set<std::string> localFileNames;
    std::atomic<bool> g_CancelledMessageAdded{false};

    disableInput();

    try {
        const bool blacklistMdf = (mode == "mdf");
        const bool blacklistNrg = (mode == "nrg");
        const bool blacklistChd = (mode == "chd");
        const bool blacklistDaa = (mode == "daa");

        std::unordered_set<std::string> cacheSet;
        {
            std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
            const std::vector<std::string>& cache =
                blacklistNrg ? GlobalState::nrgFilesCache :
                blacklistMdf ? GlobalState::mdfMdsFilesCache :
                blacklistChd ? GlobalState::chdFilesCache :
                blacklistDaa ? GlobalState::daaGbiFilesCache :
                               GlobalState::binImgFilesCache;
            cacheSet.insert(cache.begin(), cache.end());
        }

        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (GlobalState::g_operationCancelled.load()) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);

                if (!g_CancelledMessageAdded.exchange(true)) {
                    processedErrorsFind.clear();

                    std::string type = (blacklistMdf) ? "MDF" :
                                       (blacklistNrg) ? "NRG" :
                                       (blacklistChd) ? "CHD" :
                                       (blacklistDaa) ? "DAA/GBI" : "BIN/IMG";

                    processedErrorsFind.insert(dt.yellow + type +
                        " search interrupted by user\n\n" + dt.reset);
                }
                break;
            }

            if (!entry.is_regular_file()) continue;

            uint64_t val = totalFiles.fetch_add(1, std::memory_order_relaxed) + 1;
            if (val % 100 == 0) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
                std::cout << "\r" << color << "Total files processed: " << val << std::flush;
            }

            if (blacklist(entry, blacklistMdf, blacklistNrg, blacklistChd, blacklistDaa)) {
                std::string fileName = entry.path().string();

                std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
                if (!cacheSet.contains(fileName) && localFileNames.insert(fileName).second) {
                    callback(fileName, entry.path().parent_path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);

        processedErrorsFind.insert(dt.red + "Error traversing path: " + path + " - " +
                                 e.what() + dt.reset);
    }

    return localFileNames;
}

/**
 * @brief Discovers disc-image files across multiple directories using a
 * thread pool.
 *
 * Normalizes input paths (trailing slash, redundant components), deduplicates
 * semantically identical paths, and filters out subdirectories already
 * covered by a parent path, so each tree is scanned exactly once. Submits the
 * filtered scan tasks to the shared static thread pool; each task walks its
 * directory recursively and returns discovered BIN/IMG/MDF/NRG/CHD/DAA/GBI
 * files.
 *
 * Results are aggregated, deduplicated against the existing cache, and merged
 * into the appropriate RAM cache. Supports cancellation via the global
 * operation flag and signal handling. All tasks are always joined before this
 * function returns (they hold references to this function's parameters), even
 * after a cancellation.
 *
 * @par Resize contract
 * This function intentionally does NOT call verboseFind(). The
 * invalid-directory set and the processed-error set are returned to the
 * caller still populated, so verboseImageSearchResults() can run verboseFind()
 * once into a buffer and replay its output on resize. Likewise @p totalFiles
 * is left holding the final aggregate so verboseImageSearchResults() can
 * reprint "Total files processed" after it clears the screen.
 *
 * @param inputPaths           Raw user-provided directory paths.
 * @param fileNames            Output set of discovered file paths.
 * @param currentCacheOld      Returns the cache size before new additions.
 * @param mode                 File type to search for ("bin"/"img", "mdf",
 *                             "nrg", "chd", "daa").
 * @param callback             Optional callback invoked for each discovered
 *                             file.
 *
 * @param processedErrorsFind  Set of error messages encountered during
 *                             traversal; left populated for
 *                             verboseImageSearchResults().
 * @param totalFiles           Shared counter of regular files processed across
 *                             all tasks; incremented by processPaths(), read by
 *                             the caller afterwards.
 * @return Reference to the updated cache vector for @p mode.
 */
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths,
                                   std::unordered_set<std::string>& fileNames,
                                   int& currentCacheOld, const std::string& mode,
                                   const std::function<void(const std::string&, const std::string&)>& callback,
                                   std::unordered_set<std::string>& processedErrorsFind,
                                   std::atomic<size_t>& totalFiles) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    disableInput();

    std::vector<std::string>* currentCache = nullptr;
    if (mode == "bin" || mode == "img") {
        currentCacheOld = GlobalState::binImgFilesCache.size();
        currentCache = &GlobalState::binImgFilesCache;
    } else if (mode == "mdf") {
        currentCacheOld = GlobalState::mdfMdsFilesCache.size();
        currentCache = &GlobalState::mdfMdsFilesCache;
    } else if (mode == "nrg") {
        currentCacheOld = GlobalState::nrgFilesCache.size();
        currentCache = &GlobalState::nrgFilesCache;
    } else if (mode == "chd") {
        currentCacheOld = GlobalState::chdFilesCache.size();
        currentCache = &GlobalState::chdFilesCache;
    } else if (mode == "daa") {
        currentCacheOld = GlobalState::daaGbiFilesCache.size();
        currentCache = &GlobalState::daaGbiFilesCache;
    } else {
        restoreInput();
        return {};
    }

    auto normalizePath = [](const std::string& p) {
        std::string n = fs::path(p).lexically_normal().string();
        if (n.size() > 1 && n.back() == '/')
            n.pop_back();
        return n;
    };

    std::unordered_set<std::string> uniqueNormalizedPaths;
    std::vector<std::string> normalizedPaths;

    for (const auto& originalPath : inputPaths) {
        std::string norm = normalizePath(originalPath);
        if (uniqueNormalizedPaths.insert(norm).second) {
            normalizedPaths.push_back(norm);
        }
    }

    std::sort(normalizedPaths.begin(), normalizedPaths.end());

    std::vector<std::string> filteredPaths;
    for (const auto& p : normalizedPaths) {
        if (filteredPaths.empty() || !p.starts_with(filteredPaths.back() + "/")) {
            filteredPaths.push_back(p);
        }
    }

    if (filteredPaths.empty()) {
        flushStdin();
        restoreInput();
        return *currentCache;
    }

    std::vector<std::future<std::unordered_set<std::string>>> threadFutures;

    {
        auto& pool = getStaticThreadPool();

        for (const auto& path : filteredPaths) {
            std::string pathCopy = path;

            threadFutures.push_back(pool.enqueue(
                [path = std::move(pathCopy), &mode, &callback, &processedErrorsFind, &totalFiles]()
                    -> std::unordered_set<std::string> {
                    return processPaths(path, mode, callback, processedErrorsFind, totalFiles);
                }));
        }

        for (auto& future : threadFutures) {
            if (future.valid()) future.wait();
        }

        for (auto& future : threadFutures) {
            if (future.valid()) {
                std::unordered_set<std::string> threadResult = future.get();
                fileNames.insert(threadResult.begin(), threadResult.end());
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
        std::cout << "\r" << color << "Total files processed: " << totalFiles.load() << std::flush;
    }

    std::unordered_set<std::string> currentCacheSet(currentCache->begin(), currentCache->end());
    std::vector<std::string> newFiles;

    for (const auto& fileName : fileNames) {
        if (currentCacheSet.insert(fileName).second) {
            newFiles.push_back(fileName);
        }
    }

    if (!newFiles.empty()) {
        currentCache->insert(currentCache->end(), newFiles.begin(), newFiles.end());
    }

    flushStdin();
    restoreInput();

    return *currentCache;
}

void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, bool& list, std::shared_ptr<RefreshState> state);

/**
 * @brief Dispatches special command inputs during the BIN/IMG/MDF/NRG/CHD/DAA/GBI
 * search UI.
 *
 * Command router for the search interface: handles cache operations and help
 * output. Some commands trigger downstream actions such as cache listing and
 * file selection.
 *
 * @return true if @p input was handled as a special command, false otherwise.
 */
bool dispatchSpecialCommandForBinImgMdfNrgSearch(const std::string& input,
                                                  bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa,
                                                  const std::string& fileExtension,
                                                  std::vector<std::string>& files, const std::string& fileType,
                                                  bool& list,
                                                  std::shared_ptr<RefreshState> state) {

    if (input == "*stats") {
        displayDatabaseStatistics(GlobalState::databaseFilePath, GlobalState::maxDatabaseSize);
        return true;
    }
    if (input == "!clr_paths" || input == "!clr_filter") {
        clearHistory(input);
        return true;
    }
    if (input == "?") {
        bool isCpMv = false, import2ISO = false;
        helpSearches(isCpMv, import2ISO, false);
        return true;
    }
    if (input == "!clr") {
        clearRamCache(modeMdf, modeNrg, modeChd, modeDaa);
        return true;
    }
    if (input == "ls") {
        list = true;
        ramCacheList(files, list, fileExtension,
                     GlobalState::binImgFilesCache, GlobalState::mdfMdsFilesCache, GlobalState::nrgFilesCache, GlobalState::chdFilesCache, GlobalState::daaGbiFilesCache,
                     modeMdf, modeNrg, modeChd, modeDaa);
        if (!files.empty())
            selectForImageFiles(fileType, files, list, state);
        return true;
    }
    return false;
}

/**
 * @brief Interactive search and caching controller for disc-image files.
 *
 * Terminal interface for scanning user-provided directories for BIN/IMG, MDF,
 * NRG, CHD, and DAA/GBI images. Supports multi-path input
 * (semicolon-delimited), directory validation with deduplication, readline
 * history management, special command dispatch, and verbose result reporting.
 *
 * Discovered files are cached in memory and, if new files were found and the
 * operation was not cancelled, forwarded to selectForImageFiles for
 * interactive selection and conversion.
 *
 * @par Control flow
 * - Exit: ESC input ("\x1b", typically bound to '<' via
 *   setup_custom_keybindingsForSearches) or a null return from readline
 *   (Ctrl+D) — both return directly.
 * - Continue: empty or whitespace-only input, special commands handled by
 *   dispatchSpecialCommandForBinImgMdfNrgSearch, and scans that find no new
 *   files.
 * - File-type dispatch: @p fileTypeChoice is mapped via a static
 *   unordered_map to derive the file extension, display name, and per-format
 *   flags (modeMdf, modeNrg, modeChd, modeDaa) used downstream.
 * - Path parsing: input is split on ';'. Empty and duplicate tokens are
 *   skipped; each remaining token is validated with isValidDirectory. Valid
 *   tokens go into directoryPaths for scanning; invalid ones (stored
 *   uncolored) into invalidDirectoryPaths for reporting only.
 * - Progress counter: each iteration owns a std::atomic<size_t> totalFiles
 *   that is passed to findFiles (shared by all scan tasks) and then to
 *   verboseImageSearchResults, so the final count can be reprinted after the
 *   results screen clears the terminal.
 * - History: only valid, deduplicated paths are saved to history, rejoined
 *   with ';' in their original order (e.g. the input "/home/;/var/;asd" is
 *   stored as "/home/;/var/"). Invalid paths omitted from the search are
 *   never persisted. If no valid directory was found, nothing is added to
 *   history.
 * - RAII keybinding management: a SearchReadlineGuard is constructed at the
 *   top of each loop iteration (after initIterationState). Its destructor
 *   restores the saved rl_attempted_completion_function, rebinds '\t' to
 *   prevent_readline_keybindings, rebinds '\f' to rl_insert, clears readline
 *   history, resets readline pagination, and calls
 *   reset_custom_keybindingsForSearches(). The guard is destroyed at the end
 *   of each iteration (or on any exit path such as ESC/Ctrl+D or exception),
 *   so cleanup runs once per loop cycle.
 * - Iteration state: initIterationState is called at the top of each
 *   iteration to re-establish keybindings, signal handlers, pagination,
 *   completion function, and cancellation state before prompting.
 * - Resize repaint (prompt): the prompt carries its own header line, so
 *   readline() is wrapped by readlineWithResize() with a callback that clears
 *   the screen and re-prints the header (via promptHeaderForRepaint). The
 *   watcher adds the newline and Readline redraws the last prompt line and
 *   the typed input.
 * - Resize repaint (results screen): after the scan, the results block is
 *   shown by verboseImageSearchResults, which owns its own resize handling
 *   via pressEnterToContinueWithRepaint. This function supplies
 *   repaintCallerPrompt — a callback that reprints the FolderPaths prompt
 *   line — so that a resize while the user reads the results keeps the
 *   question the user just answered visible above the block. The callback is
 *   invoked inside verboseImageSearchResults's render, before the block is
 *   printed.
 *
 * @param fileTypeChoice The format category: "bin", "img", "mdf", "nrg",
 *                       "chd", or "daa". "bin" and "img" are treated
 *                       identically.
 * @param state          Shared RefreshState forwarded to selectForImageFiles
 *                       for display coordination.
 */
void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::shared_ptr<RefreshState> state) {
    PendingReadlineTextClearGuard pendingTextGuard;

    struct FileTypeConfig {
        std::string extension;
        std::string name;
    };

    static const std::unordered_map<std::string, FileTypeConfig> fileTypeMap = {
        {"bin", {".bin/.img", "BIN/IMG"}},
        {"img", {".bin/.img", "BIN/IMG"}},
        {"mdf", {".mdf",      "MDF"    }},
        {"nrg", {".nrg",      "NRG"    }},
        {"chd", {".chd",      "CHD"    }},
        {"daa", {".daa/.gbi", "DAA/GBI"}}
    };

    const auto configIt = fileTypeMap.find(fileTypeChoice);
    if (configIt == fileTypeMap.end()) {
        const VerboseAndDatabaseTheme dt = getDatabaseTheme();
        std::cout << dt.red << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG, CHD, DAA" << dt.reset << "\n";
        return;
    }

    const std::string& fileType      = fileTypeChoice;
    const std::string& fileExtension = configIt->second.extension;
    const bool modeMdf = (fileType == "mdf");
    const bool modeNrg = (fileType == "nrg");
    const bool modeChd = (fileType == "chd");
    const bool modeDaa = (fileType == "daa");

    std::vector<std::string> files;
    files.reserve(1000);

    auto initIterationState = [&]() {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        rl_attempted_completion_function = my_special_completion_entry;
        setup_custom_keybindingsForSearches();
        GlobalState::g_operationCancelled.store(false);
        clearScrollBuffer();
        bool filterHistory = false;
        loadHistory(filterHistory);
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 1;
    };

    while (true) {
        int currentCacheOld = 0;
        std::vector<std::string> directoryPaths;
        std::unordered_set<std::string> uniquePaths, processedErrors, processedErrorsFind;
        std::unordered_set<std::string> invalidDirectoryPaths, fileNames;
        std::atomic<size_t> totalFiles{0};
        bool filterHistory = false;

        initIterationState();
        SearchReadlineGuard guard;

        RestoreReadlineBuffer();

        const VerboseAndDatabaseTheme dt = getDatabaseTheme();

        std::string prompt;
        prompt.reserve(512);
        prompt.append("\001").append(dt.green).append("\002FolderPaths")
              .append("\001").append(dt.blue).append("\002 ↵ to scan for \001")
              .append("\001").append(dt.orange).append("\002").append(fileExtension)
              .append("\001").append(dt.blue).append("\002 entries and cache in \001")
              .append("\001").append(dt.yellow).append("\002RAM\001")
              .append("\001").append(dt.blue).append("\002, ? for help:\n\001")
              .append(dt.reset).append("\002");

        const auto repaintPrompt = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << promptHeaderForRepaint(prompt);
        };

        // Reprints the FolderPaths prompt line on a resize during the
        // verboseImageSearchResults wait, so the question the user just
        // answered stays visible above the results block.
        const auto repaintCallerPrompt = [&] {
            std::cout << promptHeaderForRepaint(prompt) << "\n";
        };

        RlLine searchQuery = readlineWithResize(prompt, repaintPrompt);
        if (!searchQuery) return;

        armPendingInputRestore();
        rl_startup_hook = nullptr;

        const std::string inputSearch = trimWhitespace(searchQuery.get());

        if (inputSearch == "\x1b") return;

        if (inputSearch.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }

        bool list = false;

        if (dispatchSpecialCommandForBinImgMdfNrgSearch(inputSearch, modeMdf, modeNrg, modeChd, modeDaa,
                                   fileExtension, files, fileType, list, state))
            continue;

        std::cout << " \n\033[3H\033[J\n";

        std::istringstream ss(inputSearch);
        std::string path;
        std::string cleanedInput;
        while (std::getline(ss, path, ';')) {
            if (!path.empty() && uniquePaths.insert(path).second) {
                if (isValidDirectory(path)) {
                    directoryPaths.push_back(path);
                    if (!cleanedInput.empty()) cleanedInput += ';';
                    cleanedInput += path;
                } else {
                    invalidDirectoryPaths.insert(path);
                }
            }
        }

        bool newFilesFound = false;
        const auto start_time = std::chrono::high_resolution_clock::now();

        files = findFiles(directoryPaths, fileNames, currentCacheOld, fileType,
                          [&](const std::string&, const std::string&) { newFilesFound = true; },
                          processedErrorsFind, totalFiles);

        try {
            if (!directoryPaths.empty()) {
                add_history(cleanedInput.c_str());
                saveHistory(filterHistory);
            }
        } catch (const std::exception& e) {
            std::cerr << "\n\n" << dt.red << "Unable to access local database: " << e.what()
                      << dt.reset << std::endl;
        }

        verboseImageSearchResults(fileExtension, fileNames, invalidDirectoryPaths,
                             newFilesFound, list, currentCacheOld, files,
                             start_time, processedErrorsFind, directoryPaths, totalFiles,
                             repaintCallerPrompt);

        if (!newFilesFound) continue;

        if (!files.empty() && !GlobalState::g_operationCancelled.load())
            selectForImageFiles(fileType, files, list, state);
    }
}
