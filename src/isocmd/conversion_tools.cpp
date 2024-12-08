// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
static std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here

// Boolean flag for verbose beautification
bool gapSet = true;
bool gapSetTotal = true;

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
		if (!gapSetTotal){
		     std::cout << "\033[2A\033[K";
		}
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
    }

		std::cerr << "\033[0;1m.\n"; // Print a newline at the end
		invalidDirectoryPaths.clear();
	}
}


// Function to apply input filtering
void applyFilter(std::vector<std::string>& files, const std::vector<std::string>& originalFiles, const std::string& fileTypeName) {
    while (true) {
		clear_history();
        historyPattern = true;
        loadHistory();
        std::string filterPrompt = "\001\033[1A\002\001\033[K\002\001\033[1A\002\001\033[K\002\n\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" + fileTypeName + "\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";

        std::unique_ptr<char, decltype(&std::free)> rawSearchQuery(readline(filterPrompt.c_str()), &std::free);
        std::string inputSearch(rawSearchQuery.get());
        if (!inputSearch.empty() && inputSearch != "/") {
            add_history(rawSearchQuery.get());
            saveHistory();
        }
        historyPattern = false;
        clear_history();
        if (inputSearch.empty() || inputSearch == "/") {
            break;
        }
        std::vector<std::string> filteredFiles = filterFiles(originalFiles, inputSearch); // Filter the original list
        if (filteredFiles.empty()) {
            std::cout << "\033[K";  // Clear the previous input line
            continue;
        }
        files = filteredFiles;
        break;
    }
}


// Function to select and convert files based on user's choice of file type
void select_and_convert_files_to_iso(const std::string& fileTypeChoice) {
    std::vector<std::string> files, originalFiles;
    files.reserve(100);
    binImgFilesCache.reserve(100);
	mdfMdsFilesCache.reserve(100);
    std::vector<std::string> directoryPaths;
    std::set<std::string> uniquePaths, processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts, invalidDirectoryPaths;

    std::string fileExtension, fileTypeName, fileType = fileTypeChoice;
    bool modeMdf = (fileType == "mdf");
    bool modeNrg = (fileType == "nrg");

    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        fileTypeName = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileTypeName = "MDF";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileTypeName = "NRG";
    }
    else {
        std::cout << "Invalid file type choice. Supported types: BIN/IMG, MDF, NRG\n";
        return;
    }

    while (true) {
        bool list = false, clr = false;
        successOuts.clear(); skippedOuts.clear(); failedOuts.clear(); deletedOuts.clear(); processedErrors.clear();
        clear_history();
        historyPattern = false;
        loadHistory();

        std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m ↵ to scan for \001\033[1;38;5;208m\002" + fileExtension +
                             "\001\033[1;94m files (>= 5MB) and import into \001\033[1;93m\002RAM\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), \001\033[1;92m\002ls \001\033[1;94m\002↵ open \001\033[1;93m\002RAM\001\033[1;94m\002 cache, "
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

            if (!modeMdf && !modeNrg) {
                binImgFilesCache.clear();
                std::cout << "\n\033[1;92mBIN/IMG RAM cache cleared.\033[0;1m\n";
            } else if (modeMdf){
                mdfMdsFilesCache.clear();
                std::cout << "\n\033[1;92mMDF RAM cache cleared.\033[0;1m\n";
            } else if (modeNrg){
                mdfMdsFilesCache.clear();
                std::cout << "\n\033[1;92mNRG RAM cache cleared.\033[0;1m\n";
            }
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        // Return static cache if input=ls
		if (list && !modeMdf && !modeNrg) {
			files = binImgFilesCache;
		} else if (list && modeMdf) {
			files = mdfMdsFilesCache;
		} else if (list && modeNrg) {
			files = nrgFilesCache;
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
            std::cout << "\033[1;91mNo valid paths provided.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        bool newFilesFound = false;
        if (!list) {
			// Disable input before processing
			disableInput();
            files = findFiles(directoryPaths, fileType, [&](const std::string&, const std::string&) {
                newFilesFound = true;
            }, invalidDirectoryPaths, processedErrors);
        }

        if (!newFilesFound && !files.empty() && !list) {
            std::cout << "\n";
            verboseFind(invalidDirectoryPaths);
            auto end_time = std::chrono::high_resolution_clock::now();
            if (gapSet || !gapSet) {
				std::cout << "\n";
			}
			gapSetTotal = true;
            std::cout << "\033[1;91mNo new " << fileExtension << " files over 5MB found. \033[1;92m" << files.size() << " files are cached in RAM from previous searches.\033[0;1m\n\n";
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
            std::cout << "\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        if (files.empty() && !list) {
            std::cout << "\n";
            verboseFind(invalidDirectoryPaths);
            auto end_time = std::chrono::high_resolution_clock::now();
            if (gapSet || !gapSet) {
				std::cout << "\n";
			}
			if (!gapSetTotal && invalidDirectoryPaths.empty()) {
			     std::cout << "\033[2A\033[K";
			}
			gapSetTotal = true;
            std::cout << "\033[1;91mNo " << fileExtension << " files over 5MB found in the specified paths or cached in RAM.\n\033[0;1m\n";
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
            std::cout << "\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }
        if (!modeMdf && !modeNrg) {
			originalFiles = binImgFilesCache;
        } else if (modeMdf){
			originalFiles = mdfMdsFilesCache;
		} else if (modeNrg) {
			originalFiles = nrgFilesCache;
		}
		bool isFiltered = false;
		bool isFilteredButUnchanged = false;
		bool needsScrnClr = true;
        while (true) {
            successOuts.clear(); skippedOuts.clear(); failedOuts.clear(); deletedOuts.clear(); processedErrors.clear();

            if (binImgFilesCache.empty() && !modeMdf && !modeNrg) {
                std::cout << "\n\033[1;93mNo " << fileExtension << " file entries stored in RAM cache for potential ISO conversions.\033[1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                break;
            } else if (mdfMdsFilesCache.empty() && modeMdf) {
                std::cout << "\n\033[1;93mNo " << fileExtension << " file entries stored in RAM cache for potential ISO conversions.\033[1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                break;
            } else if (nrgFilesCache.empty() && modeNrg) {
                std::cout << "\n\033[1;93mNo " << fileExtension << " file entries stored in RAM cache for potential ISO conversions.\033[1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                break;
            }
			if (needsScrnClr) {
				clearScrollBuffer();
				std::cout << "\n";
				sortFilesCaseInsensitive(files);
				printFileList(files);
			}

            clear_history();
           std::string prompt = std::string(isFiltered ? "\n\001\033[1;96m\002Filtered \001\033[1;92m\002" : "\n\001\033[1;92m\002" ) + fileTypeName + "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
			std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
			std::string mainInputString(rawInput.get());

			if (std::isspace(rawInput.get()[0]) || rawInput.get()[0] == '\0') {
				clearScrollBuffer();
				if (isFiltered && !isFilteredButUnchanged) {
					needsScrnClr = true;
					// Reset to unfiltered list
					if (!modeMdf && !modeNrg) {
						files = binImgFilesCache;
					} else if (modeMdf) {
						files = mdfMdsFilesCache;
					} else if (modeNrg) {
						files = nrgFilesCache;
					}
					isFiltered = false;
					isFilteredButUnchanged = false;
					continue;  // Continue to show unfiltered list
				} else {
					needsScrnClr = false;
					break;  // Exit the function
				}
			}

			if (strcmp(rawInput.get(), "/") == 0) {
				std::vector<std::string> beforeFilterFiles = files;

				// If the list is already filtered, apply filter on the filtered list
				if (isFiltered || isFilteredButUnchanged) {
					applyFilter(files, files, fileTypeName);  // Apply filter on the already filtered list
				} else {
					// First time filtering or no filter applied yet
					applyFilter(files, originalFiles, fileTypeName);  // Apply filter on the full list
				}

				// Check filter status after applying
				if (!modeMdf && !modeNrg) {
					if (binImgFilesCache.size() == files.size() || files.size() == originalFiles.size()) {
						isFilteredButUnchanged = true;
					} else {
						isFiltered = true;
						isFilteredButUnchanged = false;
					}
				} else if (modeMdf){
					if (mdfMdsFilesCache.size() == files.size() || files.size() == originalFiles.size()) {
						isFilteredButUnchanged = true;
					} else {
						isFiltered = true;
						isFilteredButUnchanged = false;
					}
				} else if (modeNrg) {
					if (nrgFilesCache.size() == files.size() || files.size() == originalFiles.size()) {
						isFilteredButUnchanged = true;
					} else {
						isFiltered = true;
						isFilteredButUnchanged = false;
					}
				}

			} else {
				clearScrollBuffer();
				std::cout << "\033[1m" << std::endl;
				processInput(mainInputString, files, modeMdf, modeNrg, processedErrors, successOuts, skippedOuts, failedOuts, deletedOuts);
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
void processInput(const std::string& input, const std::vector<std::string>& fileList, bool modeMdf, bool modeNrg, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts) {
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

	std::atomic<size_t> completedTasks(0);
	// Create atomic flag for completion status
    std::atomic<bool> isComplete(false);

    std::set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;
    auto asyncConvertToISO = [&](const std::string& selectedFile) {
        std::size_t found = selectedFile.find_last_of("/\\");
        std::string filePath = selectedFile.substr(0, found);
        selectedFilePaths.emplace(filePath);
        convertToISO(selectedFile, successOuts, skippedOuts, failedOuts, deletedOuts, modeMdf, modeNrg);
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
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrors) {
    std::mutex fileCheckMutex;
    bool blacklistMdf = false;
    bool blacklistNrg = false;
    std::set<std::string> fileNames;
    std::mutex mutex4search;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Disable input before processing
    disableInput();

    // Consolidated set for all invalid paths
    std::set<std::string> invalidPaths;

    size_t totalFiles = 0;
    for (const auto& path : paths) {
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    totalFiles++;
                    std::cout << "\rTotal files processed: " << totalFiles << std::flush;
                }
            }
            if (totalFiles == 0) {
                gapSetTotal = false;
            }
        } catch (const std::filesystem::filesystem_error& e) {
            gapSet = false;
            std::string errorMessage = "\033[1;91mError traversing directory: " + path + " - " + e.what() + "\033[0;1m";
            processedErrors.insert(errorMessage);
            invalidPaths.insert(path);
        }
    }

    if (!processedErrors.empty()) {
        std::cout << "\n\n";
        for (const auto& error : processedErrors) {
            std::cout << "\033[1;91m" << error << "\033[0;1m" << std::endl;
        }
        processedErrors.clear();
    }

    std::vector<std::future<void>> futures;
    futures.reserve(totalFiles);
    unsigned int numOngoingTasks = 0;

    // Process a file asynchronously
    auto processFileAsync = [&](const std::filesystem::directory_entry& entry) {
        std::string fileName = entry.path().string();
        std::string filePath = entry.path().parent_path().string();
        
        // Callback to handle the file (actual file processing logic)
        callback(fileName, filePath);

        // Lock mutex to ensure thread-safe access to fileNames
        std::lock_guard<std::mutex> lock(mutex4search);
        fileNames.insert(fileName);
        --numOngoingTasks;
    };

    // Function to process files in batches
    auto processBatch = [&](const std::vector<std::filesystem::directory_entry>& batch) {
        // Process each file in the batch asynchronously
        for (const auto& entry : batch) {
            if (entry.is_regular_file()) {
                std::string fileName = entry.path().string();
                std::vector<std::string>* cache = (mode == "bin") ? &binImgFilesCache
                                                                 : (mode == "mdf") ? &mdfMdsFilesCache
                                                                                   : (mode == "nrg") ? &nrgFilesCache
                                                                                                     : nullptr;

                // Skip the file if it already exists in the cache
                if (std::find(cache->begin(), cache->end(), fileName) == cache->end()) {
                    if (numOngoingTasks < maxThreads) {
                        ++numOngoingTasks;
                        std::lock_guard<std::mutex> lock(mutex4search);
                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                    } else {
                        // Wait for some tasks to finish
                        for (auto& future : futures) {
                            if (future.valid()) {
                                future.get();
                            }
                        }
                        ++numOngoingTasks;
                        std::lock_guard<std::mutex> lock(mutex4search);
                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                    }
                }
            }
        }
    };

    // Iterate through the paths and process files in batches of 100
    for (const auto& path : paths) {
        try {
            blacklistMdf = (mode == "mdf");
            blacklistNrg = (mode == "nrg");

            std::vector<std::filesystem::directory_entry> batch;

            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file() && blacklist(entry, blacklistMdf, blacklistNrg)) {
                    batch.push_back(entry);

                    // Once we have 100 files in the batch, process them
                    if (batch.size() >= 100) {
                        processBatch(batch);
                        batch.clear();  // Reset the batch for the next set of files
                    }
                }
            }

            // Process any remaining files in the batch (if less than 100)
            if (!batch.empty()) {
                processBatch(batch);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::lock_guard<std::mutex> lock(mutex4search);
            if (e.code() == std::errc::permission_denied) {
                invalidPaths.insert(path);
            } else {
                std::string errorMessage = "\033[1;91mError processing path: " + path + " - " + e.what() + "\033[0;1m";
                processedErrors.insert(errorMessage);
            }
        }
    }

    // Wait for all tasks to finish
    for (auto& future : futures) {
        if (future.valid()) {
            future.get();
        }
    }

    // Flush and Restore input after processing
    flushStdin();
    restoreInput();

    // Update invalidDirectoryPaths with all invalid paths
    invalidDirectoryPaths.insert(invalidPaths.begin(), invalidPaths.end());

    // Print success message if files were found
    if (!fileNames.empty()) {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\n";

        verboseFind(invalidDirectoryPaths);
        if (gapSet) {
            std::cout << "\n";
        }
        if ((!invalidPaths.empty() && !gapSet) || !gapSet || (gapSet && !invalidPaths.empty())) {
            std::cout << "\n";
        }

        if (mode == "bin") {
            std::cout << "\033[1;92mFound " << fileNames.size() << " matching files" << ".\033[1;93m " << binImgFilesCache.size() << " matching files cached in RAM from previous searches.\033[0;1m\n";
        } else if (mode == "mdf") {
            std::cout << "\033[1;92mFound " << fileNames.size() << " matching files" << ".\033[1;93m " << mdfMdsFilesCache.size() << " matching files cached in RAM from previous searches.\033[0;1m\n";
        } else if (mode == "nrg") {
            std::cout << "\033[1;92mFound " << fileNames.size() << " matching files" << ".\033[1;93m " << nrgFilesCache.size() << " matching files cached in RAM from previous searches.\033[0;1m\n";
        }

        std::cout << "\n";
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        std::cout << "\n";

        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    // Add the files to the cache based on the mode
    std::lock_guard<std::mutex> lock(mutex4search);
    if (mode == "bin") {
        binImgFilesCache.insert(binImgFilesCache.end(), fileNames.begin(), fileNames.end());
        return binImgFilesCache;
    }

    if (mode == "mdf") {
        mdfMdsFilesCache.insert(mdfMdsFilesCache.end(), fileNames.begin(), fileNames.end());
        return mdfMdsFilesCache;
    }

    if (mode == "nrg") {
        nrgFilesCache.insert(nrgFilesCache.end(), fileNames.begin(), fileNames.end());
        return nrgFilesCache;
    }

    // Return an empty vector if mode is not recognized
    return std::vector<std::string>();
}



// Blacklist function for MDF BIN IMG
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf, bool blacklistNrg) {
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
    std::string output;
    output.reserve(fileList.size() * 100);  // Adjust based on average line length
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
        std::ostringstream temp;
        temp << (isSpecialExtension ? sequenceColor : "")
             << std::setw(numDigits) << std::right << (i + 1) << ". " << reset << bold;
        output.append(temp.str());
        if (isSpecialExtension) {
            output.append(directory)
                  .append("/")
                  .append(orangeBold)
                  .append(fileNameOnly);
        } else {
            output.append(filename);
        }
        output.append(reset)
              .append(bold)
              .append("\n");
    }
    std::cout << output;
}


// Function to convert a BIN/IMG/MDF file to ISO format
void convertToISO(const std::string& inputPath, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool modeMdf, bool modeNrg) {
    // Get the real user ID and group ID (of the user who invoked sudo)
    uid_t real_uid;
    gid_t real_gid;
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        real_uid = std::stoul(sudo_uid);
        real_gid = std::stoul(sudo_gid);
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name
    struct passwd *pw = getpwuid(real_uid);
    if (pw == nullptr) {
        std::cerr << "\nError getting user information: " << strerror(errno) << "\033[0;1m";
        return;
    }
    std::string real_username(pw->pw_name);

    // Get real group name
    struct group *gr = getgrgid(real_gid);
    if (gr == nullptr) {
        std::cerr << "\nError getting group information: " << strerror(errno) << "\033[0;1m";
        return;
    }
    std::string real_groupname(gr->gr_name);

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

    // Perform the conversion
    bool conversionSuccess;
    if (modeMdf) {
        conversionSuccess = convertMdfToIso(inputPath, outputPath);
    } else if (!modeMdf && !modeNrg){
        conversionSuccess = convertCcdToIso(inputPath, outputPath);
    } else if (modeNrg) {
		conversionSuccess = convertNrgToIso(inputPath, outputPath);
	}
		

    auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);

    if (conversionSuccess) {
        // Change ownership of the created ISO file
        struct stat file_stat;
        if (stat(outputPath.c_str(), &file_stat) == 0) {
            // Only change ownership if it's different from the current user
            if (file_stat.st_uid != real_uid || file_stat.st_gid != real_gid) {
                if (chown(outputPath.c_str(), real_uid, real_gid) != 0) {
                    std::string errorMessage = "\033[1;91mFailed to change ownership of \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m: " + strerror(errno) + "\033[0;1m";
                    {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        failedOuts.insert(errorMessage);
                    }
                }
            }
        } else {
            std::string errorMessage = "\033[1;91mFailed to get file information for \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m: " + strerror(errno) + "\033[0;1m";
            {   std::lock_guard<std::mutex> lowLock(Mutex4Low);
                failedOuts.insert(errorMessage);
            }
        }

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
        }
    }
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
