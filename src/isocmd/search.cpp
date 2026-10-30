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
#include "../rendering.h"
#include "../pausePrompts.h"
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
 * @brief Checks whether a path exists and refers to a directory.
 *
 * @param path Path to check.
 * @return true if @p path is an existing directory, false otherwise.
 */
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}

/**
 * @brief Prompts for directories and imports discovered ISO files into the database.
 *
 * Accepts semicolon-delimited directory paths, trims and validates each token,
 * removes duplicate normalized paths and paths covered by an earlier parent
 * directory, then scans the remaining directories in parallel up to
 * @p maxDepth. Discovered files and scan errors are passed to
 * saveAndReportResultsForDatabase().
 *
 * The prompt supports readline history, help and database commands. Invalid
 * paths are excluded from the history entry; valid paths are stored trimmed
 * but otherwise as entered. All scan tasks are joined before the iteration
 * ends, including after cancellation, because they reference iteration-local
 * data.
 *
 * @par Repaint
 * The line the user typed is kept as a std::string copy. Whenever the screen
 * is repainted after the prompt (the results report, database commands such
 * as "*stats" and "!clr*", or a terminal resize while waiting), the prompt
 * header is re-printed, followed by the typed line in the theme color on its
 * own line directly under it, so the verbose output below always starts on a
 * fresh line and the question the user answered stays visible. The typed
 * line is not restored into the next prompt, which always starts empty.
 *
 * @param promptFlag    Passed to traverse() to control progress and error
 *                      reporting.
 * @param maxDepth      Maximum directory recursion depth (-1 for unlimited).
 * @param filterHistory Whether to load and save readline history.
 * @param newISOFound   Passed to saveAndReportResultsForDatabase(), which
 *                      forwards it to saveToDatabase().
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

            // What the user typed on the prompt line, set right after readline
            // returns. Reprinted under the header so the repainted screen
            // matches what they saw, followed by the verbose output.
            std::string typedLine;

            // Header, then the typed line in `color` on its own line directly
            // under the prompt, then a final newline so whatever prints next
            // (verbose/error output) starts on a fresh line.
            const auto repaintCallerPrompt = [&] {
                const std::string header = promptHeaderForRepaint(prompt);
                std::cout << header;

                // Make sure the typed line starts on a new line: find the last
                // visible character of the header, skipping trailing ANSI SGR
                // sequences (e.g. "\033[0m") and readline's \001/\002 markers.
                size_t end = header.size();
                while (end > 0) {
                    const char c = header[end - 1];
                    if (c == '\001' || c == '\002') { --end; continue; }
                    if (c == 'm') {
                        const size_t esc = header.rfind('\033', end - 1);
                        if (esc != std::string::npos && esc + 1 < end && header[esc + 1] == '[') {
                            end = esc;
                            continue;
                        }
                    }
                    break;
                }
                if (end == 0 || header[end - 1] != '\n') std::cout << "\n";

                std::cout << color << typedLine << dt.reset << "\n";
            };

            RlLine searchQuery = readlineWithResize(prompt, repaintPrompt);
            if (!searchQuery) return;

            typedLine = searchQuery.get();

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
                databaseSwitches(input, repaintCallerPrompt);
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
 * Walks the directory tree depth-first, collecting regular files with a
 * case-insensitive ".iso" extension and aggregating filesystem errors.
 * Discovered paths are appended to the shared output in batches.
 *
 * @param path                Starting directory.
 * @param isoFiles            Output vector for discovered ISO paths.
 * @param uniqueErrorMessages Set of unique error messages.
 * @param totalFiles          Atomic counter of regular files processed for
 *                            progress reporting when @p promptFlag is true.
 * @param traverseFilesMutex  Protects @p isoFiles.
 * @param traverseErrorsMutex Protects @p uniqueErrorMessages.
 * @param maxDepth            Maximum recursion depth (-1 for unlimited).
 * @param promptFlag          Enables progress counting and error reporting.
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
 * @brief Copies the selected disc-image cache for listing.
 *
 * Selects the cache according to the active format mode. When @p list is true,
 * copies its contents into @p files; if the selected cache is empty, displays
 * a message, waits for input, and clears @p files.
 *
 * @param files             Receives the selected cache contents.
 * @param list              Enables listing and its user interaction.
 * @param fileExtension     Format label used in the empty-cache message.
 * @param binImgFilesCache  BIN/IMG cache snapshot.
 * @param mdfMdsFilesCache  MDF cache snapshot.
 * @param nrgFilesCache     NRG cache snapshot.
 * @param chdFilesCache     CHD cache snapshot.
 * @param daaGbiFilesCache  DAA/GBI cache snapshot.
 * @param modeMdf           Selects the MDF cache.
 * @param modeNrg           Selects the NRG cache.
 * @param modeChd           Selects the CHD cache.
 * @param modeDaa           Selects the DAA/GBI cache.
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

        pressEnterToReturnWithRepaint(render);

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
 * @brief Clears the cache selected by the active disc-image format mode.
 *
 * Releases the selected cache's storage and displays whether it contained
 * entries. The terminal prompt is repainted after the initial result render
 * when @p repaintHeader is provided.
 *
 * @param modeMdf       Selects the MDF cache.
 * @param modeNrg       Selects the NRG cache.
 * @param modeChd       Selects the CHD cache.
 * @param modeDaa       Selects the DAA/GBI cache.
 * @param repaintHeader Optional callback that redraws the caller's prompt.
 */
void clearRamCache(bool& modeMdf, bool& modeNrg, bool& modeChd, bool& modeDaa,
                   const std::function<void()>& repaintHeader) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    std::string cacheType;
    bool cacheIsEmpty = false;

    if (modeDaa) {
        cacheType = "DAA/GBI";
        cacheIsEmpty = GlobalState::daaGbiFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::daaGbiFilesCache);
    } else if (modeChd) {
        cacheType = "CHD";
        cacheIsEmpty = GlobalState::chdFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::chdFilesCache);
    } else if (modeMdf) {
        cacheType = "MDF";
        cacheIsEmpty = GlobalState::mdfMdsFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::mdfMdsFilesCache);
    } else if (modeNrg) {
        cacheType = "NRG";
        cacheIsEmpty = GlobalState::nrgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::nrgFilesCache);
    } else {
        cacheType = "BIN/IMG";
        cacheIsEmpty = GlobalState::binImgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(GlobalState::binImgFilesCache);
    }

    bool firstDraw = true;

        auto render = [&, cacheType, cacheIsEmpty] {
            SyncOutput sync;

            if (!firstDraw && repaintHeader) repaintHeader();
            firstDraw = false;

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


    pressEnterToContinueWithRepaint(render);

    clearScrollBuffer();
}

void toLowerInPlace(std::string& str);

/**
 * @brief Checks whether a file matches the selected disc-image format.
 *
 * Accepts BIN/IMG by default, or the MDF, NRG, CHD, or DAA/GBI category
 * selected by the corresponding mode flag. A filename-keyword exclusion
 * pass is also applied, although its keyword set is currently empty.
 *
 * @return true if @p entry passes the active format filter.
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
 * @brief Scans one directory for newly discovered disc-image files.
 *
 * Recursively visits regular files, counts each one for progress reporting,
 * and applies the selected format and filename filters. The active RAM cache
 * is copied into a lookup set once per task; matching paths not already cached
 * are added to the result set and passed to @p callback. Filesystem errors
 * are added to @p processedErrorsFind.
 *
 * The global cancellation flag is observed but not reset here. The caller
 * resets it before launching the parallel scan.
 *
 * @param path                Directory to scan.
 * @param mode                Format category ("bin", "img", "mdf", "nrg",
 *                            "chd", or "daa").
 * @param callback            Invoked for each newly discovered file while
 *                            @c globalSetsMutex is held.
 * @param processedErrorsFind Receives traversal errors and cancellation
 *                            information.
 * @param totalFiles          Shared atomic count of regular files visited.
 * @return Newly discovered file paths from this directory.
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
 * @brief Scans multiple directories and merges discovered files into a RAM cache.
 *
 * Lexically normalizes and deduplicates the input paths, then removes paths
 * covered by an already selected parent directory. The remaining directories
 * are scanned in parallel. After all tasks finish, their results are
 * aggregated, deduplicated against the selected cache, and appended to it.
 *
 * The operation-cancellation flag is reset before tasks are submitted. All
 * tasks are joined before returning because they capture references to caller
 * data. Error sets and @p totalFiles remain populated for the caller's result
 * display and resize repaint handling.
 *
 * @param inputPaths          Directories to scan.
 * @param fileNames           Receives discovered file paths.
 * @param currentCacheOld     Receives the cache size before additions.
 * @param mode                Format category ("bin"/"img", "mdf", "nrg",
 *                            "chd", or "daa").
 * @param callback            Optional callback for each newly discovered file.
 * @param processedErrorsFind Receives scan errors and cancellation information.
 * @param totalFiles          Shared count of regular files visited by all tasks.
 * @return A copy of the updated cache for @p mode; an empty vector for an
 *         unsupported mode.
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
 * @brief Handles special commands in the disc-image search interface.
 *
 * Dispatches database statistics, history clearing, help, cache clearing, and
 * cache listing commands. Listing may proceed to the interactive file
 * selection interface.
 *
 * @return true if @p input was handled, false otherwise.
 */
bool dispatchSpecialCommandForBinImgMdfNrgSearch(const std::string& input,
                                                 bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa,
                                                 const std::string& fileExtension,
                                                 std::vector<std::string>& files,
                                                 const std::string& fileType,
                                                 bool& list,
                                                 std::shared_ptr<RefreshState> state,
                                                 const std::function<void()>& repaintHeader) {

    if (input == "*stats") {
        displayDatabaseStatistics(GlobalState::databaseFilePath, GlobalState::maxDatabaseSize);
        return true;
    }
    if (input == "!clr_paths" || input == "!clr_filter") {
        clearHistory(input, repaintHeader);
        return true;
    }
    if (input == "?") {
        bool isCpMv = false, import2ISO = false;
        helpSearches(isCpMv, import2ISO, false);
        return true;
    }
    if (input == "!clr") {
        clearRamCache(modeMdf, modeNrg, modeChd, modeDaa, repaintHeader);
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
 * @brief Runs the interactive search and caching workflow for disc-image files.
 *
 * Prompts for semicolon-delimited directory paths, skips empty and exact
 * duplicate tokens, validates the remaining tokens, scans valid directories
 * in parallel, updates the selected RAM cache, and displays the results.
 * Tokens are not trimmed before validation. Special commands provide help,
 * statistics, history clearing, cache clearing, and cached-file listing.
 *
 * Only valid directory tokens are added to readline history, preserving
 * their entered text and order. If new files are found and the scan was not
 * cancelled, the user is taken to the file-selection interface.
 *
 * @par Repaint
 * The line the user typed is kept as a std::string copy. Whenever the screen
 * is repainted after the prompt (the results report from
 * verboseImageSearchResults(), the output of special commands, or a terminal
 * resize while waiting), the prompt header is re-printed, followed by the
 * typed line in the theme color on its own line directly under it, so the
 * verbose output below always starts on a fresh line and the question the
 * user answered stays visible. The typed line is not restored into the next
 * prompt, which always starts empty.
 *
 * @param fileTypeChoice Format category: "bin", "img", "mdf", "nrg", "chd",
 *                       or "daa". "bin" and "img" use the same cache.
 * @param state          Shared refresh state forwarded to file selection.
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

        // What the user typed on the prompt line, set right after readline
        // returns. Reprinted under the header so the repainted screen matches
        // what they saw, followed by the verbose output.
        std::string typedLine;

        // Reprints the FolderPaths prompt on a resize during the
        // verboseImageSearchResults wait (and for special commands): header,
        // then the typed line in `color` on its own line directly under the
        // prompt, then a final newline so whatever prints next starts on a
        // fresh line.
        const auto repaintCallerPrompt = [&] {
            const std::string header = promptHeaderForRepaint(prompt);
            std::cout << header;

            // Make sure the typed line starts on a new line: find the last
            // visible character of the header, skipping trailing ANSI SGR
            // sequences (e.g. "\033[0m") and readline's \001/\002 markers.
            size_t end = header.size();
            while (end > 0) {
                const char c = header[end - 1];
                if (c == '\001' || c == '\002') { --end; continue; }
                if (c == 'm') {
                    const size_t esc = header.rfind('\033', end - 1);
                    if (esc != std::string::npos && esc + 1 < end && header[esc + 1] == '[') {
                        end = esc;
                        continue;
                    }
                }
                break;
            }
            if (end == 0 || header[end - 1] != '\n') std::cout << "\n";

            std::cout << color << typedLine << dt.reset << "\n";
        };

        RlLine searchQuery = readlineWithResize(prompt, repaintPrompt);
        if (!searchQuery) return;

        typedLine = searchQuery.get();

        armPendingInputRestore();
        rl_startup_hook = nullptr;

        const std::string inputSearch = trimWhitespace(searchQuery.get());

        if (inputSearch == "\x1b") return;

        if (inputSearch.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }

        bool list = false;

        if (dispatchSpecialCommandForBinImgMdfNrgSearch(inputSearch, modeMdf, modeNrg, modeChd, modeDaa,
                                   fileExtension, files, fileType, list, state,
                                   repaintCallerPrompt))
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
