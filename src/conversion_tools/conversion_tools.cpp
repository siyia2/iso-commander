#include "../headers.h"
#include "../threadpool.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here

// Boolean flags for verbose beautification
bool gapSet = true;
bool gapUniqueErrors = false;

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
    verbose = false;
}


// Function to print invalid directory paths from search
void verboseFind(std::set<std::string>& invalidDirectoryPaths) {
	if (!invalidDirectoryPaths.empty()) {
		if (gapSet) {
			std::cout << "\n";
		}
		if (gapSet) {
			std::cout << "\n\033[0;1mInvalid path(s) omitted from search: \033[1:91m";
		} else {
			std::cout << "\033[0;1mInvalid path(s) omitted from search: \033[1:91m";
		}
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
    }	
		
		std::cerr << "\033[0;1m.\n"; // Print a newline at the end
		invalidDirectoryPaths.clear();
	}
}


// Function to select and convert files based on user's choice of file type
void select_and_convert_files_to_iso(const std::string& fileTypeChoice) {
    std::vector<std::string> files;
    files.reserve(100);
    std::vector<std::string> directoryPaths;
    std::set<std::string> uniquePaths, processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, invalidDirectoryPaths;

    std::string fileExtension, fileTypeName, fileType = fileTypeChoice;
    bool modeMdf = (fileType == "mdf");

    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        fileTypeName = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileTypeName = "MDF";
    } else {
        std::cout << "Invalid file type choice. Supported types: BIN/IMG, MDF\n";
        return;
    }

    while (true) {
        bool list = false, clr = false;
        successOuts.clear(); skippedOuts.clear(); failedOuts.clear(); deletedOuts.clear(); processedErrors.clear();
        historyPattern = false;
        loadHistory();

        std::string prompt = "\001\033[1;92m\002Folder path(s)\001\033[1;94m ↵ to scan for \001\033[1;38;5;208m\002" + fileExtension +
                             "\001\033[1;94m files and import into \001\033[1;93m\002RAM\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), \001\033[1;92m\002ls \001\033[1;94m\002↵ open \001\033[1;93m\002RAM\001\033[1;94m\002 cache, "
                             "\001\033[1;93m\002clr\001\033[1;94m\002 ↵ clear \001\033[1;93m\002RAM\001\033[1;94m\002 cache, ↵ return:\n\001\033[0;1m\002";

        char* rawinput = readline(prompt.c_str());
        std::unique_ptr<char, decltype(&std::free)> mainSearch(rawinput, &std::free);
        std::string inputSearch(mainSearch.get());

        if (std::isspace(mainSearch.get()[0]) || mainSearch.get()[0] == '\0') {
            break;
        }

        list = (inputSearch == "ls");
        clr = (inputSearch == "clr");

        if (clr) {
            // Clear all relevant data structures
            files.clear();
            directoryPaths.clear();
            uniquePaths.clear();
            invalidDirectoryPaths.clear();
            
            if (!modeMdf) {
                binImgFilesCache.clear();
                std::cout << "\n\033[1;92mBIN/IMG RAM cache cleared.\033[0;1m\n";
            } else {
                mdfMdsFilesCache.clear();
                std::cout << "\n\033[1;92mMDF RAM cache cleared.\033[0;1m\n";
            }
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        if (!inputSearch.empty() && !list && !clr) {
            std::cout << " " << std::endl;
            add_history(mainSearch.get());
            saveHistory();
        }

        clear_history();

        auto start_time = std::chrono::high_resolution_clock::now();
        directoryPaths.clear();
        uniquePaths.clear();
        invalidDirectoryPaths.clear();

        if (!list) {
            std::istringstream iss(inputSearch);
            std::string path;
            while (std::getline(iss, path, ';')) {
                size_t start = path.find_first_not_of(" \t");
                size_t end = path.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    std::string cleanedPath = path.substr(start, end - start + 1);
                    if (uniquePaths.find(cleanedPath) == uniquePaths.end()) {
                        if (directoryExists(cleanedPath)) {
                            directoryPaths.push_back(cleanedPath);
                            uniquePaths.insert(cleanedPath);
                        } else {
                            invalidDirectoryPaths.insert("\033[1;91m" + cleanedPath);
                        }
                    }
                }
            }
        }

        if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
            std::cout << "\033[1;91mNo valid path(s) provided.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        bool newFilesFound = false;
        if (!list) {
            files = findFiles(directoryPaths, fileType, [&](const std::string&, const std::string&) {
                newFilesFound = true;
            }, invalidDirectoryPaths, processedErrors, list);
        }

        if (!newFilesFound && !files.empty() && !list) {
            std::cout << "\n";
            verboseFind(invalidDirectoryPaths);
            auto end_time = std::chrono::high_resolution_clock::now();
            if (gapSet) {
				std::cout << "\n";
			}
			if (gapUniqueErrors && !gapSet) {
				std::cout << "\n";
			}
            std::cout << "\033[1;91mNo new " << fileExtension << " file(s) over 5MB found. \033[1;92m" << files.size() << " file(s) are cached in RAM from previous searches.\033[0;1m\n\n";
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
            std::cout << "\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        if (files.empty() && !list) {
            std::cout << "\n";
            verboseFind(invalidDirectoryPaths);
            auto end_time = std::chrono::high_resolution_clock::now();
            if (gapSet) {
				std::cout << "\n";
			}
			if (gapUniqueErrors && !gapSet) {
				std::cout << "\n";
			}
            std::cout << "\033[1;91mNo " << fileExtension << " file(s) over 5MB found in the specified path(s) or cached in RAM.\n\033[0;1m\n";
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
            std::cout << "\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        while (true) {
            successOuts.clear(); skippedOuts.clear(); failedOuts.clear(); deletedOuts.clear(); processedErrors.clear();

            if (files.empty()) {
                std::cout << "\n\033[1;93mNo " << fileExtension << " files stored in RAM cache for potential conversions.\033[1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                break;
            }

            clearScrollBuffer();
            std::cout << "\033[92;1m// SUCCESSFUL CONVERSIONS ARE AUTOMATICALLY IMPORTED INTO ISO CACHE //\033[0;1m\033[0;1m\n\n";
            sortFilesCaseInsensitive(files);
            printFileList(files);

            clear_history();
            std::string prompt = "\n\001\033[1;38;5;208m\002" + fileTypeName + " \001\033[1;94m\002file(s) ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
            char* rawInput = readline(prompt.c_str());
            std::unique_ptr<char, decltype(&std::free)> input(rawInput, &std::free);
            std::string mainInputString(input.get());

            if (std::isspace(input.get()[0]) || input.get()[0] == '\0') {
                clearScrollBuffer();
                break;
            }
			bool search = true;
            if (strcmp(input.get(), "/") == 0) {
                while (search) {
					historyPattern = true;
                    clear_history();
                    loadHistory();
                    clearScrollBuffer();

                    std::string filterPrompt = "\n\001\033[1;92m\002Term(s)\001\033[1;94m\002 ↵ to filter \001\033[1;38;5;208m\002" + fileTypeName + "\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";
                    char* rawSearchQuery = readline(filterPrompt.c_str());
                    std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
                    std::string inputSearch(searchQuery.get());

                    clearScrollBuffer();

                    if (searchQuery && searchQuery.get()[0] != '\0') {
                        std::cout << "\033[1mPlease wait...\033[1m\n";
                        if (strcmp(searchQuery.get(), "/") != 0) {
                            add_history(searchQuery.get());
                            saveHistory();
                        }
                    }
                    clear_history();

                    if (searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                        break;
                    }

                    std::vector<std::string> filteredFiles = filterFiles(files, inputSearch);

                    if (filteredFiles.empty()) {
                        clearScrollBuffer();
                        std::cout << "\n\033[1;91mNo matches found.\033[0;1m\n";
                        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    } else {
                        while (true) {
                            successOuts.clear(); skippedOuts.clear(); failedOuts.clear(); deletedOuts.clear(); processedErrors.clear();
                            clearScrollBuffer();
                            clear_history();
                            sortFilesCaseInsensitive(filteredFiles);
                            std::cout << "\033[1mFiltered results:\n\033[0;1m\n";
                            printFileList(filteredFiles);

                            std::string filterPrompt = "\n\001\033[1;94m\033[1;38;5;208m\002Filtered " + fileTypeName + "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
                            char* rawfilterPrompt = readline(filterPrompt.c_str());
                            std::unique_ptr<char, decltype(&std::free)> filterInput(rawfilterPrompt, &std::free);
                            std::string filterInputString(filterInput.get());

                            if (filterInput.get()[0] == '/') {
								search = true;
                                break;
                            }

                            if (std::isspace(filterInput.get()[0]) || filterInput.get()[0] == '\0') {
								search = false;
								historyPattern =false;
                                break;
                            }

                            clearScrollBuffer();
                            std::cout << "\033[1mPlease wait..." << std::endl;
                            processInput(filterInputString, filteredFiles, modeMdf, processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts);

                            clearScrollBuffer();
                            std::cout << "\n";
                            if (verbose) {
                                verboseConversion(processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts);
                            }
                            if (!processedErrors.empty() && successOuts.empty() && skippedOuts.empty() && failedOuts.empty() && deletedOuts.empty()) {
                                clearScrollBuffer();
                                verbose = false;
                                std::cout << "\n\033[1;91mNo valid input provided for ISO conversion.\033[0;1m";
                                std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
                                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                            }
                        }
                    }
                }
            } else {
                clearScrollBuffer();
                std::cout << "\033[1mPlease wait..." << std::endl;
                processInput(mainInputString, files, modeMdf, processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts);

                clearScrollBuffer();
                std::cout << "\n";
                if (verbose) {
                    verboseConversion(processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts);
                }
                if (!processedErrors.empty() && successOuts.empty() && skippedOuts.empty() && failedOuts.empty() && deletedOuts.empty()) {
                    clearScrollBuffer();
                    verbose = false;
                    std::cout << "\n\033[1;91mNo valid input provided for ISO conversion.\033[0;1m";
                    std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                }
            }
        }
    }
}


// Function to process user input and convert selected BIN/MDF files to ISO format
void processInput(const std::string& input, const std::vector<std::string>& fileList, bool modeMdf, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts) {
    std::mutex futuresMutex;
    std::set<int> processedIndices;

    // Step 1: Tokenize the input to determine the number of threads to use
    std::istringstream issCount(input);
    std::set<std::string> tokens;
    std::string tokenCount;

    while (issCount >> tokenCount && tokens.size() < maxThreads) {
		if (tokenCount[0] == '-' || startsWithZero(tokenCount)) continue;

    // Count the number of hyphens
    size_t hyphenCount = std::count(tokenCount.begin(), tokenCount.end(), '-');

    // Skip if there's more than one hyphen
    if (hyphenCount > 1) continue;

    size_t dashPos = tokenCount.find('-');
    if (dashPos != std::string::npos) {
        std::string start = tokenCount.substr(0, dashPos);
        std::string end = tokenCount.substr(dashPos + 1);
        if (std::all_of(start.begin(), start.end(), ::isdigit) &&
            std::all_of(end.begin(), end.end(), ::isdigit)) {
            int startNum = std::stoi(start);
            int endNum = std::stoi(end);
            if (static_cast<std::vector<std::string>::size_type>(startNum) <= fileList.size() &&
                static_cast<std::vector<std::string>::size_type>(endNum) <= fileList.size()) {
                int step = (startNum <= endNum) ? 1 : -1;
                for (int i = startNum; step > 0 ? i <= endNum : i >= endNum; i += step) {
                    if (i != 0) {
                        tokens.emplace(std::to_string(i));
                    }
                    if (tokens.size() >= maxThreads) {
                        break;
                    }
                }
            }
        }
    } else if (!tokenCount.empty() && std::all_of(tokenCount.begin(), tokenCount.end(), ::isdigit)) {
		int num = std::stoi(tokenCount);
        if (num > 0 && static_cast<std::vector<std::string>::size_type>(num) <= fileList.size()) {
            tokens.emplace(tokenCount);
				if (tokens.size() >= maxThreads) {
					break;
				}
			}
		}
	}

    unsigned int numThreads = std::min(static_cast<int>(tokens.size()), static_cast<int>(maxThreads));
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);
    ThreadPool pool(numThreads);

	std::atomic<int> completedTasks(0);
	// Create atomic flag for completion status
    std::atomic<bool> isComplete(false);

    std::set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;
    auto asyncConvertToISO = [&](const std::string& selectedFile) {
        std::size_t found = selectedFile.find_last_of("/\\");
        std::string filePath = selectedFile.substr(0, found);
        selectedFilePaths.emplace(filePath);
        convertToISO(selectedFile, successOuts, skippedOuts, failedOuts, deletedOuts, modeMdf);
        // Increment completedTasks when conversion is done
        ++completedTasks;
    };

    std::istringstream iss(input);
    std::string token;

    while (iss >> token) {
		if (startsWithZero(token)) {
			processedErrors.emplace("\033[1;91mInvalid index: '0'.\033[0;1m"); 
			continue; 
        }
        std::istringstream tokenStream(token);
        int start, end;
        char dash;
        if (tokenStream >> start) {
            if (tokenStream >> dash && dash == '-') {
                if (tokenStream >> end) {
                    char extraChar;
                    if (tokenStream >> extraChar) {
                        // Extra characters found after the range, treat as invalid
                        processedErrors.emplace("\033[1;91mInvalid range: '" + token + "'.\033[1;0m");
                    } else if (start > 0 && end > 0 && start <= static_cast<int>(fileList.size()) && end <= static_cast<int>(fileList.size())) {
                        int step = (start <= end) ? 1 : -1;
                        for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                            int selectedIndex = i - 1;
								if (processedIndices.find(selectedIndex) == processedIndices.end()) {
										std::string selectedFile = fileList[selectedIndex];
										{
											std::lock_guard<std::mutex> lock(futuresMutex);
											futures.push_back(pool.enqueue(asyncConvertToISO, selectedFile));

											processedIndices.emplace(selectedIndex);
										}
                            }
                        }
                    } else {
						if (start < 0) {
							processedErrors.emplace("\033[1;91mInvalid input: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[1;0m");
						} else {
							processedErrors.emplace("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[1;0m");
						}
                    }
                } else {
                    processedErrors.emplace("\033[1;91mInvalid range: '" + token + "'.\033[1;0m");
                }
            } else if (start >= 1 && static_cast<size_t>(start) <= fileList.size()) {
                int selectedIndex = start - 1;
					if (processedIndices.find(selectedIndex) == processedIndices.end()) {
							std::string selectedFile = fileList[selectedIndex];
							{
								std::lock_guard<std::mutex> lock(futuresMutex);
								futures.push_back(pool.enqueue(asyncConvertToISO, selectedFile));

								processedIndices.emplace(selectedIndex);
							}
                }
            } else {
                processedErrors.emplace("\033[1;91mInvalid index: '" + std::to_string(start) + "'.\033[1;0m");
            }
        } else {
            processedErrors.emplace("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
        }
    }

    if (!processedIndices.empty()) {
    // Launch progress bar display in a separate thread
    int totalTasks = processedIndices.size();  // Total number of tasks to complete
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), totalTasks, std::ref(isComplete));

    for (auto& future : futures) {
        future.wait();
    }
    // Signal the progress bar thread to stop
    isComplete = true;

    // Join the progress bar thread
    progressThread.join();
	} else {
		// No need to display progress bar or wait for futures
		isComplete = true;
	}

    concatenatedFilePaths.clear();
    for (const auto& path : selectedFilePaths) {
		concatenatedFilePaths += path + ";";
    }
    if (!concatenatedFilePaths.empty()) {
		concatenatedFilePaths.pop_back();
    }

    promptFlag = false;
    if (!processedIndices.empty()) {
        maxDepth = 0;
        manualRefreshCache(concatenatedFilePaths);
    }
    maxDepth = -1;
}


// Function to search for .bin and .img files over 5MB
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrors, bool list) {
	

    // Vector to store permission errors
    std::set<std::string> uniqueInvalidPaths;
    
    // Return early if list mode is enabled
    if (list && mode == "bin") {
		return binImgFilesCache;
	} else if (list && mode == "mdf") {
		return mdfMdsFilesCache;
	}


    std::mutex fileCheckMutex;

    bool blacklistMdf =false;

    // Vector to store file names that match the criteria
    std::set<std::string> fileNames;

    // Mutex to ensure thread safety
    std::mutex mutex4search;

    // Start the timer
   auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Preallocate enough space for the futures vector
        size_t totalFiles = 0;
        for (const auto& path : paths) {
			try {
				for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
					if (entry.is_regular_file()) {
						totalFiles++;
						std::cout << "\rTotal files processed: " << totalFiles << std::flush;
					}
				}
			} catch (const std::filesystem::filesystem_error& e) {
				std::string exception = "\033[1;91mError accessing path: " + path + " - " + e.what() + "\033[0;1m";
				gapSet = false;
				processedErrors.insert(exception);
				uniqueInvalidPaths.insert(path);
				
			}
		}

		if (!processedErrors.empty()) {
			std::cout << "\n\n";
			for (const auto& processedError : processedErrors) {
				std::cout << processedError << std::endl;
			}
			processedErrors.clear();
		}

        std::vector<std::future<void>> futures;
        futures.reserve(totalFiles);

        // Counter to track the number of ongoing tasks
        unsigned int numOngoingTasks = 0;

        // Iterate through input paths
		for (const auto& path : paths) {
			

            try {
                // Use a lambda function to process files asynchronously
                auto processFileAsync = [&](const std::filesystem::directory_entry& entry) {
                    std::string fileName = entry.path().string();
                    std::string filePath = entry.path().parent_path().string();  // Get the path of the directory

                    // Call the callback function to inform about the found file
                    callback(fileName, filePath);

                    // Lock the mutex to ensure safe access to shared data (fileNames and numOngoingTasks)
                    std::lock_guard<std::mutex> lock(mutex4search);

                    // Add the file name to the shared data
                    fileNames.insert(fileName);

                    // Decrement the ongoing tasks counter
                    --numOngoingTasks;
                };

                // Use async to process files concurrently
                // Iterate through files in the given directory and its subdirectories
                if (mode == "bin") {
					blacklistMdf = false;
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                        if (entry.is_regular_file()) {
                            // Checks .bin .img blacklist
                            if (blacklist(entry, blacklistMdf)) {
                                // Check if the file is already present in the cache to avoid duplicates
                                std::string fileName = entry.path().string();
                                if (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) == binImgFilesCache.end()) {
                                    // Process the file asynchronously
                                    if (numOngoingTasks < maxThreads) {
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    } else {
                                        // Wait for one of the ongoing tasks to complete before adding a new task
                                        for (auto& future : futures) {
                                            if (future.valid()) {
                                                future.get();
                                            }
                                        }
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    }
                                }
                            }
                        }
                    }
                } else {
                    blacklistMdf = true;

                    // Iterate through files in the given directory and its subdirectories
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                        if (entry.is_regular_file()) {
                            if (blacklist(entry, blacklistMdf)) {
                                // Check if the file is already present in the cache to avoid duplicates
                                std::string fileName = entry.path().string();
                                if (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) == mdfMdsFilesCache.end()) {
                                    // Process the file asynchronously
                                    if (numOngoingTasks < maxThreads) {
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    } else {
                                        // Wait for one of the ongoing tasks to complete before adding a new task
                                        for (auto& future : futures) {
                                            if (future.valid()) {
                                                future.get();
                                            }
                                        }
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    }
                                }
                            }
                        }
                    }

                    // Wait for remaining asynchronous tasks to complete
                    for (auto& future : futures) {
                        // Check if the future is valid
                        if (future.valid()) {
                            // Block until the future is ready
                            future.get();
                        }
                    }
                }
            } catch (const std::filesystem::filesystem_error& e) {
                std::lock_guard<std::mutex> lock(mutex4search);

                // Check if the exception is related to a permission error
                const std::error_code& ec = e.code();
                if (ec == std::errc::permission_denied) {
                    // Check if the path is unique
                    if (uniqueInvalidPaths.insert(path).second) {

                    }
                }
            }
        }

    } catch (const std::filesystem::filesystem_error& e) {
        
        // Handle filesystem errors for the overall operation
       // std::cerr << "\033[1;91m" << e.what() << ".\033[0;1m\n";
    }

	if (!uniqueInvalidPaths.empty()) {
		gapUniqueErrors = true;
	}
    // Print success message if files were found
    if (!fileNames.empty()) {

        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
			std::cout << "\n";
        if (mode == "bin") {
			verboseFind(invalidDirectoryPaths);
			if (gapSet) {
				std::cout << "\n";
			}
			if (!uniqueInvalidPaths.empty() && !gapSet) {
				std::cout << "\n";
			}
			std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)" << ".\033[1;93m " << binImgFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[0;1m\n";
		} else {
			
			verboseFind(invalidDirectoryPaths);
			if (gapSet) {
				std::cout << "\n";
			}
			if (!uniqueInvalidPaths.empty() && !gapSet) {
				std::cout << "\n";
			}
			std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)" << ".\033[1;93m " << mdfMdsFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[0;1m\n";
		}
        // Calculate and print the elapsed time
		std::cout << "\n";
		auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
		std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    std::lock_guard<std::mutex> lock(mutex4search);
    if (mode == "bin") {
        // Update the cache by appending fileNames to binImgFilesCache
        binImgFilesCache.insert(binImgFilesCache.end(), fileNames.begin(), fileNames.end());

        // Return the combined results
        return binImgFilesCache;
    }

    if (mode == "mdf") {
        // Update the cache by appending fileNames to mdfMdsFilesCache
        mdfMdsFilesCache.insert(mdfMdsFilesCache.end(), fileNames.begin(), fileNames.end());

        // Return the combined results
        return mdfMdsFilesCache;
    }

    // Return an empty vector if mode is neither "bin" nor "mdf"
    return std::vector<std::string>();
}


// Blacklist function for MDF BIN IMG
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();

    // Convert the extension to lowercase for case-insensitive comparison
    std::string extLower = ext;
    toLowerInPlace(extLower);

    // Combine extension checks
    if (!blacklistMdf) {
		if (!((extLower == ".bin" || extLower == ".img"))) {
			return false;
		}
	} else {
		if (!((extLower == ".mdf"))) {
			return false;
		}
	}

    // Check file size
    if (std::filesystem::file_size(entry) <= 5'000'000) {
        return false;
    }

    // Use a set for blacklisted keywords
    std::set<std::string> blacklistKeywords = {
      //  "block", "list", "sdcard", "index", "data", "shader", "navmesh",
      //  "obj", "terrain", "script", "history", "system", "vendor", "flora",
     //   "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
    };

    // Add blacklisted keywords for .mdf extension
    if (extLower == ".mdf") {
        blacklistKeywords.insert({
           // "flora", "terrain", "script", "history", "system", "vendor",
           // "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
        });
    }

    // Convert the filename to lowercase for additional case-insensitive comparisons
    std::string filenameLowerNoExt = filenameLower;
	filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size());

    // Check if any blacklisted word is present in the filename
    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}


// Function to print found BIN/IMG files with alternating colored sequence numbers
void printFileList(const std::vector<std::string>& fileList) {
    const char* bold = "\033[1m";
    const char* reset = "\033[0m";
    const char* red = "\033[31;1m";
    const char* green = "\033[32;1m";
    const char* orangeBold = "\033[1;38;5;208m";

    size_t maxIndex = fileList.size();
    size_t numDigits = std::to_string(maxIndex).length();

    std::ostringstream output;
    // Reserve estimated space for the string buffer
    std::string buffer;
    buffer.reserve(fileList.size() * 100);
    output.str(std::move(buffer));

    for (size_t i = 0; i < fileList.size(); ++i) {
        const auto& filename = fileList[i];
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(filename);

        const size_t dotPos = fileNameOnly.find_last_of('.');
        bool isSpecialExtension = false;

        if (dotPos != std::string::npos) {
			std::string extension = fileNameOnly.substr(dotPos);
			toLowerInPlace(extension);
			isSpecialExtension = (extension == ".bin" || extension == ".img" || extension == ".mdf");
		}

        const char* sequenceColor = (i % 2 == 0) ? red : green;

        output << (isSpecialExtension ? sequenceColor : "")
               << std::setw(numDigits) << std::right << (i + 1) << ". " << reset << bold;

        if (isSpecialExtension) {
            output << directory << '/' << orangeBold << fileNameOnly;
        } else {
            output << filename;
        }

        output << reset << "\033[0;1m\n";
    }

    std::cout << output.str();
}


// Function to convert a BIN/IMG/MDF file to ISO format
void convertToISO(const std::string& inputPath, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool modeMdf) {
    // Get current user and group
    char* current_user = getlogin();
    if (current_user == nullptr) {
        std::cerr << "\nError getting current user: " << strerror(errno) << "\033[0;1m";
        return;
    }
    gid_t current_group = getegid();
    if (current_group == static_cast<unsigned int>(-1)) {
        std::cerr << "\n\033[1;91mError getting current group:\033[0;1m " << strerror(errno) << "\033[0;1m";
        return;
    }
    std::string user_str(current_user);
    std::string group_str = std::to_string(static_cast<unsigned int>(current_group));

    auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::string failedMessage = "\033[1;91mThe specified input file \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m does not exist.\033[0;1m\n";
        {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
            failedOuts.insert(failedMessage);
        }
        return;
    }
    // Check if the corresponding .iso file already exists
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
    if (fileExists(outputPath)) {
        std::string skipMessage = "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m";
        {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
            skippedOuts.insert(skipMessage);
        }
        return;
    }
    // Escape the inputPath before using it in shell commands
    std::string escapedInputPath = shell_escape(inputPath);
    // Escape the outputPath before using it in shell commands
    std::string escapedOutputPath = shell_escape(outputPath);
    // Determine the appropriate conversion command
    std::string conversionCommand;
    if (modeMdf) {
        conversionCommand = "mdf2iso " + escapedInputPath + " " + escapedOutputPath;
        conversionCommand += " > /dev/null 2>&1";
    } else if (!modeMdf) {
        conversionCommand = "ccd2iso " + escapedInputPath + " " + escapedOutputPath;
        conversionCommand += " > /dev/null 2>&1";
    } else {
        std::string failedMessage = "\033[1;91mUnsupported file format for \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m. Conversion failed.\033[0;1m";
        {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
            failedOuts.insert(failedMessage);
        }
        return;
    }
    // Execute the conversion command
    int conversionStatus = std::system(conversionCommand.c_str());
    auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);
    // Check the result of the conversion
    if (conversionStatus == 0) {
        // Change ownership of the created ISO file
        std::string chownCommand = "chown " + user_str + ":" + group_str + " " + escapedOutputPath;
        system(chownCommand.c_str());

        std::string successMessage = "\033[1mImage file converted to ISO:\033[0;1m \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m";
        {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
            successOuts.insert(successMessage);
        }
    } else {
        std::string failedMessage = "\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m failed.\033[0;1m";
        {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
            failedOuts.insert(failedMessage);
        }
        // Delete the partially created ISO file
        if (std::remove(outputPath.c_str()) == 0) {
            std::string deletedMessage = "\033[1;92mDeleted incomplete ISO file:\033[1;91m '" + outDirectory + "/" + outFileNameOnly + "'\033[1;92m.\033[0;1m";
            {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
                deletedOuts.insert(deletedMessage);
            }
        } else {
            std::string deletedMessage = "\033[1;91mFailed to delete partially created ISO file: \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m.\033[0;1m";
            {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
                deletedOuts.insert(deletedMessage);
            }
        }
    }
}


// Function to check if a program is installed based on its name
bool isProgramInstalled(const std::string& type) {
    // Construct the command to check if the program is in the system's PATH
    std::string program;
    if (type == "mdf") {
		program = "mdf2iso";
	} else {
		program = "ccd2iso";
	}
    std::string command = "which " + shell_escape(program);

    // Execute the command and check the result
    if (std::system((command + " > /dev/null 2>&1").c_str()) == 0) {
        return true;  // Program is installed
    } else {
        return false;  // Program is not installed
    }
}
