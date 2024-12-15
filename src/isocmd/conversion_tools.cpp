// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
static std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here


// GENERAL

// Function to check if a file already exists
bool fileExists(const std::string& fullPath) {
        return std::filesystem::exists(fullPath);
}


// Function to print verbose conversion messages
void verboseConversion(std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts) {
    // Lambda function to print each element in a set followed by a newline
    auto printWithNewline = [](const std::set<std::string>& outs) {
        for (const auto& out : outs) {
            std::cout << out << "\033[0;1m\n"; // Print each element in the set
        }
        if (!outs.empty()) {
            std::cout << "\n"; // Print an additional newline if the set is not empty
        }
    };

    // Print each set of messages with a newline after each set
    printWithNewline(successOuts);   // Print success messages
    printWithNewline(skippedOuts);   // Print skipped messages
    printWithNewline(failedOuts);	 // Print failed messages
    printWithNewline(deletedOuts);   // Print deleted messages
    printWithNewline(processedErrors); // Print error messages

    std::cout << "\033[1;32m↵ to continue...\033[0;1m"; // Prompt user to continue
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // Clear all sets after printing
    successOuts.clear();   // Clear the set of success messages
    skippedOuts.clear();   // Clear the set of skipped messages
    failedOuts.clear();		// Clear the set of failed messages
    deletedOuts.clear();   // Clear the set of deleted messages
    processedErrors.clear(); // Clear the set of error messages
}


// Function to print invalid directory paths from search
void verboseFind(std::set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::set<std::string>& processedErrorsFind) {
	
	if (directoryPaths.empty() && !invalidDirectoryPaths.empty()){
		std::cout << "\r\033[0;1mTotal files processed: 0" << std::flush;
	}
			
	if (!invalidDirectoryPaths.empty()) {
				std::cout << "\n\n";
		std::cout << "\033[0;1mInvalid paths omitted from search: \033[1:91m";

		for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
			if (it == invalidDirectoryPaths.begin()) {
				std::cerr << "\033[31m'"; // Red color for the first quote
			} else {
				std::cerr << "'";
			}
			std::cerr << *it << "'";
			// Check if it's not the last element
			if (std::next(it) != invalidDirectoryPaths.end()) {
				std::cerr << " ";
			}
		}std::cerr << "\033[0;1m."; // Print a newline at the end
	}
		
    if (!processedErrorsFind.empty()) {	
			std::cout << "\n";
		std::cout << "\n";
		for (const auto& errorMsg : processedErrorsFind) {
			std::cout << errorMsg;
		}
	}
		
	invalidDirectoryPaths.clear();
	processedErrorsFind.clear();
}


// Function that handles verbose results and timing from select select_and_convert_files_to_iso
void verboseSearchResults(const std::string& fileExtension, std::set<std::string>& fileNames, std::set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths) {

    auto end_time = std::chrono::high_resolution_clock::now();

    // Case: Files were found
    if (!fileNames.empty()) {

		if (processedErrorsFind.empty()) {
			std::cout << "\n\n";
		}
        std::cout << "\033[1;92mFound " << fileNames.size() << " matching files.\033[1;93m " << currentCacheOld << " matching files cached in RAM from previous searches.\033[0;1m\n\n";

    }

    // Case: No new files were found, but files exist in cache
    if (!newFilesFound && !files.empty() && !list) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        if (processedErrorsFind.empty()) {
			std::cout << "\n\n";
		}
        std::cout << "\033[1;91mNo new " << fileExtension << " files over 5MB found. \033[1;92m";
        std::cout << files.size() << " files are cached in RAM from previous searches.\033[0;1m\n\n";
    }

    // Case: No files were found
    if (files.empty() && !list) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
		if (processedErrorsFind.empty()) {
			std::cout << "\n\n";
		}
        std::cout << "\033[1;91mNo " << fileExtension << " files over 5MB found in the specified paths or cached in RAM.\n\033[0;1m\n";
    }
    
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
    
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
    return;
}


// Function to apply input filtering


// Function to clear Ram Cache and memory transformations for bin/img mdf nrg files
void clearRamCache (bool& modeMdf, bool& modeNrg) {

    std::vector<std::string> extensions;
    std::string cacheType;

    if (!modeMdf && !modeNrg) {
        extensions = {".bin", ".img"};
        binImgFilesCache.clear();
        cacheType = "BIN/IMG";
    } else if (modeMdf) {
        extensions = {".mdf"};
        mdfMdsFilesCache.clear();
        cacheType = "MDF";
    } else if (modeNrg) {
        extensions = {".nrg"};
        nrgFilesCache.clear();
        cacheType = "NRG";
    }

    // Manually remove items with matching extensions
    for (auto it = transformationCache.begin(); it != transformationCache.end();) {
        const std::string& key = it->first;
        bool shouldErase = std::any_of(extensions.begin(), extensions.end(), 
            [&key](const std::string& ext) {
                return key.size() >= ext.size() && 
                       key.compare(key.size() - ext.size(), ext.size(), ext) == 0;
            });
        
        if (shouldErase) {
            it = transformationCache.erase(it);
        } else {
            ++it;
        }
    }

    std::cout << "\n\033[1;92m" << cacheType << " RAM cache cleared.\033[0;1m\n";
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
    
}


// Function to select and convert files based on user's choice of file type
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
    // Prepare containers for files and caches
    std::vector<std::string> files;
    files.reserve(100);
    binImgFilesCache.reserve(100);
    mdfMdsFilesCache.reserve(100);
    nrgFilesCache.reserve(100);
    
    // To keep track the number of prior cached files
    int currentCacheOld = 0;
    
    // Tracking sets and vectors
    std::vector<std::string> directoryPaths;
    std::set<std::string> uniquePaths, processedErrors, processedErrorsFind, successOuts, 
                           skippedOuts, failedOuts, deletedOuts, 
                           invalidDirectoryPaths, fileNames;
                           
    // Control flags
    std::string fileExtension, fileTypeName, fileType = fileTypeChoice;
    bool modeMdf = (fileType == "mdf");
    bool modeNrg = (fileType == "nrg");

    // Configure file type specifics
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

    // Main processing loop
    while (true) {
		
        // Reset control flags and clear tracking sets
        bool list = false, clr = false;
        successOuts.clear(); 
        skippedOuts.clear(); 
        failedOuts.clear(); 
        deletedOuts.clear(); 
        processedErrors.clear();
        directoryPaths.clear();
        invalidDirectoryPaths.clear();
        uniquePaths.clear();
        files.clear();
        fileNames.clear();
        processedErrorsFind.clear();


        // Manage command history
        clear_history();
        historyPattern = false;
        loadHistory(historyPattern);
		
		// Restore readline autocomplete and screen clear bindings
		rl_bind_key('\f', rl_clear_screen);
		rl_bind_key('\t', rl_complete);
        
        // Interactive prompt setup (similar to original code)
        std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m ↵ to scan for \001\033[1;38;5;208m\002" + fileExtension +
                             "\001\033[1;94m files (>= 5MB) and import into \001\033[1;93m\002RAM\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), \001\033[1;92m\002ls \001\033[1;94m\002↵ open \001\033[1;93m\002RAM\001\033[1;94m\002 cache, "
                             "\001\033[1;93m\002clr\001\033[1;94m\002 ↵ clear \001\033[1;93m\002RAM\001\033[1;94m\002 cache, ↵ return:\n\001\033[0;1m\002";

        // Get user input
        char* rawinput = readline(prompt.c_str());
        std::unique_ptr<char, decltype(&std::free)> mainSearch(rawinput, &std::free);
        std::string inputSearch(mainSearch.get());

        // Exit condition
        if (std::isspace(mainSearch.get()[0]) || mainSearch.get()[0] == '\0') {
			clear_history();
            break;
        }

        // Determine input type
        list = (inputSearch == "ls");
        clr = (inputSearch == "clr");

        // Handle cache clearing (similar to original code)
        if (clr) {
			clearRamCache(modeMdf, modeNrg);
			continue;

		}
		 // Ram Cache emptiness checks and returns
        if (((binImgFilesCache.empty() && !modeMdf && !modeNrg) || (mdfMdsFilesCache.empty() && modeMdf) || (nrgFilesCache.empty() && modeNrg)) && list) {
    
			std::cout << "\n\033[1;93mNo " << fileExtension << " file entries stored in RAM cache for potential ISO conversions.\033[1m\n";
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			clearScrollBuffer();
			list = false;
			continue;
		} else if (list && !modeMdf && !modeNrg) {
            files = binImgFilesCache;
        } else if (list && modeMdf) {
            files = mdfMdsFilesCache;
        } else if (list && modeNrg) {
            files = nrgFilesCache;
        }

        // Manage command history
        if (!inputSearch.empty() && !list && !clr) {
            std::cout << " " << std::endl;
        }

        // Timing setup
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // New files tracking
        bool newFilesFound = false;

        // File collection (integrated file search logic)
        if (!list) {
			disableInput();
    
			// Parse input search paths
			std::istringstream ss(inputSearch);
			std::string path;
			std::set<std::string> uniquePaths;
    
			while (std::getline(ss, path, ';')) {
				// Trim leading and trailing whitespace
				size_t start = path.find_first_not_of(" \t");
				size_t end = path.find_last_not_of(" \t");
        
				// Check if the path is not just whitespace
				if (start != std::string::npos && end != std::string::npos) {
					// Extract the cleaned path
					std::string cleanedPath = path.substr(start, end - start + 1);
            
					// Check if the path is unique
					if (uniquePaths.find(cleanedPath) == uniquePaths.end()) {
						// Check if the directory exists
						if (directoryExists(cleanedPath)) {
							directoryPaths.push_back(cleanedPath);
							uniquePaths.insert(cleanedPath);
						} else {
							// Mark invalid directories with red color
							invalidDirectoryPaths.insert("\033[1;91m" + cleanedPath);
						}
					}
				}
			}
    
			// Find files with updated logic from the separate function
			files = findFiles(directoryPaths, fileNames, currentCacheOld, fileType, 
				[&](const std::string&, const std::string&) {
					newFilesFound = true;
				},
				directoryPaths,
				invalidDirectoryPaths, 
				processedErrorsFind 
			);
		}
		
		if (!directoryPaths.empty()) {
			add_history(mainSearch.get());
            saveHistory(historyPattern);       
		}
		
		
		if (!list) {
			verboseSearchResults(fileExtension, fileNames, invalidDirectoryPaths, newFilesFound, list, currentCacheOld, files, start_time, processedErrorsFind, directoryPaths);
			if (!newFilesFound) {
				continue;
			}
		}

        // File conversion workflow (using new modular function)
        select_and_convert_to_iso(fileType, files, verbose, 
                                promptFlag, maxDepth, historyPattern);
    }
}


// Function to handle conversions for select_and_convert_to_iso
void select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, bool& verbose, bool& promptFlag, int& maxDepth, bool& historyPattern) {
	// Calls prevent_clear_screen and tab completion
    rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
    rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
    
    std::set<std::string> processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts;
    
    bool isFiltered = false;
    bool isFilteredButUnchanged = false;
    bool needsScrnClr = true;
        std::string fileExtension;
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
    }

    while (true) {
        verbose = false;
        processedErrors.clear(); 
        successOuts.clear(); 
        skippedOuts.clear(); 
        failedOuts.clear(); 
        deletedOuts.clear();

        if (needsScrnClr) {
            clearScrollBuffer();
            std::cout << "\n";
            sortFilesCaseInsensitive(files);
            printFileList(files);
        }

        clear_history();
        std::string prompt = std::string(isFiltered ? "\n\001\033[1;96m\002Filtered \001\033[1;92m\002" : "\n\001\033[1;92m\002") + 
                             (fileType == "bin" || fileType == "img" ? "BIN/IMG" : (fileType == "mdf" ? "MDF" : "NRG")) + 
                             "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion (e.g., 1-3,1 5), ~ ↵ (un)fold, / ↵ filter, ↵ return:\001\033[0;1m\002 ";
        
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        std::string mainInputString(rawInput.get());

        // Toggle list folding logic
        if (mainInputString == "~") {
            toggleFullList = !toggleFullList;
            clearScrollBuffer();
            printFileList(files);
            continue;
        }

        // Exit conditions and list reset logic
        if (std::isspace(rawInput.get()[0]) || rawInput.get()[0] == '\0') {
            clearScrollBuffer();
            if (isFiltered && !isFilteredButUnchanged) {
                needsScrnClr = true;
                files = (fileType == "bin" || fileType == "img") ? binImgFilesCache :
                        (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);
                isFiltered = false;
                isFilteredButUnchanged = false;
                continue;
            } else {
                needsScrnClr = false;
                break;
            }
        }

        // Filtering logic
        if (strcmp(rawInput.get(), "/") == 0) {
            std::vector<std::string> beforeFilterFiles = files;
            std::string fileTypeName = (fileType == "bin" || fileType == "img" ? "BIN/IMG" : (fileType == "mdf" ? "MDF" : "NRG"));

            applyFilter(files, fileTypeName, historyPattern);

            // Update filter status based on file type
            std::vector<std::string>& cacheRef = (fileType == "bin" || fileType == "img") ? binImgFilesCache : (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);

            if (cacheRef.size() == files.size()) {
                isFilteredButUnchanged = true;
            } else {
                isFiltered = true;
                isFilteredButUnchanged = false;
            }
        } else {
            clearScrollBuffer();
            std::cout << "\033[1m" << std::endl;
            
            processInput(mainInputString, files, (fileType == "mdf"), (fileType == "nrg"), processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, promptFlag, maxDepth, historyPattern, verbose);
            
            clearScrollBuffer();
            std::cout << "\n";
            
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, 3);
            }
        }
    }
}


// Function to process user input and convert selected BIN/MDF/NRG files to ISO format
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
	// Calls prevent_clear_screen and tab completion
    rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
    rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
    // Set of selected file paths for processing
    std::set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;

    // Tokenize the input string to identify files to process
    std::vector<int> processedIndices;
    tokenizeInput(input, fileList, processedErrors, processedIndices);
    
    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n\033[1;91mNo valid indices for conversion.\033[1;91m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        clear_history();
        return;
    }

    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), std::thread::hardware_concurrency());
    std::vector<std::vector<size_t>> indexChunks;
    const size_t maxFilesPerChunk = 5;

    // Distribute files evenly among threads with chunk size limits
    size_t totalFiles = processedIndices.size();
    size_t filesPerThread = (totalFiles + numThreads - 1) / numThreads;
    size_t chunkSize = std::min(maxFilesPerChunk, filesPerThread);

    for (size_t i = 0; i < totalFiles; i += chunkSize) {
        auto chunkEnd = std::min(processedIndices.begin() + i + chunkSize, processedIndices.end());
        indexChunks.emplace_back(processedIndices.begin() + i, chunkEnd);
    }

    std::atomic<size_t> totalTasks(static_cast<int>(processedIndices.size()));
    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    int totalTasksValue = totalTasks.load();
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete), std::ref(verbose));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());
    std::mutex Mutex4Low; // Mutex for low-level processing

    for (const auto& chunk : indexChunks) {
        // Gather files for this chunk
        std::vector<std::string> imageFilesInChunk;
        imageFilesInChunk.reserve(chunk.size());
        std::transform(
            chunk.begin(),
            chunk.end(),
            std::back_inserter(imageFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; }
        );

        // Enqueue task for the thread pool
        futures.emplace_back(pool.enqueue([imageFilesInChunk = std::move(imageFilesInChunk), &fileList, &successOuts, &skippedOuts, &failedOuts, &deletedOuts, modeMdf, modeNrg, &maxDepth, &promptFlag, &historyPattern, &Mutex4Low, &completedTasks]() {
            // Process each file
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts, deletedOuts, modeMdf, modeNrg, maxDepth, promptFlag, historyPattern, Mutex4Low);
            completedTasks.fetch_add(imageFilesInChunk.size(), std::memory_order_relaxed);
        }));
    }

    // Wait for all threads to complete
    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    progressThread.join();

}


// Function to process a single batch of paths and find files for findFiles
std::set<std::string> processBatchPaths(const std::vector<std::string>& batchPaths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback,std::set<std::string>& processedErrorsFind) {
    std::mutex fileNamesMutex;
    std::atomic<size_t> totalFiles{0};
    std::set<std::string> localFileNames;

    for (const auto& path : batchPaths) {
        try {
            // Flags for blacklisting
            bool blacklistMdf = (mode == "mdf");
            bool blacklistNrg = (mode == "nrg");

            // Traverse directory
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    totalFiles++;
                    std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
                    
                    if (blacklist(entry, blacklistMdf, blacklistNrg)) {
                        std::string fileName = entry.path().string();
                        // Thread-safe insertion
                        {
                            std::lock_guard<std::mutex> lock(fileNamesMutex);
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
            std::string errorMessage = "\033[1;91mError traversing path: " 
                + path + " - " + e.what() + "\033[0;1m";
            processedErrorsFind.insert(errorMessage);
        }
    }

    // If no files were processed at all
    if (totalFiles == 0) {
        std::cout << "\r\033[0;1mTotal files processed: 0\033[0m" << std::flush;
    }

    return localFileNames;
}


// Function to search for .bin .img .nrg and mdf files over 5MB
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths, std::set<std::string>& fileNames, int& currentCacheOld, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, const std::vector<std::string>& directoryPaths, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrorsFind) {
    // Thread-safe synchronization primitives
    std::mutex pathsMutex;
    
    // Tracking sets and variables
    std::set<std::string> processedValidPaths;
    
    // Disable input before processing
    disableInput();

    // Consolidated set for all invalid paths
    std::set<std::string> invalidPaths;
    
    // Batch processing configuration
    const size_t BATCH_SIZE = 100;  // Number of paths per batch
    const size_t MAX_CONCURRENT_BATCHES = maxThreads;

    // Prepare batches of input paths
    std::vector<std::vector<std::string>> pathBatches;
    std::vector<std::string> currentBatch;

    // Group input paths into batches
    for (const auto& originalPath : inputPaths) {
        std::string path = std::filesystem::path(originalPath).string();
        
        // Minimize critical section for checking unique paths
        {
            std::lock_guard<std::mutex> lock(pathsMutex);
            if (!path.empty() && processedValidPaths.find(path) == processedValidPaths.end()) {
                processedValidPaths.insert(path);
                currentBatch.push_back(path);
            }
        }

        // Create batches
        if (currentBatch.size() >= BATCH_SIZE) {
            pathBatches.push_back(currentBatch);
            currentBatch.clear();
        }
    }

    // Add any remaining paths
    if (!currentBatch.empty()) {
        pathBatches.push_back(currentBatch);
    }

    // Batch processing with thread pool
    std::vector<std::future<std::set<std::string>>> batchFutures;
    
    // Process batches with thread pool
    for (const auto& batch : pathBatches) {
        batchFutures.push_back(std::async(std::launch::async, processBatchPaths, batch, mode, callback, std::ref(processedErrorsFind)));

        // Limit concurrent batches
        if (batchFutures.size() >= MAX_CONCURRENT_BATCHES) {
            for (auto& future : batchFutures) {
                future.wait();
            }
            batchFutures.clear();
        }
    }

    // Collect results from all batches
    for (auto& future : batchFutures) {
        std::set<std::string> batchResults = future.get();
        fileNames.insert(batchResults.begin(), batchResults.end());
    }

    // Restore input
    flushStdin();
    restoreInput();

    // Update invalid directory paths
    invalidDirectoryPaths.insert(invalidPaths.begin(), invalidPaths.end());
    
    verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);

    // Choose the appropriate cache
    std::set<std::string> currentCacheSet;
    std::vector<std::string>* currentCache = nullptr;

    if (mode == "bin") {
        currentCacheOld = binImgFilesCache.size();
        currentCache = &binImgFilesCache;
        currentCacheSet.insert(binImgFilesCache.begin(), binImgFilesCache.end());
    } else if (mode == "mdf") {
        currentCacheOld = mdfMdsFilesCache.size();
        currentCache = &mdfMdsFilesCache;
        currentCacheSet.insert(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end());
    } else if (mode == "nrg") {
        currentCacheOld = nrgFilesCache.size();
        currentCache = &nrgFilesCache;
        currentCacheSet.insert(nrgFilesCache.begin(), nrgFilesCache.end());
    } else {
        return {};
    }

    // Batch insert unique files
    std::vector<std::string> batch;
    const size_t batchInsertSize = 100;

    for (const auto& fileName : fileNames) {
        if (currentCacheSet.find(fileName) == currentCacheSet.end()) {
            batch.push_back(fileName);
            currentCacheSet.insert(fileName);

            if (batch.size() == batchInsertSize) {
                currentCache->insert(currentCache->end(), batch.begin(), batch.end());
                batch.clear();
            }
        }
    }

    // Insert remaining files
    if (!batch.empty()) {
        currentCache->insert(currentCache->end(), batch.begin(), batch.end());
    }

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

    // Check file size
    if (std::filesystem::file_size(entry) <= 5'000'000) {
        return false;
    }

    // Blacklisted keywords (previously commented out)
    std::set<std::string> blacklistKeywords = {};
    
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


// Function to print found BIN/IMG files with alternating colored sequence numbers
void printFileList(const std::vector<std::string>& fileList) {
    static const char* bold = "\033[1m";
    static const char* reset = "\033[0m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* orangeBold = "\033[1;38;5;208m";
    
    size_t maxIndex = fileList.size();
    size_t numDigits = std::to_string(maxIndex).length();

    // Precompute formatted indices
    std::vector<std::string> indexStrings(maxIndex);
    for (size_t i = 0; i < maxIndex; ++i) {
        indexStrings[i] = std::to_string(i + 1);
        indexStrings[i].insert(0, numDigits - indexStrings[i].length(), ' ');  // Right-align with padding
    }

    std::string output;
    output.reserve(fileList.size() * 100);  // Estimate buffer size

    for (size_t i = 0; i < fileList.size(); ++i) {
        const auto& filename = fileList[i];
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(filename);
        std::string_view fileNameView(fileNameOnly);
        size_t dotPos = fileNameView.rfind('.');
        bool isSpecialExtension = false;
        
        if (dotPos != std::string_view::npos) {
            std::string_view extensionView = fileNameView.substr(dotPos);
            std::string extension(extensionView);
            toLowerInPlace(extension);
            isSpecialExtension = (extension == ".bin" || extension == ".img" || 
                                  extension == ".mdf" || extension == ".nrg");
        }

        const char* sequenceColor = (i % 2 == 0) ? red : green;

        // Add index and colors
        output.append(isSpecialExtension ? sequenceColor : "")
              .append(indexStrings[i])
              .append(". ")
              .append(reset)
              .append(bold);

        // Add directory and filename
        if (isSpecialExtension) {
            output.append(directory)
                  .append("/")
                  .append(orangeBold)
                  .append(fileNameOnly);
        } else {
            output.append(filename);
        }

        // Final reset and newline
        output.append(reset)
              .append(bold)
              .append("\n");
    }

    std::cout << output;
}


// Function to convert a BIN/IMG/MDF/NRG file to ISO format
void convertToISO(const std::vector<std::string>& imageFiles, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, const bool& modeMdf, const bool& modeNrg, int& maxDepth, bool& promptFlag, bool& historyPattern, std::mutex& Mutex4Low) {
    
    std::set<std::string> uniqueDirectories;
    for (const auto& filePath : imageFiles) {
        std::filesystem::path path(filePath);
        if (path.has_parent_path()) {
            uniqueDirectories.insert(path.parent_path().string());
        }
    }

    // Concatenate unique directory paths with ';'
    std::string result = std::accumulate(uniqueDirectories.begin(), uniqueDirectories.end(), std::string(), [](const std::string& a, const std::string& b) {
        return a.empty() ? b : a + ";" + b;
    });

    // Get the real user ID and group ID (of the user who invoked sudo)
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    
    getRealUserId(real_uid, real_gid, real_username, real_groupname, failedOuts, Mutex4Low);

    // Iterate over each image file
    for (const std::string& inputPath : imageFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);

        // Check if the input file exists
        if (!std::filesystem::exists(inputPath)) {
            std::string failedMessage = "\033[1;91mThe specified input file \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m does not exist anymore.\033[0;1m";
            {
                std::lock_guard<std::mutex> lock(Mutex4Low);
                failedOuts.insert(failedMessage);
            }
            continue;
        }

        // Attempt to open the file to check readability
        std::ifstream file(inputPath);
        if (!file.good()) {
            std::string failedMessage = "\033[1;91mThe specified file \033[1;93m'" + inputPath + "'\033[1;91m cannot be read. Check permissions.\033[0;1m";
            {
                std::lock_guard<std::mutex> lock(Mutex4Low);
                failedOuts.insert(failedMessage);
            }
            continue;
        }

        // Determine output ISO file path
        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

        // Skip if ISO file already exists
        if (fileExists(outputPath)) {
            std::string skipMessage = "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m";
            std::lock_guard<std::mutex> lock(Mutex4Low);
            skippedOuts.insert(skipMessage);
            continue;
        }

        // Perform the conversion based on the mode
        bool conversionSuccess = false;
        if (modeMdf) {
            conversionSuccess = convertMdfToIso(inputPath, outputPath);
        } else if (!modeMdf && !modeNrg) {
            conversionSuccess = convertCcdToIso(inputPath, outputPath);
        } else if (modeNrg) {
            conversionSuccess = convertNrgToIso(inputPath, outputPath);
        }

        // Handle output results
        auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);

        if (conversionSuccess) {
            // Change ownership if needed
            if (chown(outputPath.c_str(), real_uid, real_gid) != 0) {
                std::string errorMessage = "\033[1;91mFailed to change ownership of \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m: " + strerror(errno) + "\033[0;1m";
                std::lock_guard<std::mutex> lock(Mutex4Low);
                failedOuts.insert(errorMessage);
            }

            std::string successMessage = "\033[1mImage file converted to ISO:\033[0;1m \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m";
            std::lock_guard<std::mutex> lock(Mutex4Low);
            successOuts.insert(successMessage);
        } else {
            std::string failedMessage = "\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m failed.\033[0;1m";
            std::lock_guard<std::mutex> lock(Mutex4Low);
            failedOuts.insert(failedMessage);

            if (std::remove(outputPath.c_str()) == 0) {
                std::string deletedMessage = "\033[1;92mDeleted incomplete ISO file:\033[1;91m '" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m";
                deletedOuts.insert(deletedMessage);
            } else if (!modeNrg){
                std::string deleteFailMessage = "\033[1;91mFailed to delete incomplete ISO file: \033[1;93m'" + outputPath + "'\033[0;1m";
                deletedOuts.insert(deleteFailMessage);
            }
        }
    }

    promptFlag = false;
    maxDepth = 0;
    if (!successOuts.empty()) {
        manualRefreshCache(result, promptFlag, maxDepth, historyPattern);
    }

    promptFlag = true;
    maxDepth = -1;
}


// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.

// MDF2ISO

bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath) {
    std::ifstream mdfFile(mdfPath, std::ios::binary);
    std::ofstream isoFile(isoPath, std::ios::binary);

    if (!mdfFile.is_open() || !isoFile.is_open()) {
        return false;
    }

    size_t seek_ecc = 0, sector_size = 0, seek_head = 0, sector_data = 0;
    char buf[12];

    // Check if file is valid MDF
    mdfFile.seekg(32768);
    if (!mdfFile.read(buf, 8) || std::memcmp("CD001", buf + 1, 5) == 0) {
        return false; // Not an MDF file or unsupported format
    }

    mdfFile.seekg(0);
    if (!mdfFile.read(buf, 12)) {
        return false;
    }

    // Determine MDF type based on sync patterns
    if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
        mdfFile.seekg(2352);
        if (!mdfFile.read(buf, 12)) {
            return false;
        }

        if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
            seek_ecc = 288;
            sector_size = 2352;
            sector_data = 2048;
            seek_head = 16;
        } else {
            seek_ecc = 384;
            sector_size = 2448;
            sector_data = 2048;
            seek_head = 16;
        }
    } else {
        seek_head = 0;
        sector_size = 2448;
        seek_ecc = 96;
        sector_data = 2352;
    }

    // Calculate the number of sectors
    mdfFile.seekg(0, std::ios::end);
    size_t source_length = static_cast<size_t>(mdfFile.tellg()) / sector_size;
    mdfFile.seekg(0, std::ios::beg);

    // Pre-allocate the ISO file to optimize file write performance
    isoFile.seekp(source_length * sector_data);
    isoFile.put(0); // Write a dummy byte to allocate space
    isoFile.seekp(0); // Reset the pointer to the beginning of the file

    // Buffer for reading and writing
    const size_t bufferSize = 8 * 1024 * 1024; // 8 MB
    std::vector<char> buffer(bufferSize);
    size_t bufferIndex = 0;

    while (source_length > 0) {
        // Read sector data
        mdfFile.seekg(static_cast<std::streamoff>(seek_head), std::ios::cur);
        if (!mdfFile.read(buffer.data() + bufferIndex, sector_data)) {
            return false;
        }
        mdfFile.seekg(static_cast<std::streamoff>(seek_ecc), std::ios::cur);

        bufferIndex += sector_data;

        // Write full buffer if filled
        if (bufferIndex >= bufferSize) {
            if (!isoFile.write(buffer.data(), bufferIndex)) {
                return false;
            }
            bufferIndex = 0;
        }

        --source_length;
    }

    // Write any remaining data in the buffer
    if (bufferIndex > 0) {
        if (!isoFile.write(buffer.data(), bufferIndex)) {
            return false;
        }
    }

    return true;
}


// CCD2ISO

const size_t DATA_SIZE = 2048;
const size_t BUFFER_SIZE = 8 * 1024 * 1024;  // 8 MB buffer

struct __attribute__((packed)) CcdSectheaderSyn {
    uint8_t data[12];
};

struct __attribute__((packed)) CcdSectheaderHeader {
    uint8_t sectaddr_min, sectaddr_sec, sectaddr_frac;
    uint8_t mode;
};

struct __attribute__((packed)) CcdSectheader {
    CcdSectheaderSyn syn;
    CcdSectheaderHeader header;
};

struct __attribute__((packed)) CcdSector {
    CcdSectheader sectheader;
    union {
        struct {
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t unused[8];
            uint8_t ecc[276];
        } mode1;
        struct {
            uint8_t sectsubheader[8];
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t ecc[276];
        } mode2;
    } content;
};


bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath) {
    std::ifstream ccdFile(ccdPath, std::ios::binary | std::ios::ate);
    if (!ccdFile) return false;

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;

    size_t fileSize = ccdFile.tellg();
    ccdFile.seekg(0, std::ios::beg);

    // Pre-allocate output file to reduce fragmentation
    isoFile.seekp(fileSize / sizeof(CcdSector) * DATA_SIZE); // Preallocate based on expected data size
    isoFile.put(0);  // Write a dummy byte to allocate space
    isoFile.seekp(0); // Reset the pointer to the beginning

    std::vector<uint8_t> buffer(BUFFER_SIZE);
    size_t bufferPos = 0;
    CcdSector sector;
    int sessionCount = 0;

    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        switch (sector.sectheader.header.mode) {
            case 1:
            case 2: {
                const uint8_t* sectorData = (sector.sectheader.header.mode == 1) 
                    ? sector.content.mode1.data 
                    : sector.content.mode2.data;

                if (bufferPos + DATA_SIZE > BUFFER_SIZE) {
                    isoFile.write(reinterpret_cast<char*>(buffer.data()), bufferPos);
                    bufferPos = 0;
                }

                // Use faster memcpy
                std::memcpy(&buffer[bufferPos], sectorData, DATA_SIZE);
                bufferPos += DATA_SIZE;
                break;
            }
            case 0xe2:
                sessionCount++;
                break;
            default:
                return false;
        }
    }

    // Flush remaining data
    if (bufferPos > 0) {
        isoFile.write(reinterpret_cast<char*>(buffer.data()), bufferPos);
    }

    return true;
}


// NRG2ISO

bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream nrgFile(inputFile, std::ios::binary | std::ios::ate);  // Open for reading, with positioning at the end
    if (!nrgFile) {
        return false;
    }

    std::streamsize nrgFileSize = nrgFile.tellg();  // Get the size of the input file
    nrgFile.seekg(0, std::ios::beg);  // Rewind to the beginning of the file

    // Check if the file is already in ISO format
    nrgFile.seekg(16 * 2048);
    char isoBuf[8];
    nrgFile.read(isoBuf, 8);
    
    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
        return false;  // Already an ISO, no conversion needed
    }

    // Reopen file for conversion to avoid multiple seekg operations
    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);  // Skipping the header section

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }

    // Preallocate output file by setting its size
    isoFile.seekp(nrgFileSize - 1);  // Move the write pointer to the end of the file
    isoFile.write("", 1);  // Write a dummy byte to allocate space
    isoFile.seekp(0, std::ios::beg);  // Reset write pointer to the beginning of the file

    // Buffer for reading and writing
    constexpr size_t BUFFER_SIZE = 8 * 1024 * 1024;  // 8MB buffer
    std::vector<char> buffer(BUFFER_SIZE);

    // Read and write in chunks
    while (nrgFile) {
        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            isoFile.write(buffer.data(), bytesRead);
        }
    }

    return true;
}
