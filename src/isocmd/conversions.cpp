// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"
#include "../mdf.h"
#include "../ccd.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
static std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here


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
                             "\001\033[1;94m files and import into \001\033[1;93m\002RAM\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), \001\033[1;92m\002ls \001\033[1;94m\002↵ to open \001\033[1;93m\002RAM\001\033[1;94m\002 cache, "
                             "\001\033[1;93m\002clr\001\033[1;94m\002 ↵ to clear \001\033[1;93m\002RAM\001\033[1;94m\002 cache, ↵ to return:\n\001\033[0;1m\002";

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
    // Bind keys for preventing clear screen and enabling tab completion
    rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
    rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
    
    // Containers to track file processing results
    std::set<std::string> processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts;
    
    bool isFiltered = false; // Indicates if the file list is currently filtered
    std::string fileExtension = (fileType == "bin" || fileType == "img") ? ".bin/.img" 
                                   : (fileType == "mdf") ? ".mdf" : ".nrg"; // Determine file extension based on type
    std::string filterPrompt; // Stores the prompt for filter input
    
    std::string fileExtensionWithOutDots;
    
    for (char c : fileExtension) {
        if (c != '.') {
            fileExtensionWithOutDots += c;
        }
    }
    
    // Lambda function for filtering the file list
    auto filterQuery = [&files, &historyPattern, &fileType, &filterPrompt]() {
        while (true) {
            clear_history(); // Clear the input history
            historyPattern = true;
            loadHistory(historyPattern); // Load input history if available

            // Prompt the user for a search query
            std::unique_ptr<char, decltype(&std::free)> rawSearchQuery(readline(filterPrompt.c_str()), &std::free);
            std::string inputSearch(rawSearchQuery.get());

            // Exit the filter loop if input is empty or "/"
            if (inputSearch.empty() || inputSearch == "/") break;

            // Filter files based on the input search query
            auto filteredFiles = filterFiles(files, inputSearch);
            if (filteredFiles.empty()) continue; // Skip if no files match the filter

            // Save the search query to history and update the file list
            add_history(rawSearchQuery.get());
            saveHistory(historyPattern);
            historyPattern = false;
            clear_history(); // Clear history to reset for future inputs
            files = filteredFiles; // Update the file list with the filtered results
            break;
        }
    };

    // Main processing loop
    while (true) {
        verbose = false; // Reset verbose mode
        processedErrors.clear(); 
        successOuts.clear(); 
        skippedOuts.clear(); 
        failedOuts.clear(); 
        deletedOuts.clear();
        clear_history();	

        clearScrollBuffer(); // Clear the screen for new content
        sortFilesCaseInsensitive(files); // Sort the files case-insensitively
        printList(files, "IMAGE_FILES"); // Print the current list of files

        // Build the user prompt string dynamically
        std::string prompt = std::string("\n") + 
                     (isFiltered ? "\001\033[1;96m\002Filtered \001\033[1;38;5;208m\002" : "\001\033[1;38;5;208m\002") + 
                     (fileType == "bin" || fileType == "img" ? "BIN/IMG" : (fileType == "mdf" ? "MDF" : "NRG")) + 
                     "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";
                     
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        std::string mainInputString(rawInput.get());
        
        if (mainInputString == "?") {
            help();
            continue;
        }

        // Handle user input for toggling the full list display
        if (mainInputString == "~") {
            toggleFullList = !toggleFullList;
            clearScrollBuffer();
            printList(files, "IMAGE_FILES");
            continue;
        }

        // Handle input for returning to the unfiltered list or exiting
        if (std::isspace(rawInput.get()[0]) || rawInput.get()[0] == '\0') {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore the original file list
                files = (fileType == "bin" || fileType == "img") ? binImgFilesCache :
                        (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);
                isFiltered = false; // Reset filter status
                continue;
            } else {
                break; // Exit the loop if no input
            }
        }

        // Handle filter command
        if (strcmp(rawInput.get(), "/") == 0) {
            filterPrompt = "\001\033[1A\002\001\033[K\002\001\033[1A\002\001\033[K\002\n\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" + fileExtensionWithOutDots + "\001\033[1;94m\002 (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ to return: \001\033[0;1m\002";
            filterQuery(); // Call the filter query function
            isFiltered = files.size() != (fileType == "bin" || fileType == "img" ? binImgFilesCache.size() : (fileType == "mdf" ? mdfMdsFilesCache.size() : nrgFilesCache.size()));
        } else {
            // Process other input commands for file processing
            clearScrollBuffer();
            std::cout << "\n\033[0;1m";
            processInput(mainInputString, files, (fileType == "mdf"), (fileType == "nrg"), processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, promptFlag, maxDepth, historyPattern, verbose);
            if (verbose) verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, 3); // Print detailed logs if verbose mode is enabled
        }
    }
}

// Function type definition for ProgressBarFunction
using ProgressBarFunction = void(*)(std::atomic<size_t>*, size_t, std::atomic<bool>*, bool*);

// Function to process user input and convert selected BIN/MDF/NRG files to ISO format
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
    
    std::set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;

    std::set<int> processedIndices;
    tokenizeInput(input, fileList, processedErrors, processedIndices);
    
    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n\033[1;91mNo valid indices for conversion.\033[1;91m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        clear_history();
        return;
    }

    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), 
        std::thread::hardware_concurrency());
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
    size_t totalBytes = 0;
    size_t totalTasks = filesToProcess.size();  // Each file is a task

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
    } else {
        for (const auto& file : filesToProcess) {
            std::ifstream ccdFile(file, std::ios::binary | std::ios::ate);
            if (ccdFile) {
                size_t fileSize = ccdFile.tellg();
                totalBytes += (fileSize / sizeof(CcdSector)) * DATA_SIZE;
            }
        }
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Use the enhanced progress bar with task tracking
    std::thread progressThread(displayProgressBarSize, &completedBytes, 
        totalBytes, &completedTasks, totalTasks, &isProcessingComplete, &verbose);

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
            &fileList, &successOuts, &skippedOuts, &failedOuts, &deletedOuts, 
            modeMdf, modeNrg, &maxDepth, &promptFlag, &historyPattern, 
            &completedBytes, &completedTasks]() {
            // Process each file with task tracking
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts, 
                deletedOuts, modeMdf, modeNrg, maxDepth, promptFlag, historyPattern, 
                &completedBytes, &completedTasks);
        }));
    }

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


// Function to search for .bin .img .nrg and mdf files
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


// Function to convert a BIN/IMG/MDF/NRG file to ISO format
void convertToISO(const std::vector<std::string>& imageFiles, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, const bool& modeMdf, const bool& modeNrg, int& maxDepth, bool& promptFlag, bool& historyPattern, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks) {
    
    // Collect unique directories from the input file paths
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
    
    getRealUserId(real_uid, real_gid, real_username, real_groupname, failedOuts);

    // Iterate over each image file
    for (const std::string& inputPath : imageFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);

        // Check if the input file exists
        if (!std::filesystem::exists(inputPath)) {
            std::string failedMessage = "\033[1;91mThe specified input file \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m does not exist anymore.\033[0;1m";
            failedOuts.insert(failedMessage);
            continue;
        }

        // Attempt to open the file to check readability
        std::ifstream file(inputPath);
        if (!file.good()) {
            std::string failedMessage = "\033[1;91mThe specified file \033[1;93m'" + inputPath + "'\033[1;91m cannot be read. Check permissions.\033[0;1m";
            failedOuts.insert(failedMessage);
            continue;
        }

        // Determine output ISO file path
        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

        // Skip if ISO file already exists
        if (fileExists(outputPath)) {
            std::string skipMessage = "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m";
            skippedOuts.insert(skipMessage);
            if (completedTasks) {
                (*completedTasks)++; // Count skipped files as completed tasks
            }
            continue;
        }

        // Perform the conversion based on the mode
        bool conversionSuccess = false;
        if (modeMdf) {
            conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        } else if (!modeMdf && !modeNrg) {
            conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);
        } else if (modeNrg) {
            conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        }

        // Handle output results
        auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);

        if (conversionSuccess) {
            // Change ownership if needed
            if (chown(outputPath.c_str(), real_uid, real_gid) != 0) {
                std::string errorMessage = "\033[1;91mFailed to change ownership of \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m: " + strerror(errno) + "\033[0;1m";
                failedOuts.insert(errorMessage);
            }

            std::string successMessage = "\033[1mImage file converted to ISO:\033[0;1m \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m";
            successOuts.insert(successMessage);
            
            if (completedTasks) {
                (*completedTasks)++; // Increment completed tasks counter for successful conversions
            }
        } else {
            std::string failedMessage = "\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m failed.\033[0;1m";
            failedOuts.insert(failedMessage);

            if (std::remove(outputPath.c_str()) == 0) {
                std::string deletedMessage = "\033[1;92mDeleted incomplete ISO file:\033[1;91m '" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m";
                deletedOuts.insert(deletedMessage);
            } else if (!modeNrg) {
                std::string deleteFailMessage = "\033[1;91mFailed to delete incomplete ISO file: \033[1;93m'" + outputPath + "'\033[0;1m";
                deletedOuts.insert(deleteFailMessage);
            }
            if (completedTasks) {
                (*completedTasks)++; // Increment completed tasks counter for failed conversions
            }
        }
    }

    // Update cache and prompt flags
    promptFlag = false;
    maxDepth = 0;
    if (!successOuts.empty()) {
        manualRefreshCache(result, promptFlag, maxDepth, historyPattern);
    }

    promptFlag = true;
    maxDepth = -1;
}

