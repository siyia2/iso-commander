// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../themes.h"
#include "../readline.h"

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
 * @brief Performs interactive or non-interactive ISO database refresh
 * 
 * This function scans directories for ISO files and updates the local database.
 * It supports both interactive mode (with user prompts) and non-interactive mode.
 * 
 * @param initialDir Initial directory path to scan (if empty, prompts user)
 * @param maxDepth Maximum directory depth to traverse (-1 for unlimited)
 * @param filterHistory Whether to filter command history
 * @param newISOFound Atomic flag set to true if new ISO files were discovered
 */
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound) {
    try {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        resetReadlinePagination();
        rl_attempted_completion_function = my_special_completion_entry;
        g_operationCancelled.store(false);
        
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        clearScrollBuffer();
        loadHistory(filterHistory);

        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_key('\t', rl_complete);
        
        std::string_view primary = isOrig ? originalColors::green : theme->accent;
        std::string_view secondary = isOrig ? originalColors::blue : theme->muted;

        std::string prompt;
        prompt.reserve(512);
        prompt.append("\001").append(primary).append("\002FolderPaths")
              .append("\001").append(secondary).append("\002 ↵ to scan for ")
              .append("\001").append(primary).append("\002.iso")
              .append("\001").append(secondary).append("\002 entries and import them into the ")
              .append("\001").append(primary).append("\002local")
              .append("\001").append(secondary).append("\002 database, ? ↵ help, ↵ return:\n")
              .append("\001").append(originalColors::boldAlt).append("\002");

        char* rawSearchQuery = readline(prompt.c_str());
        if (!rawSearchQuery) return;

        std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
        std::string input = trimWhitespace(searchQuery.get());

        if (input == "?") {
            bool isCpMv = false, import2ISO = true;
            helpSearches(isCpMv, import2ISO);
            return refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
        }

        if (input.starts_with("*") || input.starts_with("?") || input.starts_with("!") || isValidInput(input)) {
            databaseSwitches(input, promptFlag, maxDepth, filterHistory, newISOFound);
            return;
        }

        if (!input.empty()) {
            add_history(input.c_str());
            std::cout << "\n";
        }

        if (input.find_first_not_of(" \t\n\r") == std::string::npos) return;

        std::unordered_set<std::string> uniquePaths;
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

        while (std::getline(iss, path, ';')) {
            if (!isValidDirectory(path)) {
				std::string errCol = isOrig ? std::string(originalColors::red) : std::string(theme->secondary);
				invalidPaths.insert(errCol + path);
                continue;
            }

            if (uniquePaths.insert(path).second) {
                validPaths.push_back(path);
            }
        }
        
        if (validPaths.empty()) {
			flushStdin();
			restoreInput();
			resetReadlinePagination();
			if (!invalidPaths.empty()) {
				verboseForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newISOFound);
			}
            return;
        }
        
        {
            auto& pool = getStaticThreadPool();
            std::vector<std::future<void>> futures;
            std::mutex processMutex;
            std::mutex traverseErrorMutex;

            for (const auto& validPath : validPaths) {
                futures.emplace_back(
                    pool.enqueue([validPath, &allIsoFiles, &uniqueErrorMessages, &totalFiles, 
                                  &processMutex, &traverseErrorMutex, &maxDepth, &promptFlag]() {
                        traverse(validPath, allIsoFiles, uniqueErrorMessages, totalFiles, 
                                processMutex, traverseErrorMutex, maxDepth, promptFlag);
                    })
                );
            }

            for (auto& future : futures) {
                future.wait();
                if (g_operationCancelled.load()) break;
            }
        }
        
        if (!g_operationCancelled.load()) {
            std::unordered_set<std::string> uniqueFiles(allIsoFiles.begin(), allIsoFiles.end());
            allIsoFiles.assign(uniqueFiles.begin(), uniqueFiles.end());
        }
        
        flushStdin();
        restoreInput();
        resetReadlinePagination();
            
        std::cout << "\r" << (isOrig ? originalColors::boldAlt : theme->accent) 
                  << "Total files processed: " << totalFiles;
        
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
        verboseForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newISOFound);
        if (!g_operationCancelled.load()) {
            saveToDatabase(allIsoFiles, newISOFound);
        }

    } catch (const std::exception& e) {
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        std::cerr << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "Unable to access ISO database: " << e.what() << originalColors::boldAlt << std::endl;
        
        std::cout << color << "\n↵ to continue..." << reset; 

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
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
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");
	
	g_operationCancelled.store(false);

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
            
            if (g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(traverseErrorsMutex);
                    uniqueErrorMessages.clear(); 
                    
                    std::string warnCol = std::string(isOriginal ? originalColors::yellow : theme->warning);
                    std::string msg = "\n" + warnCol + "ISO search interrupted by user." + std::string(originalColors::boldAlt);
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
                totalFiles.fetch_add(1, std::memory_order_acq_rel); 
                if (totalFiles % 100 == 0) { 
                    std::lock_guard<std::mutex> lock(couNtMutex); 
                    std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                              << "Total files processed: " << totalFiles << std::flush;
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
        std::string errCol = std::string(isOriginal ? originalColors::red : theme->secondary);
        std::string formattedError = "\n" + errCol + "Error: " + path.string() + " - " + 
                                     e.what() + std::string(originalColors::boldAlt);
        
        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex); 
            uniqueErrorMessages.insert(formattedError); 
        }
    }
}

//=============================================================================
// Image Section (BIN/IMG/CHD/DAA/MDF/NRG)
//=============================================================================

/**
 * @brief Prepares and optionally displays the contents of a disc image RAM cache.
 *
 * Copies the active cache (BIN/IMG, MDF, NRG, CHD, or DAA) into the output vector
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
 * @param daaFilesCache Snapshot of DAA cache
 * @param modeMdf Select MDF cache mode
 * @param modeNrg Select NRG cache mode
 * @param modeChd Select CHD cache mode
 * @param modeDaa Select DAA cache mode
 */
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension, 
                  const std::vector<std::string>& binImgFilesCache, 
                  const std::vector<std::string>& mdfMdsFilesCache, 
                  const std::vector<std::string>& nrgFilesCache,
                  const std::vector<std::string>& chdFilesCache,
                  const std::vector<std::string>& daaFilesCache,   // <-- added
                  bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa) {   // <-- added modeDaa
    
    signal(SIGINT, SIG_IGN);        
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    bool isEmpty = false;
    if (modeDaa) {
        isEmpty = daaFilesCache.empty();
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
        std::cout << "\n" << (isOriginal ? originalColors::yellow : theme->warning) 
                  << "No " << fileExtension << " entries stored in RAM.\033[J" 
                  << originalColors::boldAlt << "\n";

        std::cout << color << "\n↵ to continue..." << reset; 
        
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        files.clear();
        
        clearScrollBuffer();
        return;
        
    } else if (list) {
        if (modeDaa) {
            files = daaFilesCache;
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
 * - DAA
 * - MDF
 * - NRG
 * - BIN/IMG (default)
 *
 * Also removes matching entries from the transformation cache based on file extension.
 *
 * Additionally performs terminal/UI state handling (signal reset, input control)
 * and prints a status message to the user.
 *
 * @param modeMdf Select MDF cache mode (mutually exclusive)
 * @param modeNrg Select NRG cache mode (mutually exclusive)
 * @param modeChd Select CHD cache mode (mutually exclusive)
 * @param modeDaa Select DAA cache mode (mutually exclusive)
 */
void clearRamCache(bool& modeMdf, bool& modeNrg, bool& modeChd, bool& modeDaa) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::vector<std::string> extensions;
    std::string cacheType;
    bool cacheIsEmpty = false;

    if (modeDaa) {
        extensions = {".daa"};
        cacheType = "DAA";
        cacheIsEmpty = daaFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(daaFilesCache);
    } else if (modeChd) {
        extensions = {".chd"};
        cacheType = "CHD";
        cacheIsEmpty = chdFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(chdFilesCache);
    } else if (modeMdf) {
        extensions = {".mdf"};
        cacheType = "MDF";
        cacheIsEmpty = mdfMdsFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(mdfMdsFilesCache);
    } else if (modeNrg) {
        extensions = {".nrg"};
        cacheType = "NRG";
        cacheIsEmpty = nrgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(nrgFilesCache);
    } else {
        extensions = {".bin", ".img"};
        cacheType = "BIN/IMG";
        cacheIsEmpty = binImgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(binImgFilesCache);
    }

    bool transformationCacheWasCleared = false;
    
    for (auto it = transformationCache.begin(); it != transformationCache.end();) {
        std::string keyLower = it->first;
        toLowerInPlace(keyLower);

        bool shouldErase = std::any_of(extensions.begin(), extensions.end(),
            [&keyLower](std::string ext) {
                toLowerInPlace(ext);
                return keyLower.size() >= ext.size() &&
                       keyLower.compare(keyLower.size() - ext.size(), ext.size(), ext) == 0;
            });

        if (shouldErase) {
            it = transformationCache.erase(it);
            transformationCacheWasCleared = true;
        } else {
            ++it;
        }
    }

    if (cacheIsEmpty && !transformationCacheWasCleared) {
        std::cout << "\n" << (isOriginal ? originalColors::yellow : theme->warning) 
                  << cacheType << " buffer is empty. Nothing to clear.\033[J" 
                  << originalColors::boldAlt << "\n";
    } else {
        std::cout << "\n" << (isOriginal ? originalColors::green : theme->accent) 
                  << cacheType << " buffer cleared.\033[J" 
                  << originalColors::boldAlt << "\n";
    }

    std::cout << color << "\n↵ to continue..." << reset; 

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}

/**
 * @brief Filters filesystem entries based on disc image mode.
 *
 * Acts as a mode-based extension filter that allows only one category of disc image
 * files at a time:
 * - BIN/IMG (default)
 * - MDF
 * - NRG
 * - CHD
 * - DAA
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
        if (extLower != ".daa") {
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
 * (BIN/IMG, MDF, NRG, CHD, or DAA). Applies cache checks and blacklist filtering
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
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> localFileNames;
    std::atomic<bool> g_CancelledMessageAdded{false};
    
    g_operationCancelled.store(false);
    disableInput();
    
    try {
        bool blacklistMdf = (mode == "mdf");
        bool blacklistNrg = (mode == "nrg");
        bool blacklistChd = (mode == "chd");
        bool blacklistDaa = (mode == "daa");   // <-- DAA mode flag
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(globalSetsMutex);
                    processedErrorsFind.clear();
                    localFileNames.clear();
                    
                    // Determine the file type string for the cancellation message
                    std::string type = (blacklistMdf) ? "MDF" : 
                                       (blacklistNrg) ? "NRG" : 
                                       (blacklistChd) ? "CHD" : 
                                       (blacklistDaa) ? "DAA" : "BIN/IMG";
                    
                    std::string warnCol = std::string(isOriginal ? originalColors::yellow : theme->warning);
                    
                    processedErrorsFind.insert(warnCol + type + " search interrupted by user.\n\n" + 
                                             std::string(originalColors::boldAlt));
                }
                break;
            }
            
            if (entry.is_regular_file()) {
                totalFiles.fetch_add(1, std::memory_order_acq_rel);
                
                if (totalFiles % 100 == 0) {
                    std::lock_guard<std::mutex> lock(couNtMutex);
                    std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                              << "Total files processed: " << totalFiles << std::flush;
                }
                
                // Call blacklist with the new DAA flag
                if (blacklist(entry, blacklistMdf, blacklistNrg, blacklistChd, blacklistDaa)) {
                    std::string fileName = entry.path().string();
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex);
                        
                        bool isInCache = false;
                        if (mode == "nrg") {
                            isInCache = (std::find(nrgFilesCache.begin(), nrgFilesCache.end(), fileName) != nrgFilesCache.end());
                        } else if (mode == "mdf") {
                            isInCache = (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) != mdfMdsFilesCache.end());
                        } else if (mode == "bin") {
                            isInCache = (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) != binImgFilesCache.end());
                        } else if (mode == "chd") {
                            isInCache = (std::find(chdFilesCache.begin(), chdFilesCache.end(), fileName) != chdFilesCache.end());
                        } else if (mode == "daa") {   // <-- DAA cache check
                            isInCache = (std::find(daaFilesCache.begin(), daaFilesCache.end(), fileName) != daaFilesCache.end());
                        }
                        
                        if (!isInCache && localFileNames.insert(fileName).second) {
                            callback(fileName, entry.path().parent_path().string());
                        }
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        std::string errCol = std::string(isOriginal ? originalColors::red : theme->secondary);
        
        processedErrorsFind.insert(errCol + "Error traversing path: " + path + " - " + 
                                 e.what() + std::string(originalColors::boldAlt));
    }
    
    {
        std::lock_guard<std::mutex> lock(couNtMutex);
        std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                  << "Total files processed: " << totalFiles << originalColors::boldAlt;
    }
    
    return localFileNames;
}

/**
 * @brief Discovers disc image files across multiple directories using a thread pool.
 *
 * Submits directory scan tasks to a shared static thread pool, where each task
 * processes a directory and returns discovered BIN/IMG/MDF/NRG/CHD/DAA files.
 *
 * Results are aggregated, deduplicated, and merged into the appropriate RAM cache.
 * Supports cancellation via global operation flag and signal handling.
 *
 * @return Updated cache containing previously known and newly discovered files.
 */
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths, std::unordered_set<std::string>& fileNames, int& currentCacheOld, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& invalidDirectoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);
    
    disableInput();
    
    std::vector<std::string>* currentCache = nullptr;
    if (mode == "bin") {
        currentCacheOld = binImgFilesCache.size();
        currentCache = &binImgFilesCache;
    } else if (mode == "mdf") {
        currentCacheOld = mdfMdsFilesCache.size();
        currentCache = &mdfMdsFilesCache;
    } else if (mode == "nrg") {
        currentCacheOld = nrgFilesCache.size();
        currentCache = &nrgFilesCache;
    } else if (mode == "chd") {
        currentCacheOld = chdFilesCache.size();
        currentCache = &chdFilesCache;
    } else if (mode == "daa") {        // <-- DAA branch
        currentCacheOld = daaFilesCache.size();
        currentCache = &daaFilesCache;
    } else {
        restoreInput();
        return {};
    }
    
    std::vector<std::future<std::unordered_set<std::string>>> threadFutures;
    std::unordered_set<std::string> processedValidPaths;
    std::vector<std::string> uniquePaths;
    
    for (const auto& originalPath : inputPaths) {
        std::string path = std::filesystem::path(originalPath).string();
        if (path.empty() || !processedValidPaths.insert(path).second) {
            continue;
        }
        uniquePaths.push_back(path);
    }

    if (uniquePaths.empty()) {
        flushStdin();
        restoreInput();
        return *currentCache;
    }
    
    {
        //Use the static pool
        auto& pool = getStaticThreadPool();
    
        for (const auto& path : uniquePaths) {
            threadFutures.push_back(pool.enqueue([path, &mode, &callback, &processedErrorsFind]() -> std::unordered_set<std::string> {
                return processPaths(path, mode, callback, std::ref(processedErrorsFind));
            }));
        }
    
        for (auto& future : threadFutures) {
            if (future.valid()) {
                std::unordered_set<std::string> threadResult = future.get();
                fileNames.insert(threadResult.begin(), threadResult.end());
            }
            // If the user cancelled via the signal handler, we can stop waiting
            if (g_operationCancelled.load()) break;
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

/**
 * @brief Dispatches special command inputs during BIN/IMG/MDF/NRG/CHD/DAA search UI interaction.
 *
 * Acts as a command router for the search interface, handling configuration changes,
 * UI settings, pagination control, cache operations, help output, and display mode switching.
 *
 * Some commands trigger downstream actions such as cache listing and file selection workflows.
 *
 * @return true if the input was handled as a special command, otherwise false.
 */
bool dispatchSpecialCommandForBinImgMdfNrgSearch(const std::string& input, const std::string& configPath,
                                                  bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa,
                                                  const std::string& fileExtension,
                                                  std::vector<std::string>& files, const std::string& fileType,
                                                  std::atomic<bool>& newISOFound, bool& list,
                                                  std::atomic<bool>& isImportRunning) {
    
    if (input == "?stats") {
        displayDatabaseStatistics(databaseFilePath, maxDatabaseSize, transformationCache, globalIsoFileList);
        return true;
    }
    if (input == "?config") {
        displayConfigurationOptions(configPath);
        return true;
    }
    if (input.starts_with("*pagination:")) {
        updatePagination(input, configPath);
        return true;
    }
    if (input == "*flno:on" || input == "*flno:off") {
        updateFilenamesOnly(configPath, input);
        return true;
    }
    if (input.starts_with("*skin:")) {
        updateUIAppearance(configPath, input);
        return true;
    }
    if (input.starts_with("*theme:")) {
        updateUIAppearance(configPath, input);
        return true;
    }
    if (input == "!clr_paths" || input == "!clr_filter") {
        clearHistory(input);
        return true;
    }
    if (isValidInput(input)) {
        setDisplayMode(input);
        return true;
    }
    if (input == "?") {
        bool isCpMv = false, import2ISO = false;
        helpSearches(isCpMv, import2ISO);
        return true;
    }
    if (input == "!clr") {
        // Now passing modeDaa to clear the DAA cache as well
        clearRamCache(modeMdf, modeNrg, modeChd, modeDaa);
        return true;
    }
    if (input == "ls") {
        list = true;
        // Pass daaFilesCache and modeDaa to ramCacheList
        ramCacheList(files, list, fileExtension,
                     binImgFilesCache, mdfMdsFilesCache, nrgFilesCache, chdFilesCache, daaFilesCache,
                     modeMdf, modeNrg, modeChd, modeDaa);
        if (!files.empty())
            selectForImageFiles(fileType, files, newISOFound, list, isImportRunning);
        return true;
    }
    return false;
}

/**
 * @brief Interactive search and caching controller for disc image files.
 *
 * Provides a terminal-based interface for scanning user-provided directories
 * for BIN/IMG, MDF, NRG, CHD, and DAA image files. Supports multi-path input,
 * directory validation, history management, and special command dispatch.
 *
 * Discovered files are cached in memory and forwarded to the file selection
 * and conversion workflow when available.
 */
void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning) {
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
        {"daa", {".daa",      "DAA"    }}   // <-- DAA entry
    };

    const auto configIt = fileTypeMap.find(fileTypeChoice);
    if (configIt == fileTypeMap.end()) {
        std::cout << originalColors::red << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG, CHD, DAA" << originalColors::boldAlt << "\n";
        return;
    }

    const std::string& fileType      = fileTypeChoice;
    const std::string& fileExtension = configIt->second.extension;
    const bool modeMdf = (fileType == "mdf");
    const bool modeNrg = (fileType == "nrg");
    const bool modeChd = (fileType == "chd");
    const bool modeDaa = (fileType == "daa");   // <-- DAA mode flag

    std::vector<std::string> files;
    files.reserve(100);
    binImgFilesCache.reserve(100);
    mdfMdsFilesCache.reserve(100);
    nrgFilesCache.reserve(100);
    chdFilesCache.reserve(100);
    daaFilesCache.reserve(100);   // <-- reserve for DAA cache

    auto initIterationState = [&]() {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        resetReadlinePagination();
        rl_attempted_completion_function = my_special_completion_entry;
        g_operationCancelled.store(false);
        clearScrollBuffer();
        clear_history();
        bool filterHistory = false;
        loadHistory(filterHistory);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_key('\t', rl_complete);
    };

    auto isBlankInput = [](const char* s) {
        return !s || s[0] == '\0' || std::all_of(s, s + strlen(s), [](char c){ return c == ' '; });
    };

    while (true) {
        int currentCacheOld = 0;
        std::vector<std::string> directoryPaths;
        std::unordered_set<std::string> uniquePaths, processedErrors, processedErrorsFind;
        std::unordered_set<std::string> successOuts, skippedOuts, failedOuts, invalidDirectoryPaths, fileNames;
        bool filterHistory = false;

        initIterationState();
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        
        const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");

        std::string_view headCol = isOriginal ? originalColors::green  : theme->accent;
        std::string_view textCol = isOriginal ? originalColors::blue   : theme->muted;
        std::string_view extCol  = isOriginal ? originalColors::orange : theme->accent;
        std::string_view ramCol  = isOriginal ? originalColors::yellow : theme->accent;

        std::string prompt;
        prompt.reserve(512);
        prompt.append("\001").append(headCol).append("\002FolderPaths")
              .append("\001").append(textCol).append("\002 ↵ to scan for \001")
              .append("\001").append(extCol).append("\002").append(fileExtension)
              .append("\001").append(textCol).append("\002 entries and cache them into \001")
              .append("\001").append(ramCol).append("\002RAM\001")
              .append("\001").append(textCol).append("\002, ? ↵ help, ↵ return:\n\001")
              .append(originalColors::boldAlt).append("\002");
        
        std::unique_ptr<char, decltype(&std::free)> mainSearch(readline(prompt.c_str()), &std::free);

        if (isBlankInput(mainSearch.get())) break;

        const std::string inputSearch = trimWhitespace(mainSearch.get());

        bool list = false;
        // Update the helper to accept modeDaa (you'll need to modify its signature accordingly)
        if (dispatchSpecialCommandForBinImgMdfNrgSearch(inputSearch, configPath, modeMdf, modeNrg, modeChd, modeDaa,
                                   fileExtension, files, fileType,
                                   newISOFound, list, isImportRunning))
            continue;

        std::cout << " \n\033[3H\033[J\n";

        std::istringstream ss(inputSearch);
        std::string path;
        while (std::getline(ss, path, ';')) {
            if (!path.empty() && uniquePaths.insert(path).second) {
                if (isValidDirectory(path)) {
                    directoryPaths.push_back(path);
                } else {
                    std::string errPrefix = isOriginal ? std::string(originalColors::red) : std::string(theme->secondary);
                    invalidDirectoryPaths.insert(errPrefix + path);
                }
            }
        }

        bool newFilesFound = false;
        const auto start_time = std::chrono::high_resolution_clock::now();

        // The findFiles function must be extended to handle DAA (scan for .daa files)
        files = findFiles(directoryPaths, fileNames, currentCacheOld, fileType,
                          [&](const std::string&, const std::string&) { newFilesFound = true; },
                          directoryPaths, invalidDirectoryPaths, processedErrorsFind);

        try {
            if (!directoryPaths.empty()) {
                add_history(inputSearch.c_str());
                saveHistory(filterHistory);
            }
        } catch (const std::exception& e) {
            std::cerr << "\n\n" << (isOriginal ? originalColors::red : theme->secondary) 
                      << "Unable to access local database: " << e.what() 
                      << originalColors::boldAlt << std::endl;
        }

        verboseSearchResults(fileExtension, fileNames, invalidDirectoryPaths,
                             newFilesFound, list, currentCacheOld, files,
                             start_time, processedErrorsFind, directoryPaths);

        if (!newFilesFound) continue;

        if (!files.empty() && !g_operationCancelled.load())
            selectForImageFiles(fileType, files, newISOFound, list, isImportRunning);
    }
}
