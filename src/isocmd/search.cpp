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
 * @param promptFlag If true, runs in interactive mode with user prompts
 * @param maxDepth Maximum directory depth to traverse (-1 for unlimited)
 * @param filterHistory Whether to filter command history
 * @param newISOFound Atomic flag set to true if new ISO files were discovered
 */
void refreshForDatabase(std::string& initialDir, bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound) {
    try {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        resetReadlinePagination();
        rl_attempted_completion_function = my_special_completion_entry;
        g_operationCancelled.store(false);
        
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        std::string input = initialDir;
        if (input.empty()) {
            if (promptFlag) clearScrollBuffer();
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
                  .append("\001").append(secondary).append("\002 entries and import into ")
                  .append("\001").append(primary).append("\002local")
                  .append("\001").append(secondary).append("\002 database, ? ↵ help, ↵ return:\n")
                  .append("\001").append(originalColors::boldAlt).append("\002");

            char* rawSearchQuery = readline(prompt.c_str());
            if (!rawSearchQuery) {
                input.clear();
            } else {
                std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
                input = trimWhitespace(searchQuery.get());
                
                if (input == "?") {
                    bool isCpMv = false, import2ISO = true;
                    helpSearches(isCpMv, import2ISO);
                    std::string dummy = "";
                    return refreshForDatabase(dummy, promptFlag, maxDepth, filterHistory, newISOFound);
                }
                
                if (input.starts_with("*") || input.starts_with("?") || input.starts_with("!") || isValidInput(input)) {
                    databaseSwitches(input, promptFlag, maxDepth, filterHistory, newISOFound);
                    return;
                }
                
                if (!input.empty() && promptFlag) {
                    add_history(input.c_str());
                    std::cout << "\n";
                }
            }
        }

        if (input.find_first_not_of(" \t\n\r") == std::string::npos) return;
        
        std::unordered_set<std::string> uniquePaths;
        std::vector<std::string> validPaths;
        std::unordered_set<std::string> invalidPaths;
        std::unordered_set<std::string> uniqueErrorMessages;
        std::vector<std::string> allIsoFiles;
        std::atomic<size_t> totalFiles{0};

        if (promptFlag) {
            std::cout << "\033[3H\033[J\n";
            disableInput();
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        std::istringstream iss(input);
        std::string path;

        while (std::getline(iss, path, ';')) {
            if (!isValidDirectory(path)) {
                if (promptFlag) {
                    // Colorize the invalid path with the theme's secondary (error) color
                    std::string errCol = isOrig ? std::string(originalColors::red) : std::string(theme->secondary);
                    invalidPaths.insert(errCol + path);
                }
                continue;
            }

            if (uniquePaths.insert(path).second) {
                validPaths.push_back(path);
            }
        }
        
        if (validPaths.empty()) {
            if (promptFlag) {
                flushStdin();
                restoreInput();
                resetReadlinePagination();
                if (!invalidPaths.empty()) {
                    verboseForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newISOFound);
                }
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
        
        if (promptFlag) {
            flushStdin();
            restoreInput();
            resetReadlinePagination();
                
            // Use theme accent for the final progress count
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
        } else {
            if (!g_operationCancelled.load()) {
                saveToDatabase(allIsoFiles, newISOFound);
            }
        }
    } catch (const std::exception& e) {
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        std::cerr << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "Unable to access ISO database: " << e.what() << originalColors::boldAlt << std::endl;
        
        std::cout << color << "\n↵ to continue..." << reset; 

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::string dummyDir = "";
        refreshForDatabase(dummyDir, promptFlag, maxDepth, filterHistory, newISOFound);
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
// Image Section (BIN/IMG/MDF/NRG)
//=============================================================================

/**
 * @brief Lists the contents of RAM cache for disc image files
 * 
 * Displays all files currently stored in the specified RAM cache (BIN/IMG, MDF, or NRG)
 * and provides an interactive selection interface.
 * 
 * @param files Output vector to populate with cached file paths
 * @param list Flag indicating whether to list the cache contents
 * @param fileExtension File extension string for display purposes
 * @param binImgFilesCache Reference to BIN/IMG cache vector
 * @param mdfMdsFilesCache Reference to MDF cache vector
 * @param nrgFilesCache Reference to NRG cache vector
 * @param modeMdf If true, operate on MDF cache mode
 * @param modeNrg If true, operate on NRG cache mode
 */
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension, 
                  const std::vector<std::string>& binImgFilesCache, 
                  const std::vector<std::string>& mdfMdsFilesCache, 
                  const std::vector<std::string>& nrgFilesCache, bool modeMdf, bool modeNrg) {
    
    signal(SIGINT, SIG_IGN);        
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    bool isEmpty = false;
    if (!modeMdf && !modeNrg) isEmpty = binImgFilesCache.empty();
    else if (modeMdf)         isEmpty = mdfMdsFilesCache.empty();
    else if (modeNrg)         isEmpty = nrgFilesCache.empty();

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
        if (!modeMdf && !modeNrg) {
            files = binImgFilesCache;
        } 
        else if (modeMdf) {
            files = mdfMdsFilesCache;
        } 
        else if (modeNrg) {
            files = nrgFilesCache;
        }
    }
}

/**
 * @brief Clears the RAM cache for disc image files
 * 
 * Removes all entries from the specified RAM cache (BIN/IMG, MDF, or NRG) and
 * also clears associated transformation cache entries.
 * 
 * @param modeMdf If true, clear MDF cache
 * @param modeNrg If true, clear NRG cache
 */
void clearRamCache(bool& modeMdf, bool& modeNrg) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::vector<std::string> extensions;
    std::string cacheType;
    bool cacheIsEmpty = false;

    if (!modeMdf && !modeNrg) {
        extensions = {".bin", ".img"};
        cacheType = "BIN/IMG";
        cacheIsEmpty = binImgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(binImgFilesCache);
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
 * @brief Applies blacklist filtering to disc image files
 * 
 * Checks if a file should be included based on its extension and blacklisted keywords.
 * 
 * @param entry Filesystem entry to check
 * @param blacklistMdf If true, only allow .mdf files
 * @param blacklistNrg If true, only allow .nrg files
 * @return true if the file passes the blacklist filter, false otherwise
 */
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();
    std::string extLower = ext;
    toLowerInPlace(extLower);

    if (!blacklistMdf && !blacklistNrg) {
        if (!((extLower == ".bin" || extLower == ".img"))) {
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
 * @brief Processes a batch of directory paths to find disc image files
 * 
 * Traverses a single directory path and collects all matching disc image files
 * based on the specified mode (BIN/IMG, MDF, or NRG).
 * 
 * @param path Directory path to traverse
 * @param mode File type mode ("bin", "mdf", or "nrg")
 * @param callback Callback function called for each discovered file
 * @param processedErrorsFind Set to store error messages encountered
 * @return Unordered set of discovered file paths
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
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(globalSetsMutex);
                    processedErrorsFind.clear();
                    localFileNames.clear();
                    
                    std::string type = (blacklistMdf) ? "MDF" : (blacklistNrg) ? "NRG" : "BIN/IMG";
                    
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
                
                if (blacklist(entry, blacklistMdf, blacklistNrg)) {
                    std::string fileName = entry.path().string();
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex);
                        
                        bool isInCache = false;
                        if (mode == "nrg") isInCache = (std::find(nrgFilesCache.begin(), nrgFilesCache.end(), fileName) != nrgFilesCache.end());
                        else if (mode == "mdf") isInCache = (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) != mdfMdsFilesCache.end());
                        else if (mode == "bin") isInCache = (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) != binImgFilesCache.end());
                        
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
 * @brief Finds disc image files across multiple directories using multithreading
 * 
 * Spawns one thread per unique directory path to efficiently scan for disc image files
 * and updates the appropriate RAM cache with newly discovered files.
 * 
 * @param inputPaths Vector of directory paths to scan
 * @param fileNames Set to store discovered file names
 * @param currentCacheOld Reference to store previous cache size
 * @param mode File type mode ("bin", "mdf", or "nrg")
 * @param callback Callback function called for each discovered file
 * @param directoryPaths Vector of valid directory paths (output parameter)
 * @param invalidDirectoryPaths Set of invalid directory paths (output parameter)
 * @param processedErrorsFind Set to store error messages (output parameter)
 * @return Vector of all discovered file paths (including previously cached files)
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
    
    // We no longer need to calculate numThreads or check for 0.
    // The singleton handles the safety floor and capacity for us.
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
 * @brief Dispatches special commands for BIN/IMG/MDF/NRG search operations
 * 
 * Handles various command-line commands including statistics display, configuration,
 * pagination settings, theme changes, and cache management.
 * 
 * @param input The command input string
 * @param configPath Path to configuration file
 * @param modeMdf If true, operating in MDF mode
 * @param modeNrg If true, operating in NRG mode
 * @param fileExtension File extension for display purposes
 * @param files Vector of file paths (modified by 'ls' command)
 * @param fileType Type of file being processed
 * @param newISOFound Atomic flag for new ISO discovery
 * @param list Flag indicating whether to list cache contents
 * @param isImportRunning Atomic flag indicating if import is in progress
 * @return true if a special command was handled and caller should continue, false otherwise
 */
bool dispatchSpecialCommandForBinImgMdfNrgSearch(const std::string& input, const std::string& configPath, bool modeMdf, bool modeNrg, const std::string& fileExtension, 
std::vector<std::string>& files, const std::string& fileType, std::atomic<bool>& newISOFound, bool& list, std::atomic<bool>& isImportRunning) {
    
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
        clearRamCache(modeMdf, modeNrg);
        return true;
    }
    if (input == "ls") {
        list = true;
        ramCacheList(files, list, fileExtension, binImgFilesCache, mdfMdsFilesCache, nrgFilesCache, modeMdf, modeNrg);
        if (!files.empty())
            selectForImageFiles(fileType, files, newISOFound, list, isImportRunning);
        return true;
    }
    return false;
}

/**
 * @brief Prompts user to search for disc image files (BIN/IMG/MDF/NRG)
 * 
 * Provides an interactive interface for scanning directories for disc image files,
 * caching results in RAM, and selecting files for further operations.
 * 
 * @param fileTypeChoice User's choice of file type ("bin", "img", "mdf", or "nrg")
 * @param newISOFound Atomic flag set to true if new ISO files were discovered
 * @param isImportRunning Atomic flag indicating if import is in progress
 */
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning) {
    struct FileTypeConfig {
        std::string extension;
        std::string name;
    };

    static const std::unordered_map<std::string, FileTypeConfig> fileTypeMap = {
        {"bin", {".bin/.img", "BIN/IMG"}},
        {"img", {".bin/.img", "BIN/IMG"}},
        {"mdf", {".mdf",      "MDF"    }},
        {"nrg", {".nrg",      "NRG"    }},
    };

    const auto configIt = fileTypeMap.find(fileTypeChoice);
    if (configIt == fileTypeMap.end()) {
        std::cout << originalColors::red << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG" << originalColors::boldAlt << "\n";
        return;
    }

    const std::string& fileType      = fileTypeChoice;
    const std::string& fileExtension = configIt->second.extension;
    const bool modeMdf = (fileType == "mdf");
    const bool modeNrg = (fileType == "nrg");

    std::vector<std::string> files;
    files.reserve(100);
    binImgFilesCache.reserve(100);
    mdfMdsFilesCache.reserve(100);
    nrgFilesCache.reserve(100);

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
        if (dispatchSpecialCommandForBinImgMdfNrgSearch(inputSearch, configPath, modeMdf, modeNrg,
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
