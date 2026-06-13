// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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
#include "../globalMutexes.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../globalMutexes.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../search.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../verbose.h"
#include "../sharedRefreshState.h"

namespace fs = std::filesystem;

/**
 * @file database_operations.cpp
 * @brief Database operations and file scanning functionality for ISO and disc image files
 *
 * This file contains functions for scanning directories for ISO files, managing the ISO database,
 * handling RAM caches for BIN/IMG/MDF/NRG files, and providing interactive user interfaces
 * for file selection and management.
 */

//=============================================================================
// General Section
//=============================================================================
/**
 * @brief Checks if a given path is a valid directory
 * @param path The filesystem path to check
 * @return true if the path exists and is a directory, false otherwise
 */
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}

//=============================================================================
// ISO Section
//=============================================================================

/**
 * @brief Prompts the user for directory paths and imports discovered ISO files
 *        into the local database.
 *
 * Reads a semicolon-delimited list of directory paths from the user, validates
 * and trims each path, deduplicates by normalized form, sorts, then eliminates
 * subdirectories of already-included paths (prefix check requires prior sort).
 * Remaining paths are scanned in parallel via a static thread pool up to
 * @p maxDepth. Results are forwarded to @c saveAndReportResultsForDatabase,
 * which calls @c saveToDatabase and may mutate @p newISOFound.
 *
 * @details **Control flow:**
 * - **Exit conditions:** @c '<' input or null return from readline (Ctrl+D).
 * - **Continue conditions:** '?' (shows help via @c helpSearches), switch commands
 *   (@c *stats, @c !clr, @c !clr_paths, @c !clr_filter via @c databaseSwitches),
 *   empty/whitespace-only input, empty @p validPaths with no invalid paths, and
 *   exceptions (prints error, waits for Enter, re-arms and retries).
 * - **Early save path:** If all paths are invalid (validPaths empty, invalidPaths
 *   non-empty), resets @c g_operationCancelled and calls
 *   @c saveAndReportResultsForDatabase without scanning.
 * - **Cancellation guarding:** @c g_operationCancelled is reset to @c false at
 *   both the early-save and pre-scan sites to prevent spurious cancellation from
 *   a premature Ctrl+C before input is processed.
 * - **Keybinding reset:** A @c KeybindingGuard RAII object is constructed each
 *   iteration; its destructor calls @c reset_custom_keybindingsForSearches,
 *   ensuring cleanup on both normal and exceptional exits from the loop body.
 *
 * @param promptFlag    Passed to @c traverse() to control scan behaviour.
 * @param maxDepth      Maximum directory recursion depth (-1 for unlimited).
 * @param filterHistory If true, readline history is loaded at prompt time and
 *                      saved after a successful scan via @c loadHistory /
 *                      @c saveHistory.
 * @param newISOFound   Passed by reference into @c saveAndReportResultsForDatabase,
 *                      which forwards it to @c saveToDatabase as a pointer;
 *                      @c saveToDatabase is responsible for setting it.
 */
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, bool& newISOFound) {
    struct KeybindingGuard {
        ~KeybindingGuard() {
            rl_bind_key('\t', prevent_readline_keybindings);
            reset_custom_keybindingsForSearches();
        }
    };

    auto rearmSetup = [&]() {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        resetReadlinePagination();
        setup_custom_keybindingsForSearches();
        rl_attempted_completion_function = my_special_completion_entry;
        GlobalState::g_operationCancelled.store(false);
    };

    while (true) {
        rearmSetup();
        KeybindingGuard guard;
        try {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();

            clearScrollBuffer();
            loadHistory(filterHistory);

            rl_bind_key('\f', clear_screen_and_buffer);
            rl_bind_key('\t', my_rl_complete);

            std::string prompt;
            prompt.reserve(512);
            prompt.append("\001").append(dt.green).append("\002FolderPaths")
                  .append("\001").append(dt.blue).append("\002 ↵ to scan for ")
                  .append("\001").append(dt.green).append("\002.iso")
                  .append("\001").append(dt.blue).append("\002 entries and import them into the ")
                  .append("\001").append(dt.green).append("\002local")
                  .append("\001").append(dt.blue).append("\002 database, ? for help:\n")
                  .append("\001").append(dt.reset).append("\002");

            char* rawSearchQuery = readline(prompt.c_str());
            if (!rawSearchQuery) return;

            std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
            std::string input = trimWhitespace(searchQuery.get());

            if (input == "\x1b") return;

            if (input == "?") {
                bool isCpMv = false, import2ISO = true;
                helpSearches(isCpMv, import2ISO);
                continue;
            }

            if (input == "*stats" || input == "!clr" || input == "!clr_paths" || input == "!clr_filter") {
                databaseSwitches(input);
                continue;
            }

            if (!input.empty()) {
                add_history(input.c_str());
                std::cout << "\n";
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

            while (std::getline(iss, path, ';')) {
                path = trimWhitespace(path);
                if (!isValidDirectory(path)) {
                    invalidPaths.insert(dt.red + path);
                    continue;
                }

                std::string normPath = normalizePath(path);
                if (uniqueNormalizedPaths.insert(normPath).second) {
                    validPaths.push_back(normPath);
                }
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
                    // To prevent unintended cancellation with premature ctrl+c
                    GlobalState::g_operationCancelled.store(false);
                    saveAndReportResultsForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, newISOFound, start_time);
                }
                continue;
            }

            {
                auto& pool = getStaticThreadPool();
                std::vector<std::future<void>> futures;
                std::mutex processMutex;
                std::mutex traverseErrorMutex;
                // To prevent unintended cancellation with premature ctrl+c
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
                    if (GlobalState::g_operationCancelled.load()) break;
                }
            }

            flushStdin();
            restoreInput();
            resetReadlinePagination();

            std::cout << "\r" << color << "Total files processed: " << totalFiles;

            if (!invalidPaths.empty() || !validPaths.empty()) {
                std::cout << "\n";
            }

            if (validPaths.empty()) {
                input = "";
                clear_history();
                std::cout << "\033[1A\033[K";
            }
            if (!validPaths.empty() && !input.empty()) {
                saveHistory(filterHistory);
                clear_history();
            }
            saveAndReportResultsForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, newISOFound, start_time);

        } catch (const std::exception& e) {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();
            std::cerr << "\n" << dt.red << "Unable to access ISO database: " << e.what() << dt.reset << std::endl;
            pressEnterToContinue();
        }
    }
}

/**
 * @brief Recursively traverses a directory to find ISO files
 *
 * This function performs a depth-first traversal of the filesystem starting at
 * the specified path, collecting all .iso files and handling errors appropriately.
 *
 * @param path The starting directory path for traversal
 * @param isoFiles Output vector to store discovered ISO file paths
 * @param uniqueErrorMessages Set to store unique error messages encountered
 * @param totalFiles Atomic counter for total files processed (for progress reporting)
 * @param traverseFilesMutex Mutex for protecting isoFiles vector access
 * @param traverseErrorsMutex Mutex for protecting error messages set access
 * @param maxDepth Maximum recursion depth (-1 for unlimited)
 * @param promptFlag If true, displays progress updates
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

//=============================================================================
// Image Section (BIN/IMG/CHD/DAA/GBI/MDF/NRG)
//=============================================================================

/**
 * @brief Prepares and optionally displays the contents of a disc image RAM cache.
 *
 * Copies the active cache (BIN/IMG, MDF, NRG, CHD, or DAA/GBI) into the output vector
 * when listing is enabled. If the cache is empty, prints a status message
 * and waits for user input.
 *
 * Also performs terminal state handling (signal ignoring, input control,
 * and scroll buffer clearing).
 *
 * @param files Output vector populated with cached file paths when available
 * @param list Enables display/listing mode and user interaction behavior
 * @param fileExtension Display-only string used in status messages
 * @param binImgFilesCache Snapshot of BIN/IMG cache
 * @param mdfMdsFilesCache Snapshot of MDF cache
 * @param nrgFilesCache Snapshot of NRG cache
 * @param chdFilesCache Snapshot of CHD cache
 * @param daaGbiFilesCache Snapshot of DAA/GBI cache
 * @param modeMdf Select MDF cache mode
 * @param modeNrg Select NRG cache mode
 * @param modeChd Select CHD cache mode
 * @param modeDaa Select DAA/GBI cache mode
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
        std::cout << "\n" << dt.yellow << "No " << fileExtension << " entries stored in RAM.\033[J"
                  << dt.reset << "\n";

        pressEnterToContinue();

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
 * @brief Clears the active disc image file cache and related transformation entries.
 *
 * Clears one cache at a time based on the selected mode:
 * - CHD
 * - DAA/GBI
 * - MDF
 * - NRG
 * - BIN/IMG (default)
 *
 *
 * Additionally performs terminal/UI state handling (signal reset, input control)
 * and prints a status message to the user.
 *
 * @param modeMdf Select MDF cache mode (mutually exclusive)
 * @param modeNrg Select NRG cache mode (mutually exclusive)
 * @param modeChd Select CHD cache mode (mutually exclusive)
 * @param modeDaa Select DAA/GBI cache mode (mutually exclusive)
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

    if (cacheIsEmpty) {
        std::cout << "\n" << dt.yellow << cacheType << " buffer is empty. Nothing to clear.\033[J"
                  << dt.reset << "\n";
    } else {
        std::cout << "\n" << dt.green << cacheType << " buffer cleared.\033[J"
                  << dt.reset << "\n";
    }

    pressEnterToContinue();
    clearScrollBuffer();
}

void toLowerInPlace(std::string& str);

/**
 * @brief Filters filesystem entries based on disc image mode.
 *
 * Acts as a mode-based extension filter that allows only one category of disc image
 * files at a time:
 * - BIN/IMG (default)
 * - MDF
 * - NRG
 * - CHD
 * - DAA/GBI
 *
 * Optionally supports keyword-based exclusion (currently unused).
 *
 * @return true if the file matches the active mode filter, false otherwise.
 */
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg, const bool& blacklistChd, const bool& blacklistDaa) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();
    std::string extLower = ext;
    toLowerInPlace(extLower);

    // Determine which extension(s) are allowed
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

    // Optional keyword blacklisting (currently empty)
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
 * @brief Recursively scans a single directory for disc image files as part of a parallel search system.
 *
 * Traverses the directory tree and filters files based on the specified mode
 * (BIN/IMG, MDF, NRG, CHD, or DAA/GBI). Applies cache checks and blacklist filtering
 * before invoking a callback for newly discovered files.
 *
 * Supports cancellation handling, progress reporting, and error aggregation
 * for the parent search system.
 *
 * @return Set of newly discovered file paths from this directory.
 */
std::unordered_set<std::string> processPaths(const std::string& path, const std::string& mode,
                                            const std::function<void(const std::string&, const std::string&)>& callback,
                                            std::unordered_set<std::string>& processedErrorsFind) {
    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> localFileNames;
    std::atomic<bool> g_CancelledMessageAdded{false};

    GlobalState::g_operationCancelled.store(false);
    disableInput();

    try {
        bool blacklistMdf = (mode == "mdf");
        bool blacklistNrg = (mode == "nrg");
        bool blacklistChd = (mode == "chd");
        bool blacklistDaa = (mode == "daa");

        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (GlobalState::g_operationCancelled.load()) {
                if (GlobalState::g_operationCancelled.load()) {
					std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);

					// Re-check under the lock to prevent races between threads
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
			}

                if (entry.is_regular_file()) {
                    uint64_t val = totalFiles.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (val % 100 == 0) {
                        std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
                        std::cout << "\r" << color << "Total files processed: " << val << std::flush;
                }

                if (blacklist(entry, blacklistMdf, blacklistNrg, blacklistChd, blacklistDaa)) {
                    std::string fileName = entry.path().string();
                    {
                        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);

                        bool isInCache = false;
                        if (mode == "nrg") {
                            isInCache = (std::find(GlobalState::nrgFilesCache.begin(), GlobalState::nrgFilesCache.end(), fileName) != GlobalState::nrgFilesCache.end());
                        } else if (mode == "mdf") {
                            isInCache = (std::find(GlobalState::mdfMdsFilesCache.begin(), GlobalState::mdfMdsFilesCache.end(), fileName) != GlobalState::mdfMdsFilesCache.end());
                        } else if (mode == "bin") {
                            isInCache = (std::find(GlobalState::binImgFilesCache.begin(), GlobalState::binImgFilesCache.end(), fileName) != GlobalState::binImgFilesCache.end());
                        } else if (mode == "chd") {
                            isInCache = (std::find(GlobalState::chdFilesCache.begin(), GlobalState::chdFilesCache.end(), fileName) != GlobalState::chdFilesCache.end());
                        } else if (mode == "daa") {
                            isInCache = (std::find(GlobalState::daaGbiFilesCache.begin(), GlobalState::daaGbiFilesCache.end(), fileName) != GlobalState::daaGbiFilesCache.end());
                        }

                        if (!isInCache && localFileNames.insert(fileName).second) {
                            callback(fileName, entry.path().parent_path().string());
                        }
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);

        processedErrorsFind.insert(dt.red + "Error traversing path: " + path + " - " +
                                 e.what() + dt.reset);
    }

    {
        std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
        std::cout << "\r" << color << "Total files processed: " << totalFiles << dt.reset;
    }

    return localFileNames;
}

/**
 * @brief Discovers disc image files across multiple directories using a thread pool.
 *
 * Normalizes input paths to resolve trailing slashes and redundant components,
 * deduplicates semantically identical paths, and filters out subdirectories that
 * are already covered by a parent path. This ensures each directory tree is
 * scanned exactly once.
 *
 * Submits the filtered directory scan tasks to a shared static thread pool, where
 * each task processes a directory recursively and returns discovered
 * BIN/IMG/MDF/NRG/CHD/DAA/GBI files.
 *
 * Results are aggregated, deduplicated against the existing cache, and merged
 * into the appropriate RAM cache. Supports cancellation via the global operation
 * flag and signal handling.
 *
 * @param inputPaths          Vector of raw user-provided directory paths.
 * @param fileNames           Output set to populate with discovered file paths.
 * @param currentCacheOld     Returns the cache size before new additions.
 * @param mode                File type to search for ("bin", "mdf", "nrg", "chd", "daa").
 * @param callback            Optional callback invoked for each discovered file.
 * @param directoryPaths      (Unused) Reserved for future use.
 * @param invalidDirectoryPaths Set of paths that failed validation (for verbose output).
 * @param processedErrorsFind Set of error messages encountered during traversal.
 *
 * @return Reference to the updated cache vector containing all known files of the given mode.
 */
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths,
                                   std::unordered_set<std::string>& fileNames,
                                   int& currentCacheOld, const std::string& mode,
                                   const std::function<void(const std::string&, const std::string&)>& callback,
                                   const std::vector<std::string>& directoryPaths,
                                   std::unordered_set<std::string>& invalidDirectoryPaths,
                                   std::unordered_set<std::string>& processedErrorsFind) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    disableInput();

    std::vector<std::string>* currentCache = nullptr;
    if (mode == "bin") {
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

    // ----- Normalization and deduplication -----
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

    // ----- Subpath filtering (O(n log n)) -----
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

    // ----- Submit filtered paths to thread pool -----
    std::vector<std::future<std::unordered_set<std::string>>> threadFutures;

    {
        auto& pool = getStaticThreadPool();

        for (const auto& path : filteredPaths) {
            // Explicitly isolate a string copy for this thread task
            std::string pathCopy = path;

            // Move that copy into the lambda's storage block
            threadFutures.push_back(pool.enqueue([path = std::move(pathCopy), &mode, &callback, &processedErrorsFind]() -> std::unordered_set<std::string> {
                return processPaths(path, mode, callback, std::ref(processedErrorsFind));
            }));
        }

        for (auto& future : threadFutures) {
            if (future.valid()) {
                std::unordered_set<std::string> threadResult = future.get();
                fileNames.insert(threadResult.begin(), threadResult.end());
            }
            if (GlobalState::g_operationCancelled.load()) break;
        }
    }

    verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);

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
 * @brief Dispatches special command inputs during BIN/IMG/MDF/NRG/CHD/DAA/GBI search UI interaction.
 *
 * Acts as a command router for the search interface, handling cache operations and help output.
 *
 * Some commands trigger downstream actions such as cache listing and file selection workflows.
 *
 * @return true if the input was handled as a special command, otherwise false.
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
        helpSearches(isCpMv, import2ISO);
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
 * @brief Interactive search and caching controller for disc image files.
 *
 * Provides a terminal-based interface for scanning user-provided directories
 * for BIN/IMG, MDF, NRG, CHD, and DAA/GBI image files. Supports multi-path input,
 * directory validation, history management, and special command dispatch.
 *
 * Discovered files are cached in memory and forwarded to the file selection
 * and conversion workflow when available.
 */
void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::shared_ptr<RefreshState> state) {
    struct KeybindingGuard {
        ~KeybindingGuard() {
            clear_history();
            rl_bind_key('\t', prevent_readline_keybindings);
            reset_custom_keybindingsForSearches();
        }
    } guard;

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
        resetReadlinePagination();
        rl_attempted_completion_function = my_special_completion_entry;
        setup_custom_keybindingsForSearches();
        GlobalState::g_operationCancelled.store(false);
        clearScrollBuffer();
        clear_history();
        bool filterHistory = false;
        loadHistory(filterHistory);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_key('\t', my_rl_complete);
    };

    while (true) {
        int currentCacheOld = 0;
        std::vector<std::string> directoryPaths;
        std::unordered_set<std::string> uniquePaths, processedErrors, processedErrorsFind;
        std::unordered_set<std::string> invalidDirectoryPaths, fileNames;
        bool filterHistory = false;

        initIterationState();

        const VerboseAndDatabaseTheme dt = getDatabaseTheme();

        std::string prompt;
        prompt.reserve(512);
        prompt.append("\001").append(dt.green).append("\002FolderPaths")
              .append("\001").append(dt.blue).append("\002 ↵ to scan for \001")
              .append("\001").append(dt.orange).append("\002").append(fileExtension)
              .append("\001").append(dt.blue).append("\002 entries and cache them in \001")
              .append("\001").append(dt.yellow).append("\002RAM\001")
              .append("\001").append(dt.blue).append("\002, ? for help:\n\001")
              .append(dt.reset).append("\002");

        char* rawSearchQuery = readline(prompt.c_str());
        if (!rawSearchQuery) return;

        std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);

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
        while (std::getline(ss, path, ';')) {
            if (!path.empty() && uniquePaths.insert(path).second) {
                if (isValidDirectory(path)) {
                    directoryPaths.push_back(path);
                } else {
                    invalidDirectoryPaths.insert(dt.red + path);
                }
            }
        }

        bool newFilesFound = false;
        const auto start_time = std::chrono::high_resolution_clock::now();

        files = findFiles(directoryPaths, fileNames, currentCacheOld, fileType,
                          [&](const std::string&, const std::string&) { newFilesFound = true; },
                          directoryPaths, invalidDirectoryPaths, processedErrorsFind);

        try {
            if (!directoryPaths.empty()) {
                add_history(inputSearch.c_str());
                saveHistory(filterHistory);
            }
        } catch (const std::exception& e) {
            std::cerr << "\n\n" << dt.red << "Unable to access local database: " << e.what()
                      << dt.reset << std::endl;
        }

        verboseImageSearchResults(fileExtension, fileNames, invalidDirectoryPaths,
                             newFilesFound, list, currentCacheOld, files,
                             start_time, processedErrorsFind, directoryPaths);

        if (!newFilesFound) continue;

        if (!files.empty() && !GlobalState::g_operationCancelled.load())
            selectForImageFiles(fileType, files, list, state);
    }
}
