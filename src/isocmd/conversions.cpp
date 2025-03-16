
// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"
#include "../mdf.h"
#include "../ccd.h"


std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here

// Function to clear Ram Cache and memory transformations for bin/img mdf nrg files
void clearRamCache(bool& modeMdf, bool& modeNrg) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
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

    // Manually remove items with matching extensions from transformationCache
    bool transformationCacheWasCleared = false;
    bool originalCacheWasCleared = false;
    
    for (auto it = transformationCache.begin(); it != transformationCache.end();) {
		const std::string& key = it->first;
		std::string keyLower = key; // Create a lowercase copy of the key
		toLowerInPlace(keyLower);

		bool shouldErase = std::any_of(extensions.begin(), extensions.end(),
			[&keyLower](std::string ext) { // Pass by value to modify locally
				toLowerInPlace(ext); // Convert extension to lowercase
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

	// Manually remove items with matching extensions from original cache
	for (auto it = originalPathsCache.begin(); it != originalPathsCache.end();) {
		const std::string& key = it->first;
		std::string keyLower = key; // Create a lowercase copy of the key
		toLowerInPlace(keyLower);

		bool shouldErase = std::any_of(extensions.begin(), extensions.end(),
			[&keyLower](std::string ext) { // Pass by value to modify locally
				toLowerInPlace(ext); // Convert extension to lowercase
				return keyLower.size() >= ext.size() &&
					keyLower.compare(keyLower.size() - ext.size(), ext.size(), ext) == 0;
			});

		if (shouldErase) {
			it = originalPathsCache.erase(it);
			originalCacheWasCleared = true;
		} else {
			++it;
		}
	}


    // Display appropriate messages
    if (cacheIsEmpty && (!transformationCacheWasCleared || !originalCacheWasCleared)) {
        std::cout << "\n\033[1;93m" << cacheType << " buffer is empty. Nothing to clear.\033[0;1m\n";
    } else {
        std::cout << "\n\033[1;92m" << cacheType << " buffer cleared.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}


// Function to check and list stored ram cache
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension, const std::vector<std::string>& binImgFilesCache, const std::vector<std::string>& mdfMdsFilesCache, const std::vector<std::string>& nrgFilesCache, bool modeMdf, bool modeNrg) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    if (((binImgFilesCache.empty() && !modeMdf && !modeNrg) || 
         (mdfMdsFilesCache.empty() && modeMdf) || 
         (nrgFilesCache.empty() && modeNrg)) && list) {
        std::cout << "\n\033[1;93mNo " << fileExtension << " entries stored in RAM.\033[1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        files.clear();
        clearScrollBuffer();
        return;
    } else if (list) {
        if (!modeMdf && !modeNrg) {
            files = binImgFilesCache;
        } else if (modeMdf) {
            files = mdfMdsFilesCache;
        } else if (modeNrg) {
            files = nrgFilesCache;
        }
    }
}


// Function to select and convert files based on user's choice of file type
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
                    if (directoryExists(path)) {
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
std::unordered_set<std::string> processBatchPaths(const std::vector<std::string>& batchPaths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback,std::unordered_set<std::string>& processedErrorsFind) {
    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> localFileNames;
    
    std::atomic<bool> g_CancelledMessageAdded{false};
    g_operationCancelled.store(false);
    
    disableInput();

    for (const auto& path : batchPaths) {
		
        try {
            // Flags for blacklisting
            bool blacklistMdf = (mode == "mdf");
            bool blacklistNrg = (mode == "nrg");

            // Traverse directory
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
				if (g_operationCancelled.load()) {
					if (!g_CancelledMessageAdded.exchange(true)) {
						std::lock_guard<std::mutex> lock(globalSetsMutex);
						processedErrorsFind.clear();
						localFileNames.clear();
						std::string type = (blacklistMdf) ? "MDF" : (blacklistNrg) ? "NRG" : "BIN/IMG";
						processedErrorsFind.insert("\033[1;33m" + type + " search interrupted by user.\n\n\033[0;1m");
					}
					break;
				}
                if (entry.is_regular_file()) {
                    totalFiles.fetch_add(1, std::memory_order_acq_rel);
                    if (totalFiles % 100 == 0) { // Update display periodically
						std::lock_guard<std::mutex> lock(couNtMutex);
                        std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
                    }
                    
                    if (blacklist(entry, blacklistMdf, blacklistNrg)) {
                        std::string fileName = entry.path().string();
                        // Thread-safe insertion
                        {
                            std::lock_guard<std::mutex> lock(globalSetsMutex);
                            bool isInCache = false;
                            if (mode == "nrg") {
                                isInCache = (std::find(nrgFilesCache.begin(), nrgFilesCache.end(), fileName) != nrgFilesCache.end());
                            } else if (mode == "mdf") {
                                isInCache = (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) != mdfMdsFilesCache.end());
                            } else if (mode == "bin") {
                                isInCache = (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) != binImgFilesCache.end());
                            }
                            
                            if (!isInCache) {
                                if (localFileNames.insert(fileName).second) {
                                    callback(fileName, entry.path().parent_path().string());
                                }
                            }
                        }
                    }
                }
            }

            
        } catch (const std::filesystem::filesystem_error& e) {
			std::lock_guard<std::mutex> lock(globalSetsMutex);
            std::string errorMessage = "\033[1;91mError traversing path: " 
                + path + " - " + e.what() + "\033[0;1m";
            processedErrorsFind.insert(errorMessage);
        }
    }
	
	{
		std::lock_guard<std::mutex> lock(couNtMutex);
		// Print the total files processed after all paths are handled
		std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << "\033[0;1m";
	}

    return localFileNames;
}


// Function to search for .bin .img .nrg and mdf files
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
    
    // Constants for batch processing
    const size_t BATCH_SIZE = 100;
    const size_t MAX_CONCURRENT_BATCHES = maxThreads;
    
    // Create batches of unique input paths
    std::vector<std::vector<std::string>> pathBatches;
    std::vector<std::string> currentBatch;
    std::unordered_set<std::string> processedValidPaths;
    
    for (const auto& originalPath : inputPaths) {
        std::string path = std::filesystem::path(originalPath).string();
        
        // Skip empty or already processed paths
        if (path.empty() || !processedValidPaths.insert(path).second) {
            continue;
        }
        
        currentBatch.push_back(path);
        
        // Create a new batch when current one is full
        if (currentBatch.size() >= BATCH_SIZE) {
            pathBatches.push_back(std::move(currentBatch));
            currentBatch = std::vector<std::string>();
        }
    }
    
    // Add any remaining paths
    if (!currentBatch.empty()) {
        pathBatches.push_back(std::move(currentBatch));
    }
    
    // Process batches with thread pool
    std::vector<std::future<std::unordered_set<std::string>>> batchFutures;
    
    for (const auto& batch : pathBatches) {
        batchFutures.push_back(std::async(std::launch::async, processBatchPaths, 
                                         batch, mode, callback, std::ref(processedErrorsFind)));
        
        // Limit concurrent batches and check for cancellation
        if (batchFutures.size() >= MAX_CONCURRENT_BATCHES) {
            for (auto& future : batchFutures) {
                future.wait();
                if (g_operationCancelled.load()) {
                    // Clean up and exit early if operation was cancelled
                    restoreInput();
                    return *currentCache;
                }
            }
            batchFutures.clear();
        }
    }
    
    // Collect results from all batches
    for (auto& future : batchFutures) {
        std::unordered_set<std::string> batchResults = future.get();
        fileNames.insert(batchResults.begin(), batchResults.end());
    }
    
    verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
    
    // Efficiently update cache with new files
    std::unordered_set<std::string> currentCacheSet(currentCache->begin(), currentCache->end());
    std::vector<std::string> newFiles;
    
    for (const auto& fileName : fileNames) {
        if (currentCacheSet.insert(fileName).second) {
            newFiles.push_back(fileName);
        }
    }
    
    // Append all new files at once
    if (!newFiles.empty()) {
        currentCache->insert(currentCache->end(), newFiles.begin(), newFiles.end());
    }
    
    // Restore input
    flushStdin();
    restoreInput();
    
    return *currentCache;
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


// Function to clear and load list for image files
void clearAndLoadImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list) {
    // Clear the screen for new content
    clearScrollBuffer(); 
    // Assist in automatic removal of non-existent entries from cache
	files = 
    (!isFiltered && !binImgFilesCache.empty() && (fileType == "bin" || fileType == "img") &&
     (binImgFilesCache.size() != files.size() || !std::equal(binImgFilesCache.begin(), binImgFilesCache.end(), files.begin())))
        ? (need2Sort = true, binImgFilesCache) 
    : (!isFiltered && !mdfMdsFilesCache.empty() && fileType == "mdf" &&
       (mdfMdsFilesCache.size() != files.size() || !std::equal(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), files.begin())))
        ? (need2Sort = true, mdfMdsFilesCache) 
    : (!isFiltered && !nrgFilesCache.empty() && fileType == "nrg" &&
       (nrgFilesCache.size() != files.size() || !std::equal(nrgFilesCache.begin(), nrgFilesCache.end(), files.begin())))
        ? (need2Sort = true, nrgFilesCache) 
    : files;
            
    if (!list) {
		if (need2Sort) {
			sortFilesCaseInsensitive(files); // Sort the files case-insensitively
				(fileType == "bin" || fileType == "img") 
					? sortFilesCaseInsensitive(binImgFilesCache) 
						: fileType == "mdf" 
							? sortFilesCaseInsensitive(mdfMdsFilesCache) 
						: sortFilesCaseInsensitive(nrgFilesCache);
		}
			need2Sort = false;
	}
	
    printList(files, "IMAGE_FILES", "conversions"); // Print the current list of files
}


// Handle filtering for select_and_convert_to_iso
void handle_filtering(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort) {
    
    if (mainInputString == "/") {
        std::cout << "\033[1A\033[K";
        std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" + 
                                    fileExtensionWithOutDots + "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";
        
        // Inline filter query functionality
        while (true) {
            clear_history(); // Clear the input history
            filterHistory = true;
            loadHistory(filterHistory); // Load input history if available

            // Prompt the user for a search query
            std::unique_ptr<char, decltype(&std::free)> rawSearchQuery(readline(filterPrompt.c_str()), &std::free);
            std::string inputSearch(rawSearchQuery.get());

            // Exit the filter loop if input is empty or "/"
            if (inputSearch.empty() || inputSearch == "/") {
                std::cout << (hasPendingProcess ? "\033[4A\033[K" : "\033[2A\033[K");
                needsClrScrn = false;
                need2Sort = false;
                break;
            }

            // Filter files based on the input search query
            auto filteredFiles = filterFiles(files, inputSearch);
            if (filteredFiles.empty()) {
                std::cout << "\033[1A\033[K";
                continue; // Skip if no files match the filter
            }
            if (filteredFiles.size() == files.size()) {
                std::cout << (hasPendingProcess ? "\033[4A\033[K" : "\033[2A\033[K");
                needsClrScrn = false;
                need2Sort = false;
                break;
            }
            // Save the search query to history and update the file list
            try {
                add_history(rawSearchQuery.get());
                saveHistory(filterHistory);
            } catch (const std::exception& e) {
                // Optionally, you can log the error or take other actions here
            }
            
            filterHistory = false;
            clear_history(); // Clear history to reset for future inputs
            need2Sort = true;
            files = filteredFiles; // Update the file list with the filtered results
            needsClrScrn = true;
            isFiltered = true;
            pendingIndices.clear();
            hasPendingProcess = false;
            break;
        }
    } else if (mainInputString[0] == '/' && mainInputString.size() > 1) {
        // Directly filter the files based on the input without showing the filter prompt
        std::string inputSearch(mainInputString.substr(1)); // Skip the '/' character
        auto filteredFiles = filterFiles(files, inputSearch);
        if (!filteredFiles.empty() && !(filteredFiles.size() == files.size())) {
            filterHistory = true;
            loadHistory(filterHistory);
            try {
                add_history(inputSearch.c_str());
                saveHistory(filterHistory);
            } catch (const std::exception& e) {
                // Optionally, you can log the error or take other actions here
            }
            
            need2Sort = true;
            files = filteredFiles; // Update the file list with the filtered results
            
            isFiltered = true;
            needsClrScrn = true;
            pendingIndices.clear();
            hasPendingProcess = false;
            
            clear_history();
        } else {
            std::cout << (hasPendingProcess ? "\033[4A\033[K" : "\033[2A\033[K"); // Clear the line if no files match the filter
            need2Sort = false;
            needsClrScrn = false;
        }
    }
}


// Main function to select and convert image files based on type to ISO
void select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list) {

    // Bind keys for preventing clear screen and enabling tab completion
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    // Containers to track file processing results
    std::unordered_set<std::string> processedErrors, successOuts, skippedOuts, failedOuts;
    
    // New vector to store delayed execution indices
    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;
    
    // Reset page when entering this menu
    currentPage = 0;
    
    bool isFiltered = false; // Indicates if the file list is currently filtered
    bool needsClrScrn = true;
    bool filterHistory = false;
    bool need2Sort = true;

    std::string fileExtension = (fileType == "bin" || fileType == "img") ? ".bin/.img" 
                               : (fileType == "mdf") ? ".mdf" : ".nrg"; // Determine file extension based on type
    
    std::string fileExtensionWithOutDots;
    for (char c : fileExtension) {
        if (c != '.') {
            fileExtensionWithOutDots += toupper(c);  // Capitalize the character and add it to the result
        }
    }
    
    // Main processing loop
    while (true) {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        bool verbose = false; // Reset verbose mode
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        
        clear_history();
        if (needsClrScrn) clearAndLoadImageFiles(files, fileType, need2Sort, isFiltered, list);
        
        // Display pending indices if there are any
         if (hasPendingProcess && !pendingIndices.empty()) {
                std::cout << "\n\033[1;35mPending: " << (isFiltered ? "\033[1;96mF⊳\033[1;35m " : "");
                for (size_t i = 0; i < pendingIndices.size(); ++i) {
                    std::cout << "\033[1;93m" << pendingIndices[i];
                    if (i < pendingIndices.size() - 1) {
                        std::cout << " ";
                    }
                }
                std::cout << "\033[1;35m ([\033[1;92mproc\033[1;35m] ↵ to process [\033[1;93mclr\033[1;35m] ↵ to clear)\033[0;1m\n";
            }
            
        
        std::cout << "\n\n";
        std::cout << "\033[1A\033[K";
        
        // Build the user prompt string dynamically
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;38;5;208m\002" : "\001\033[1;38;5;208m\002")
                         + fileExtensionWithOutDots + "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";
        
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        if (!rawInput) break;
        
        std::string mainInputString(rawInput.get());
        
        // Check for clear pending command
        if (mainInputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
			needsClrScrn = true;
            continue;
        }
        // check if first char is ; and skip
        if (rawInput && rawInput.get()[0] == ';') {
			 std::cout << (hasPendingProcess ? "\033[4A\033[K" : "\033[2A\033[K"); // Clear the line if no files match the filter
			continue;
		}
		
        std::atomic<bool> isAtISOList{false};
        
        size_t totalPages = (ITEMS_PER_PAGE != 0) ? ((files.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE) : 0;
        bool validCommand = processPaginationHelpAndDisplay(mainInputString, totalPages, currentPage, needsClrScrn, false, false, false, true, isAtISOList);
        
        if (validCommand) continue;
                
        // Handle input for returning to the unfiltered list or exiting
        if (rawInput.get()[0] == '\0') {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore the original file list
                files = (fileType == "bin" || fileType == "img") ? binImgFilesCache :
                        (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);
                pendingIndices.clear();
                hasPendingProcess = false;
                needsClrScrn = true;
                isFiltered = false; // Reset filter status
                need2Sort = false;
                // Reset page when exiting filtered list
                currentPage = 0;
                continue;
            } else {
                need2Sort = false;
                break; // Exit the loop if no input
            }
        }
        
        // Check for "proc" command to execute pending operations
        if (mainInputString == "proc" && hasPendingProcess && !pendingIndices.empty()) {
            // Combine all pending indices into a single string as if they were entered normally
            std::string combinedIndices = "";
            for (size_t i = 0; i < pendingIndices.size(); ++i) {
                combinedIndices += pendingIndices[i];
                if (i < pendingIndices.size() - 1) {
                    combinedIndices += " ";
                }
            }
            
            // Process the pending operations
            processInput(combinedIndices, files, (fileType == "mdf"), (fileType == "nrg"), 
                        processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3);
            }
            continue;
        }       

        // Handle filter commands
        if (mainInputString == "/" || (!mainInputString.empty() && mainInputString[0] == '/')) {
            handle_filtering(mainInputString, files, fileExtensionWithOutDots, pendingIndices, hasPendingProcess, isFiltered, needsClrScrn, filterHistory, need2Sort);
            continue;
        }// Check if input contains a semicolon for delayed execution
        else if (mainInputString.find(';') != std::string::npos) {
			if (handlePendingInduction(mainInputString, pendingIndices, hasPendingProcess, needsClrScrn)) {
				continue; // Continue the loop if new indices were processed
			}
		}
        // Process other input commands for file processing
        else {
            processInput(mainInputString, files, (fileType == "mdf"), (fileType == "nrg"), 
                         processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3); // Print detailed logs if verbose mode is enabled
                needsClrScrn = true;
            }
        }
    }
}


// Function to calculate converted files size
size_t calculateSizeForConverted(const std::vector<std::string>& filesToProcess, bool modeNrg, bool modeMdf) {
    size_t totalBytes = 0;

    if (modeNrg) {
        for (const auto& file : filesToProcess) {
            std::ifstream nrgFile(file, std::ios::binary);
            if (nrgFile) {
                // Seek to the end of the file to get the total size
                nrgFile.seekg(0, std::ios::end);
                size_t nrgFileSize = nrgFile.tellg();

                // The ISO data starts after the 307,200-byte header
                size_t isoDataSize = nrgFileSize - 307200;

                // Add the ISO data size to the total bytes
                totalBytes += isoDataSize;
            }
        }
    } else if (modeMdf) {
        for (const auto& file : filesToProcess) {
            std::ifstream mdfFile(file, std::ios::binary);
            if (mdfFile) {
                MdfTypeInfo mdfInfo;
                if (!mdfInfo.determineMdfType(mdfFile)) {
                    continue;
                }
                mdfFile.seekg(0, std::ios::end);
                size_t fileSize = mdfFile.tellg();
                size_t numSectors = fileSize / mdfInfo.sector_size;
                totalBytes += numSectors * mdfInfo.sector_data;
            }
        }
    } else {
        for (const auto& file : filesToProcess) {
            std::ifstream ccdFile(file, std::ios::binary | std::ios::ate);
            if (ccdFile) {
                size_t fileSize = ccdFile.tellg();
                totalBytes += (fileSize / sizeof(CcdSector)) * DATA_SIZE;
            }
        }
    }

    return totalBytes;
}


// Function to process user input and convert selected BIN/MDF/NRG files to ISO format
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound) {
	// Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
    
    g_operationCancelled.store(false);
    
    std::unordered_set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;

    std::unordered_set<int> processedIndices;
    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))){
		tokenizeInput(input, fileList, processedErrors, processedIndices);
	} else {
		return;
	}
    
    if (processedIndices.empty()) {
		clearScrollBuffer();
		std::cout << "\n\033[1;91mNo valid input provided.\033[1;91m\n";
		std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		needsClrScrn = true;
        return;
    }

    // Ensure safe unsigned conversion of number of threads
    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
    std::vector<std::vector<size_t>> indexChunks;
    const size_t maxFilesPerChunk = 5;

    size_t totalFiles = processedIndices.size();
    size_t filesPerThread = (totalFiles + numThreads - 1) / numThreads;
    size_t chunkSize = std::min(maxFilesPerChunk, filesPerThread);

    auto it = processedIndices.begin();
    for (size_t i = 0; i < totalFiles; i += chunkSize) {
        auto chunkEnd = std::next(it, std::min(chunkSize, 
            static_cast<size_t>(std::distance(it, processedIndices.end()))));
        indexChunks.emplace_back(it, chunkEnd);
        it = chunkEnd;
    }
    
    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(fileList[index - 1]);
    }

    // Calculate total bytes and tasks
    size_t totalTasks = filesToProcess.size();  // Each file is a task
    
    size_t totalBytes = calculateSizeForConverted(filesToProcess, modeNrg, modeMdf);

	std::string operation = modeMdf ? (std::string("\033[1;38;5;208mMDF\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion")) :
                       modeNrg ? (std::string("\033[1;38;5;208mNRG\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion")) :
                                 (std::string("\033[1;38;5;208mBIN/IMG\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion"));
                     
	clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing \001\033[1;38;5;208m\002" << operation << "\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Use the enhanced progress bar with task tracking
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
        totalBytes, &completedTasks, &failedTasks, totalTasks, &isProcessingComplete, &verbose, std::string(operation));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> imageFilesInChunk;
        imageFilesInChunk.reserve(chunk.size());
        std::transform(
            chunk.begin(),
            chunk.end(),
            std::back_inserter(imageFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; }
        );

        futures.emplace_back(pool.enqueue([imageFilesInChunk = std::move(imageFilesInChunk), 
            &fileList, &successOuts, &skippedOuts, &failedOuts, 
            modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, &newISOFound]() {
            // Process each file with task tracking
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts, 
                modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, newISOFound);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
}


// Function to convert a BIN/IMG/MDF/NRG file to ISO format
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, const bool& modeMdf, const bool& modeNrg, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::atomic<bool>& newISOFound) {

    namespace fs = std::filesystem;

    // Batch size constant for inserting entries into sets
    const size_t BATCH_SIZE = 1000;

    // Collect unique directories from input file paths
    std::unordered_set<std::string> uniqueDirectories;
    for (const auto& filePath : imageFiles) {
        std::filesystem::path path(filePath);
        if (path.has_parent_path()) {
            uniqueDirectories.insert(path.parent_path().string());
        }
    }

    std::string result = std::accumulate(uniqueDirectories.begin(), uniqueDirectories.end(), std::string(), 
        [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ";" + b; });

    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    // Thread-local message buffers to reduce lock contention
    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs, localDeletedMsgs;

    // Function to check if any buffer has reached the batch size and flush if needed
    auto batchInsertMessages = [&]() {
        bool shouldFlush = 
            localSuccessMsgs.size() >= BATCH_SIZE ||
            localFailedMsgs.size() >= BATCH_SIZE ||
            localSkippedMsgs.size() >= BATCH_SIZE;
            
        if (shouldFlush) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            failedOuts.insert(localFailedMsgs.begin(), localFailedMsgs.end());
            skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            
            localSuccessMsgs.clear();
            localFailedMsgs.clear();
            localSkippedMsgs.clear();
        }
    };

    for (const std::string& inputPath : imageFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");

        if (!fs::exists(inputPath)) {
            localFailedMsgs.push_back(
                "\033[1;35mMissing: \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;35m.\033[0;1m");

            // Select the appropriate cache based on the mode.
            auto& cache = modeNrg ? nrgFilesCache :
                            (modeMdf ? mdfMdsFilesCache : binImgFilesCache);
            cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::ifstream file(inputPath);
        if (!file.good()) {
            localFailedMsgs.push_back("\033[1;91mThe specified file \033[1;93m'" + inputPath + "'\033[1;91m cannot be read. Check permissions.\033[0;1m");
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            localSkippedMsgs.push_back("\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m");
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::atomic<bool> conversionSuccess(false); // Atomic boolean for thread safety
        if (modeMdf) {
            conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        } else if (!modeMdf && !modeNrg) {
            conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);
        } else if (modeNrg) {
            conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        }

        auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath, "conversions");

        if (conversionSuccess) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid); // Attempt to change ownership, ignore result
            std::string fileNameLower = fileNameOnly;
			toLowerInPlace(fileNameLower);

			std::string fileType = 
								fileNameLower.ends_with(".bin") ? "\033[0;1m.bin" : 
								fileNameLower.ends_with(".img") ? "\033[0;1m.bin" : 
								fileNameLower.ends_with(".mdf") ? "\033[0;1m.mdf" : 
								fileNameLower.ends_with(".nrg") ? "\033[0;1m.nrg" : "\033[0;1mImage";

			localSuccessMsgs.push_back(fileType + " file converted to ISO: \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m");
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
			if (fs::exists(outputPath)) std::remove(outputPath.c_str());
            localFailedMsgs.push_back("\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m " + 
                                      (g_operationCancelled.load() ? "cancelled" : "failed") + ".\033[0;1m");
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        
        // Check if we need to batch insert
        batchInsertMessages();
    }

    // Insert any remaining messages under one lock
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
        failedOuts.insert(localFailedMsgs.begin(), localFailedMsgs.end());
        skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
    }

    // Update cache and prompt flags
    if (!successOuts.empty()) {
		bool promptFlag = false;
		bool filterHistory = false;
		int maxDepth = 0;
        manualRefreshForDatabase(result, promptFlag, maxDepth, filterHistory, newISOFound);
    }
}
