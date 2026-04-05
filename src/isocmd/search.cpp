// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../themes.h"
#include "../readline.h"


//GENERAL SECTION


// Function to check if a directory input is valid for searches
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// ISO SECTION


// Function for interactive and non-interactive ISO database refresh
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
            
            // Define thematic colors
            std::string_view primary = isOrig ? originalColors::green : theme->accent;
            std::string_view secondary = isOrig ? originalColors::blue : theme->muted;

            // Build prompt with Readline non-printing character wrappers \001 and \002
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
        
        const size_t numThreads = std::min({
            validPaths.size(),
            static_cast<size_t>(maxThreads),
            static_cast<size_t>(MAX_USEFUL_THREADS)
        });
        
        {
            ThreadPool pool(numThreads);
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
                  << "Unable to access ISO database: " << e.what() << originalColors::reset << std::endl;
        
        std::cout << color << "\n↵ to continue..." << reset; 

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::string dummyDir = "";
        refreshForDatabase(dummyDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }
}


// Function to traverse a directory and find ISO files
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
            
            // 1. Handle Interruption (Theme-aware)
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

            // 2. Progress Feedback (Theme-aware)
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

            // 3. ISO Filtering
            if (!iequals(filePath.extension().string(), ".iso")) continue;

            localIsoFiles.push_back(filePath.string());

            // 4. Batch Updates (Thread Safety)
            if (localIsoFiles.size() >= BATCH_SIZE) {
                std::lock_guard<std::mutex> lock(traverseFilesMutex); 
                isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end()); 
                localIsoFiles.clear(); 
            }
        }

        // Final Flush of local buffer
        if (!localIsoFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
        }

    } catch (const std::filesystem::filesystem_error& e) {
        // 5. Error Handling (Theme-aware)
        std::string errCol = std::string(isOriginal ? originalColors::red : theme->secondary);
        std::string formattedError = "\n" + errCol + "Error: " + path.string() + " - " + 
                                     e.what() + std::string(originalColors::boldAlt);
        
        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex); 
            uniqueErrorMessages.insert(formattedError); 
        }
    }
}


// IMAGE SECTION


// Function to check and list stored ram cache
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension, 
                  const std::vector<std::string>& binImgFilesCache, 
                  const std::vector<std::string>& mdfMdsFilesCache, 
                  const std::vector<std::string>& nrgFilesCache, bool modeMdf, bool modeNrg) {
    
    // Ignore SIGINT (Ctrl+C) and disable Ctrl+D for this UI state
    signal(SIGINT, SIG_IGN);        
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    // Check if the relevant cache is empty based on the current mode
    bool isEmpty = false;
    if (!modeMdf && !modeNrg) isEmpty = binImgFilesCache.empty();
    else if (modeMdf)         isEmpty = mdfMdsFilesCache.empty();
    else if (modeNrg)         isEmpty = nrgFilesCache.empty();

    if (isEmpty && list) {
        // Use theme->warning for the "No entries" notification
        std::cout << "\n" << (isOriginal ? originalColors::yellow : theme->warning) 
                  << "No " << fileExtension << " entries stored in RAM.\033[J" 
                  << originalColors::bold << "\n";

        // Standardized "Press Enter" prompt using the theme's muted/secondary color
        std::cout << color << "\n↵ to continue..." << reset; 
        
        // Wait for the user to press Enter
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        // Clear files to prevent artifacts in the UI selection logic
        files.clear();
        
        // Wipe terminal state
        clearScrollBuffer();
        return;
        
    } else if (list) {
        // Populate the active 'files' vector from the specific cache
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


// Function to clear Ram Cache and memory transformations for bin/img mdf nrg files
void clearRamCache(bool& modeMdf, bool& modeNrg) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::vector<std::string> extensions;
    std::string cacheType;
    bool cacheIsEmpty = false;

    // --- Cache Identification ---
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

    // --- Transformation Cache Cleanup ---
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

    // --- UI Feedback ---
    // Use theme->warning for "nothing to clear" and theme->accent for success
    if (cacheIsEmpty && !transformationCacheWasCleared) {
        std::cout << "\n" << (isOriginal ? originalColors::yellow : theme->warning) 
                  << cacheType << " buffer is empty. Nothing to clear.\033[J" 
                  << originalColors::boldAlt << "\n";
    } else {
        std::cout << "\n" << (isOriginal ? originalColors::green : theme->accent) 
                  << cacheType << " buffer cleared.\033[J" 
                  << originalColors::boldAlt << "\n";
    }

    // "Press Enter" prompt using the theme's muted color
    std::cout << color << "\n↵ to continue..." << reset; 

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}


// Blacklist function for MDF BIN IMG NRG
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();
    std::string extLower = ext;
    toLowerInPlace(extLower);

    // Default mode: .bin and .img files
    if (!blacklistMdf && !blacklistNrg) {
        if (!((extLower == ".bin" || extLower == ".img"))) {
            return false;
        }
    } 
    // MDF mode
    else if (blacklistMdf) {
        if (extLower != ".mdf") {
            return false;
        }
    } 
    // NRG mode
    else if (blacklistNrg) {
        if (extLower != ".nrg") {
            return false;
        }
    }

    // Blacklisted keywords (previously commented out)
    std::unordered_set<std::string> blacklistKeywords = {};
    
    // Convert filename to lowercase without extension
    std::string filenameLowerNoExt = filenameLower;
    filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size());

    // Check blacklisted keywords
    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}


// Function to process a single batch of paths and find files for findFiles
std::unordered_set<std::string> processPaths(const std::string& path, const std::string& mode, 
                                            const std::function<void(const std::string&, const std::string&)>& callback, 
                                            std::unordered_set<std::string>& processedErrorsFind) {
    // Fetch the active theme and check if we are in "original" mode
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
            // 1. Handle User Cancellation
            if (g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(globalSetsMutex);
                    processedErrorsFind.clear();
                    localFileNames.clear();
                    
                    std::string type = (blacklistMdf) ? "MDF" : (blacklistNrg) ? "NRG" : "BIN/IMG";
                    
                    // Use theme->warning (or yellow if original)
                    std::string warnCol = std::string(isOriginal ? originalColors::yellow : theme->warning);
                    
                    processedErrorsFind.insert(warnCol + type + " search interrupted by user.\n\n" + 
                                             std::string(originalColors::boldAlt));
                }
                break;
            }
            
            if (entry.is_regular_file()) {
                totalFiles.fetch_add(1, std::memory_order_acq_rel);
                
                // 2. Update Progress (Theme-aware)
                if (totalFiles % 100 == 0) {
                    std::lock_guard<std::mutex> lock(couNtMutex);
                    // Use theme->accent for the "Total files" label to make it pop
                    std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                              << "Total files processed: " << totalFiles << std::flush;
                }
                
                // 3. Blacklist & Cache Logic
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
        // 4. Handle Errors (Theme-aware)
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        // Using theme->secondary as the error color
        std::string errCol = std::string(isOriginal ? originalColors::red : theme->secondary);
        
        processedErrorsFind.insert(errCol + "Error traversing path: " + path + " - " + 
                                 e.what() + std::string(originalColors::boldAlt));
    }
    
    // Final UI cleanup
    {
        std::lock_guard<std::mutex> lock(couNtMutex);
        std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                  << "Total files processed: " << totalFiles << originalColors::reset;
    }
    
    return localFileNames;
}


// Modified findFiles that spawns one thread per unique path.
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths, std::unordered_set<std::string>& fileNames, int& currentCacheOld, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& invalidDirectoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    // Setup signal handler and reset cancellation flag
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);
    
    // Disable input before processing
    disableInput();
    
    // Choose the appropriate cache upfront
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
    
    // Create threads for each unique input path using a thread pool.
    std::vector<std::future<std::unordered_set<std::string>>> threadFutures;
    std::unordered_set<std::string> processedValidPaths;
    std::vector<std::string> uniquePaths;
    
    for (const auto& originalPath : inputPaths) {
        std::string path = std::filesystem::path(originalPath).string();
        // Skip empty or already processed paths.
        if (path.empty() || !processedValidPaths.insert(path).second) {
            continue;
        }
        uniquePaths.push_back(path);
    }
    
    unsigned int numThreads = std::min({
		static_cast<unsigned int>(uniquePaths.size()),
		static_cast<unsigned int>(maxThreads),
		static_cast<unsigned int>(MAX_USEFUL_THREADS)
	});
	
    if (numThreads == 0) {
        flushStdin();
        restoreInput();
        return *currentCache;
    }
    
    {
        // Create a local thread pool. When this block exits, the pool's destructor
        // will stop and join all threads.
        ThreadPool pool(numThreads);
    
        for (const auto& path : uniquePaths) {
            threadFutures.push_back(pool.enqueue([path, &mode, &callback, &processedErrorsFind]() -> std::unordered_set<std::string> {
                return processPaths(path, mode, callback, std::ref(processedErrorsFind));
            }));
        }
    
        // Wait for all tasks to finish and collect results.
        for (auto& future : threadFutures) {
            std::unordered_set<std::string> threadResult = future.get();
            fileNames.insert(threadResult.begin(), threadResult.end());
        }
    } // The thread pool is automatically cleaned up here.
    
    verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
    
    // Efficiently update cache with new files.
    std::unordered_set<std::string> currentCacheSet(currentCache->begin(), currentCache->end());
    std::vector<std::string> newFiles;
    
    for (const auto& fileName : fileNames) {
        if (currentCacheSet.insert(fileName).second) {
            newFiles.push_back(fileName);
        }
    }
    
    // Append all new files at once.
    if (!newFiles.empty()) {
        currentCache->insert(currentCache->end(), newFiles.begin(), newFiles.end());
    }
    
    // Restore input.
    flushStdin();
    restoreInput();
    
    return *currentCache;
}


// Returns true if a special command was handled (caller should `continue`)
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


// Function to search  files based on user's choice of file type (MDF, BIN/IMG, NRG)
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning) {
    // --- Configuration ---
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
        std::cout << originalColors::red << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG" << originalColors::reset << "\n";
        return;
    }

    const std::string& fileType      = fileTypeChoice;
    const std::string& fileExtension = configIt->second.extension;
    const bool modeMdf = (fileType == "mdf");
    const bool modeNrg = (fileType == "nrg");

    // --- Pre-allocate caches ---
    std::vector<std::string> files;
    files.reserve(100);
    binImgFilesCache.reserve(100);
    mdfMdsFilesCache.reserve(100);
    nrgFilesCache.reserve(100);

    // --- Helpers ---
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

    // --- Main loop ---
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

        // Define semantic colors based on active theme or original fallback
        std::string_view headCol = isOriginal ? originalColors::green  : theme->accent;
        std::string_view textCol = isOriginal ? originalColors::blue   : theme->muted;
        std::string_view extCol  = isOriginal ? originalColors::orange : theme->accent;
        std::string_view ramCol  = isOriginal ? originalColors::yellow : theme->accent;

        // Construct dynamic, theme-aware prompt
        std::string prompt;
        prompt.reserve(512);
        prompt.append("\001").append(headCol).append("\002FolderPaths")
              .append("\001").append(textCol).append("\002 ↵ to scan for \001")
              .append("\001").append(extCol).append("\002").append(fileExtension)
              .append("\001").append(textCol).append("\002 entries into \001")
              .append("\001").append(ramCol).append("\002RAM\001")
              .append("\001").append(textCol).append("\002, ? ↵ help, ↵ return:\n\001")
              .append(originalColors::boldAlt).append("\002");
        
        // --- Read input ---
        std::unique_ptr<char, decltype(&std::free)> mainSearch(readline(prompt.c_str()), &std::free);

        if (isBlankInput(mainSearch.get())) break;

        const std::string inputSearch = trimWhitespace(mainSearch.get());

        bool list = false;
        if (dispatchSpecialCommandForBinImgMdfNrgSearch(inputSearch, configPath, modeMdf, modeNrg,
                                   fileExtension, files, fileType,
                                   newISOFound, list, isImportRunning))
            continue;

        // --- Scan directories ---
        std::cout << " \n\033[3H\033[J\n";

        std::istringstream ss(inputSearch);
        std::string path;
        while (std::getline(ss, path, ';')) {
            if (!path.empty() && uniquePaths.insert(path).second) {
                if (isValidDirectory(path)) {
                    directoryPaths.push_back(path);
                } else {
                    // Use theme secondary (usually red/error) for invalid paths
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
            // Error color from theme
            std::cerr << "\n\n" << (isOriginal ? originalColors::red : theme->secondary) 
                      << "Unable to access local database: " << e.what() 
                      << originalColors::reset << std::endl;
        }

        verboseSearchResults(fileExtension, fileNames, invalidDirectoryPaths,
                             newFilesFound, list, currentCacheOld, files,
                             start_time, processedErrorsFind, directoryPaths);

        if (!newFilesFound) continue;

        if (!files.empty() && !g_operationCancelled.load())
            selectForImageFiles(fileType, files, newISOFound, list, isImportRunning);
    }
}
