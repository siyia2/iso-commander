// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Function to check if a directory input is valid for searches
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function for manual database refresh for displaying ISO list
void manualRefreshForDatabase(std::string& initialDir, bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound) {
    try {
        enable_ctrl_d();
        // Setup signal handler at the start of the operation
        setupSignalHandlerCancellations();
        
        // Reset cancellation flag
        g_operationCancelled.store(false);
        
        // Centralize input handling
        std::string input = initialDir;
        if (input.empty()) {
            if (promptFlag) {
                clearScrollBuffer();
            }
            
            loadHistory(filterHistory);
            
            const std::unordered_set<std::string> validInputs = {
                "*fl_m", "*cl_m", "*fl_u", "*cl_u", "*fl_fo", "*cl_fo", "*fl_w", "*cl_w", "*fl_c", "*cl_c"
            };
            
            // Restore readline autocomplete and screen clear bindings
            rl_bind_key('\f', clear_screen_and_buffer);
            rl_bind_key('\t', rl_complete);
            
            bool isCpMv = false;
            // Prompt the user to enter directory paths for manual database refresh
            std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ to scan for \001\033[1;92m\002.iso\001\033[1;94m\002 files and import them into the \001\033[1;92m\002local\001\033[1;94m\002 database, ? ↵ for help, ↵ to return:\n\001\033[0;1m\002";
            char* rawSearchQuery = readline(prompt.c_str());
            
            // Handle EOF (Ctrl+D) scenario
            if (!rawSearchQuery) {
                input.clear();  // Explicitly clear input to trigger early exit
            } else {
                std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
                input = trimWhitespace(searchQuery.get());  // Trim only leading and trailing spaces
                
                if (input == "?") {
                    bool import2ISO = true;
                    helpSearches(isCpMv, import2ISO);
                    input = "";
                    std::string dummyDir = "";
                    manualRefreshForDatabase(dummyDir, promptFlag, maxDepth, filterHistory, newISOFound);
                }
                
                if (input ==  "config" || input == "stats" || input == "!clr" || input == "!clr_paths" || input == "!clr_filter" || input == "*auto_off" || input == "*auto_on" || isValidInput(input) || input.starts_with("*pagination_")) {
                    databaseSwitches(input, promptFlag, maxDepth, filterHistory, newISOFound);
                    return;
                }
                
                if (!input.empty() && promptFlag) {
                    add_history(input.c_str());
                    std::cout << "\n";
                }
            }
        }

        // Early exit for empty or whitespace-only input
        if (std::all_of(input.begin(), input.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
            return;
        }
		
        // Combine path validation and processing
        std::unordered_set<std::string> uniquePaths;
        std::vector<std::string> validPaths;
        std::unordered_set<std::string> invalidPaths;
        std::unordered_set<std::string> uniqueErrorMessages;
        std::vector<std::string> allIsoFiles;
        std::atomic<size_t> totalFiles{0};

        if (promptFlag) {
            // Move the cursor to line 3 (2 lines down from the top)
            std::cout << "\033[3H";
            // Clear any listings if visible and leave a new line
            std::cout << "\033[J";
            std::cout << "\n";
            disableInput();
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        // First, process input paths and collect valid ones
        std::istringstream iss(input);
        std::string path;
        while (std::getline(iss, path, ';')) {
            if (!isValidDirectory(path)) {
                if (promptFlag) {
                    invalidPaths.insert(path);
                }
                continue;
            }
            if (uniquePaths.insert(path).second) {
                validPaths.push_back(path);
            }
        }
        
        // Use the global maxThreads variable. Determine numThreads as the minimum of valid paths and maxThreads.
        int numThreads = std::min(static_cast<int>(validPaths.size()), static_cast<int>(maxThreads));
        if (numThreads == 0) {
            // No valid paths to process, return early
            return;
        }
        
        {
			ThreadPool pool(numThreads);

			// Mutexes for synchronization shared across tasks
			std::mutex processMutex;
			std::mutex traverseErrorMutex;

			// Enqueue tasks into the thread pool
			std::vector<std::future<void>> futures;
			for (const auto& validPath : validPaths) {
				futures.emplace_back(pool.enqueue([&, validPath]() {
					traverse(validPath, allIsoFiles, uniqueErrorMessages, totalFiles,
							 processMutex, traverseErrorMutex, maxDepth, promptFlag);
				}));
			}
			
			// Wait for all tasks to finish
			for (auto& future : futures) {
				future.wait();
				if (g_operationCancelled.load()) break;
			}

			// When this block ends, the ThreadPool destructor will clean up the threads.
		}
        
        // Post-processing
        if (promptFlag) {
            flushStdin();
            restoreInput();
            std::cout << "\r\033[0;1mTotal files processed: " << totalFiles;
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
            verboseForDatabase(allIsoFiles, totalFiles, validPaths, invalidPaths,
                               uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newISOFound);
        } else {
            if (!g_operationCancelled.load()) {
                saveToDatabase(allIsoFiles, newISOFound);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\n\033[1;91mUnable to access ISO database: " << e.what() << std::endl;
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::string dummyDir = "";
        manualRefreshForDatabase(dummyDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }
}


// Function to traverse a directory and find ISO files
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag) {
    const size_t BATCH_SIZE = 100;
    std::vector<std::string> localIsoFiles;
    
    std::atomic<bool> g_CancelledMessageAdded{false};
    // Reset cancellation flag
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
					std::lock_guard<std::mutex> lock(globalSetsMutex);
					uniqueErrorMessages.clear();
					uniqueErrorMessages.insert("\n\033[1;33mISO search interrupted by user.\033[0;1m");
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
                    if (totalFiles % 100 == 0) { // Update display periodically
						std::lock_guard<std::mutex> lock(couNtMutex);
                        std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
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

        // Merge leftovers
        if (!localIsoFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
        }

        // Merge errors
    } catch (const std::filesystem::filesystem_error& e) {
        std::string formattedError = "\n\033[1;91mError traversing directory: " + 
                                    path.string() + " - " + e.what() + "\033[0;1m";
        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
            uniqueErrorMessages.insert(formattedError);
        }
    }
}


// Function to search  files based on user's choice of file type (MDF, BIN/IMG, NRG)
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound) {
    // Setup file type configuration
    std::string fileExtension, fileTypeName, fileType = fileTypeChoice;
    bool modeMdf = (fileType == "mdf");
    bool modeNrg = (fileType == "nrg");

    // Configure file type specifics once
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        fileTypeName = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileTypeName = "MDF";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileTypeName = "NRG";
    } else {
        std::cout << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG\n";
        return;
    }
    
    // Pre-allocate container space
    std::vector<std::string> files;
    files.reserve(100);
    binImgFilesCache.reserve(100);
    mdfMdsFilesCache.reserve(100);
    nrgFilesCache.reserve(100);
    
    // Define prompt once
    std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ to scan for \001\033[1;38;5;208m\002" + fileExtension +
                         "\001\033[1;94m\002 files and store them into \001\033[1;93m\002RAM\001\033[1;94m\002, ? ↵ for help, ↵ to return:\n\001\033[0;1m\002";
    
    // Main processing loop
    while (true) {
        // Reset state for each iteration
        int currentCacheOld = 0;
        std::vector<std::string> directoryPaths;
        std::unordered_set<std::string> uniquePaths, processedErrors, processedErrorsFind;
        std::unordered_set<std::string> successOuts, skippedOuts, failedOuts, invalidDirectoryPaths, fileNames;
        bool list = false, clr = false, newFilesFound = false;

        // Setup environment
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        clearScrollBuffer();
        clear_history();
        bool filterHistory = false;
        loadHistory(filterHistory);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_key('\t', rl_complete);
        
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> mainSearch(readline(prompt.c_str()), &std::free);
        
        // Check for exit conditions
        if (!mainSearch.get() || mainSearch.get()[0] == '\0' || 
            std::all_of(mainSearch.get(), mainSearch.get() + strlen(mainSearch.get()), 
                       [](char c) { return c == ' '; })) {
            break;
        }
        
        // Process input
        std::string inputSearch = trimWhitespace(mainSearch.get());
        
        if (inputSearch == "stats") {
			displayDatabaseStatistics(databaseFilePath, maxDatabaseSize, transformationCache, globalIsoFileList);
			continue;
		}
		
		if (inputSearch == "config") {
			displayConfigurationOptions(configPath);
			continue;
		}
		
		if (inputSearch.starts_with("*pagination_")) {
			updatePagination(inputSearch, configPath);
			continue;
		}
		
        // Handle special commands
        if (inputSearch == "!clr_paths" || inputSearch == "!clr_filter") {
            clearHistory(inputSearch);
            continue;
        }
        
        if (isValidInput(inputSearch)) {
            setDisplayMode(inputSearch);
            continue;
        }
        
        if (inputSearch == "?") {
            bool isCpMv = false, import2ISO = false;
            helpSearches(isCpMv, import2ISO);
            continue;
        }
        
        // Determine operation type
        list = (inputSearch == "ls");
        clr = (inputSearch == "!clr");
        
        // Handle cache clearing
        if (clr) {
            clearRamCache(modeMdf, modeNrg);
            continue;
        }
        
        // Show cache contents if requested
		if (list) {
			ramCacheList(files, list, fileExtension, binImgFilesCache, mdfMdsFilesCache, nrgFilesCache, modeMdf, modeNrg);
			if (files.empty()) {
				continue;
			}
			// If files is not empty, display corresponding list
		}
        
        // Add spacing for non-list, non-clear operations
        if (!inputSearch.empty() && !list && !clr) {
            std::cout << " " << std::endl;
        }
        
        // Start timing for performance measurement
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Process file search (if not just listing)
        if (!list) {
			// Move the cursor to line 3 (2 lines down from the top)
			std::cout << "\033[3H";
			// Clear any listings if visible and leave a new line
			std::cout << "\033[J";
			std::cout << "\n";
            // Parse input paths
            std::istringstream ss(inputSearch);
            std::string path;
            
            // Collect valid paths
            while (std::getline(ss, path, ';')) {
                if (!path.empty() && uniquePaths.insert(path).second) {
                    if (isValidDirectory(path)) {
                        directoryPaths.push_back(path);
                    } else {
                        invalidDirectoryPaths.insert("\033[1;91m" + path);
                    }
                }
            }
            
            // Find matching files
            files = findFiles(directoryPaths, fileNames, currentCacheOld, fileType,
                             [&](const std::string&, const std::string&) { newFilesFound = true; },
                             directoryPaths, invalidDirectoryPaths, processedErrorsFind);
            
            // Update history if valid paths were processed
            try {
				if (!directoryPaths.empty()) {
					add_history(inputSearch.c_str());
					saveHistory(filterHistory);
				}
			} catch (const std::exception& e) {
				std::cerr << "\n\n\033[1;91mUnable to access local database: " << e.what();
			// Optionally, you can log the error or take other actions here
			}
            
            // Display search results
            verboseSearchResults(fileExtension, fileNames, invalidDirectoryPaths, 
                                newFilesFound, list, currentCacheOld, files, 
                                start_time, processedErrorsFind, directoryPaths);
            
            if (!newFilesFound) {
                continue;
            }
        }
        
        // Process files if operation wasn't cancelled
        if (!g_operationCancelled.load()) {
            select_and_convert_to_iso(fileType, files, newISOFound, list);
        }
    }
}


// Function to process a single batch of paths and find files for findFiles
std::unordered_set<std::string> processPaths(const std::string& path, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, std::unordered_set<std::string>& processedErrorsFind) {
    
    // Atomic counter for tracking total number of files processed
    std::atomic<size_t> totalFiles{0};
    
    // Local set to track found filenames
    std::unordered_set<std::string> localFileNames;
    
    // Flag to ensure cancellation message is only added once
    std::atomic<bool> g_CancelledMessageAdded{false};
    
    // Reset the operation cancellation flag
    g_operationCancelled.store(false);
    
    // Disable user input during processing
    disableInput();
    
    try {
        // Set blacklist flags based on the mode
        bool blacklistMdf = (mode == "mdf");
        bool blacklistNrg = (mode == "nrg");
        
        // Recursively iterate through all files and directories in the single path
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            // Check if operation was cancelled by user
            if (g_operationCancelled.load()) {
                // Only add cancellation message once
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(globalSetsMutex);
                    // Clear all collected data
                    processedErrorsFind.clear();
                    localFileNames.clear();
                    
                    // Determine file type based on mode
                    std::string type = (blacklistMdf) ? "MDF" : (blacklistNrg) ? "NRG" : "BIN/IMG";
                    
                    // Add cancellation message to errors
                    processedErrorsFind.insert("\033[1;33m" + type + " search interrupted by user.\n\n\033[0;1m");
                }
                break;
            }
            
            // Process only regular files (not directories or symlinks)
            if (entry.is_regular_file()) {
                // Increment the file counter atomically
                totalFiles.fetch_add(1, std::memory_order_acq_rel);
                
                // Update progress display every 100 files
                if (totalFiles % 100 == 0) {
                    std::lock_guard<std::mutex> lock(couNtMutex);
                    std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
                }
                
                // Check if file passes the blacklist filter for the current mode
                if (blacklist(entry, blacklistMdf, blacklistNrg)) {
                    std::string fileName = entry.path().string();
                    
                    // Ensure thread-safe access to shared data structures
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex);
                        
                        // Check if the file is already in the appropriate cache
                        bool isInCache = false;
                        if (mode == "nrg") {
                            isInCache = (std::find(nrgFilesCache.begin(), nrgFilesCache.end(), fileName) != nrgFilesCache.end());
                        } else if (mode == "mdf") {
                            isInCache = (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) != mdfMdsFilesCache.end());
                        } else if (mode == "bin") {
                            isInCache = (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) != binImgFilesCache.end());
                        }
                        
                        // Only process files not already in cache
                        if (!isInCache) {
                            // Add to local set and call callback if file is newly discovered
                            if (localFileNames.insert(fileName).second) {
                                callback(fileName, entry.path().parent_path().string());
                            }
                        }
                    }
                }
            }
        }
        
    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors (e.g., permission issues)
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        std::string errorMessage = "\033[1;91mError traversing path: " 
            + path + " - " + e.what() + "\033[0;1m";
        processedErrorsFind.insert(errorMessage);
    }
    
    // Print final count of processed files
    {
        std::lock_guard<std::mutex> lock(couNtMutex);
        std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << "\033[0;1m";
    }
    
    // Return the set of discovered filenames
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
    
    int numThreads = std::min(static_cast<int>(uniquePaths.size()), static_cast<int>(maxThreads));
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
