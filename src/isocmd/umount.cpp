#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF

// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    const char* isoPath = "/mnt";
    std::vector<std::string> isoDirs;
    isoDirs.reserve(100);  // Pre-allocate space for 100 entries

        DIR* dir = opendir(isoPath);
        if (dir == nullptr) {
            std::cerr << "\033[1;91mError opening the /mnt directory.\033[0;1m\n";
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && strncmp(entry->d_name, "iso_", 4) == 0) {
                isoDirs.emplace_back(entry->d_name + 4);
            }
        }
        closedir(dir);

    if (isoDirs.empty()) {
        return;
    }

    sortFilesCaseInsensitive(isoDirs);

    std::ostringstream output;
    // Reserve estimated space for the string buffer
    std::string buffer;
    buffer.reserve(isoDirs.size() * 100);
    output.str(std::move(buffer));

    output << "\033[92;1m// CHANGES ARE REFLECTED AUTOMATICALLY //\033[0;1m\n\n";

    size_t numDigits = std::to_string(isoDirs.size()).length();

    for (size_t i = 0; i < isoDirs.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? "\033[31;1m" : "\033[32;1m";
        output << sequenceColor << std::setw(numDigits) << (i + 1) << ". "
               << "\033[94;1m/mnt/iso_\033[95;1m" << isoDirs[i] << "\033[0;1m\n";
    }

    std::cout << output.str();
}


// Function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;  // Unable to open directory
    }

    errno = 0;
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (++count > 2) {
            closedir(dir);
            return false;  // Directory not empty
        }
    }

    closedir(dir);
    return errno == 0 && count <= 2;  // Empty if only "." and ".." entries and no errors
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    // Check for root privileges
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "\033[1;93m'\033[1;91m. Root privileges are required.\033[0m";
            {
				std::lock_guard<std::mutex> lowLock(Mutex4Low);
				unmountedErrors.insert(errorMessage.str());
			}
        }
        return;
    }

    // Construct the unmount command
    std::string unmountCommand = "umount -l";
    for (const auto& isoDir : isoDirs) {
        unmountCommand += " " + shell_escape(isoDir) + " 2>/dev/null";
    }

    // Execute the unmount command
    int unmountResult = system(unmountCommand.c_str());
    if (unmountResult != 0) {
        // Some error occurred during unmounting
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            if (!isDirectoryEmpty(isoDir)) {
                errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "\033[1;93m'\033[1;91m. Probably not an ISO mountpoint.\033[0m";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.insert(errorMessage.str());
					}
                }
            }
        }
    }

    // Remove empty directories
    std::vector<const char*> directoriesToRemove;
    for (const auto& isoDir : isoDirs) {
        if (isDirectoryEmpty(isoDir)) {
            directoriesToRemove.push_back(isoDir.c_str());
        }
    }

    if (!directoriesToRemove.empty()) {
        int removeDirResult = 0;
        for (const char* dir : directoriesToRemove) {
            removeDirResult = rmdir(dir);
            if (removeDirResult != 0) {
                break;
            }
        }

        if (removeDirResult == 0) {
            for (const auto& dir : directoriesToRemove) {	
                auto [directory, filename] = extractDirectoryAndFilename(dir);
                std::string removedDirInfo = "\033[0;1mUnmounted: \033[1;92m'" + directory + "/" + filename + "\033[1;92m'\033[0m.";
                {
					std::lock_guard<std::mutex> lowLock(Mutex4Low);
					unmountedFiles.insert(removedDirInfo);
				}
            }
        } else {
            for (const auto& isoDir : directoriesToRemove) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDir << "'\033[1;91m.\033[0m";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.insert(errorMessage.str());
					}
                }
            }
        }
    }
}


// Function to print unmounted ISOs and errors
void printUnmountedAndErrors(bool invalidInput, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::set<std::string>& errorMessages) {
	clearScrollBuffer();
	
    // Print unmounted files
    for (const auto& unmountedFile : unmountedFiles) {
        std::cout << "\n" << unmountedFile;
    }
    
    if (!unmountedErrors.empty() && !unmountedFiles.empty()) {
				std::cout << "\n";
			}
			
	for (const auto& unmountedError : unmountedErrors) {
        std::cout << "\n" << unmountedError;
    }
    
    if (invalidInput) {
				std::cout << "\n";
			}
	unmountedFiles.clear();
	unmountedErrors.clear();
    // Print unique error messages
    for (const auto& errorMessage : errorMessages) {
            std::cerr << "\n\033[1;91m" << errorMessage << "\033[0m\033[1m";
        }
    errorMessages.clear();
}


// Main function for unmounting ISOs
void unmountISOs() {
    // Initialize necessary variables
    std::vector<std::string> isoDirs;
    isoDirs.reserve(100);
    
    // Vector to store ISO invalid input errors
	std::set<std::string> errorMessages;
	// Vector to store ISO unmounts
	std::set<std::string> unmountedFiles;
	// Vector to store ISO unmount errors
	std::set<std::string> unmountedErrors;
	
    const std::string isoPath = "/mnt";
    bool invalidInput = false, skipEnter = false, isFiltered = false, noValid = true;

    while (true) {
        // Initialize variables for each loop iteration
        std::vector<std::string> filteredIsoDirs, selectedIsoDirs, selectedIsoDirsFiltered;
        
        clearScrollBuffer();
        listMountedISOs();
        isoDirs.clear();
        unmountedFiles.clear();
		unmountedErrors.clear();
		errorMessages.clear();
        invalidInput = false;

            // Populate isoDirs with directories that match the "iso_" prefix
            for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
                if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                    isoDirs.push_back(entry.path().string());
                }
            }

            sortFilesCaseInsensitive(isoDirs);


        // Check if there are no matching directories
        if (isoDirs.empty()) {
            std::cerr << "\033[1;93mNo path(s) matching the '/mnt/iso_*' pattern found.\033[0m\033[1m\n";
            std::cout << "\n\033[1;32m↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
		
        // Prompt the user for input
        char* input = readline("\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), / ↵ to filter\001\033[1;94m\002, or ↵ to return:\001\033[0m\002\001\033[1m\002 ");
        clearScrollBuffer();
        

        if (input[0] != '/' || (!(std::isspace(input[0]) || input[0] == '\0'))) {
            std::cout << "\033[0;1mPlease wait...\n";
        }

        // Check if the user wants to return to the main menu
        if (std::isspace(input[0]) || input[0] == '\0') {
			free(input);
            break;
        }

        // Check if the user wants to filter the list of ISOs
        if (strcmp(input, "/") == 0) {
			bool breakOuterLoop = false;
            while (true) {
				if (breakOuterLoop) {
					historyPattern = false;
					break;
				}
                clearScrollBuffer();
                isFiltered = true;
                historyPattern = true;
                loadHistory();
                std::string prompt;
                prompt = "\n\001\033[1;92m\002Term(s)\001\033[1;94m\002 ↵ to filter \001\033[1;93m\002umount\001\033[1;94m\002 list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0m\033[1m\002";
                
                char* filterPattern = readline(prompt.c_str());
                clearScrollBuffer();
                
                if (filterPattern && filterPattern[0] != '\0') {
					std::cout << "\033[1mPlease wait...\033[1m\n";
					if (strcmp(filterPattern, "/") != 0){
						add_history(filterPattern); // Add the search query to the history
						saveHistory();
					}
				}

				clear_history();

				if (filterPattern[0] == '\0' || strcmp(filterPattern, "/") == 0) {
					free(filterPattern);
					skipEnter = false;
					isFiltered = false;
					noValid = false;
					historyPattern = false;
					break;
				}

				// Split the filterPattern string into tokens using the delimiter ';'
				std::vector<std::string> filterPatterns;
				std::stringstream ss(filterPattern);
				std::string token;
				while (std::getline(ss, token, ';')) {
					filterPatterns.push_back(token);
					toLowerInPlace(filterPatterns.back());
				}
				free(filterPattern);

				// Filter the list of ISO directories based on the filter pattern
				filteredIsoDirs.clear();

				

				// Calculate the number of directories per thread
				size_t numDirs = isoDirs.size();
				unsigned int numThreads = std::min(static_cast<unsigned int>(numDirs), maxThreads);
				// Vector to store futures for tracking tasks' completion
				std::vector<std::future<void>> futuresFilter;
				futuresFilter.reserve(numThreads);
				size_t baseDirsPerThread = numDirs / numThreads;
				size_t remainder = numDirs % numThreads;

				// Launch filter tasks asynchronously
				for (size_t i = 0; i < numThreads; ++i) {
					size_t start = i * baseDirsPerThread + std::min(i, remainder);
					size_t end = start + baseDirsPerThread + (i < remainder ? 1 : 0);

					futuresFilter.push_back(std::async(std::launch::async, [&](size_t start, size_t end) {
						filterMountPoints(isoDirs, filterPatterns, filteredIsoDirs, start, end);
					}, start, end));
				}

				// Wait for all filter tasks to complete
				for (auto& future : futuresFilter) {
					future.get();
				}

                // Check if any directories matched the filter
                if (filteredIsoDirs.empty()) {
                    clearScrollBuffer();
                    std::cout << "\033[1;91mNo ISO mountpoint(s) match the filter pattern.\033[0m\033[1m\n";
                    std::cout << "\n\033[1;32m↵ to continue...";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    clearScrollBuffer();
                } else {
                    // Display the filtered list and prompt the user for input
                    while (true) {
                        clearScrollBuffer();
                        unmountedFiles.clear();
						unmountedErrors.clear();
						errorMessages.clear();
                        invalidInput = false;
                        sortFilesCaseInsensitive(filteredIsoDirs);
                        std::cout << "\033[1mFiltered results:\n\033[0m\033[1m" << std::endl;
                        size_t maxIndex = filteredIsoDirs.size();
						size_t numDigits = std::to_string(maxIndex).length();

						for (size_t i = 0; i < filteredIsoDirs.size(); ++i) {
							std::string afterSlash = filteredIsoDirs[i].substr(filteredIsoDirs[i].find_last_of("/") + 1);
							std::string afterUnderscore = afterSlash.substr(afterSlash.find("_") + 1);
							std::string color = (i % 2 == 0) ? "\033[1;31m" : "\033[1;32m"; // Red if even, Green if odd

							std::cout << color << "\033[1m"
                            << std::setw(numDigits) << std::setfill(' ') << (i + 1) << ".\033[0;1m /mnt/iso_"
                            << "\033[1;95m" << afterUnderscore << "\033[0;1m\n";
						}

                        // Prompt the user for the list of ISOs to unmount
                        char* chosenNumbers = readline("\n\001\033[1;92m\002Filtered ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), or ↵ to return:\001\033[0m\002\001\033[1m\002 ");

                        if (std::isspace(chosenNumbers[0]) || chosenNumbers[0] == '\0') {
							free(chosenNumbers);
                            noValid = false;
                            skipEnter = true;
                            historyPattern = false;
                            break;
                        }

                        if (std::strcmp(chosenNumbers, "00") == 0) {
							free(chosenNumbers);
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
                            selectedIsoDirs = filteredIsoDirs;
                            isFiltered = true;
                            breakOuterLoop = true;
                            historyPattern = false;
                            skipEnter= false;
                            break;
                        }

                        // Parse the user input to determine which ISOs to unmount
                        std::set<size_t> selectedIndices;
                        std::istringstream iss(chosenNumbers);
                        free(chosenNumbers);
                        for (std::string token; iss >> token;) {
                            try {
                                size_t dashPos = token.find('-');
                                if (dashPos != std::string::npos) {
                                    size_t start = std::stoi(token.substr(0, dashPos)) - 1;
                                    size_t end = std::stoi(token.substr(dashPos + 1)) - 1;
                                    if (start < filteredIsoDirs.size() && end < filteredIsoDirs.size()) {
                                        for (size_t i = std::min(start, end); i <= std::max(start, end); ++i) {
                                            selectedIndices.insert(i);
                                        }
                                    } else {
                                        errorMessages.insert("Invalid range: '" + token + "'.");
                                        invalidInput = true;
                                    }
                                } else {
                                    size_t index = std::stoi(token) - 1;
                                    if (index < filteredIsoDirs.size()) {
                                        selectedIndices.insert(index);
                                    } else {
                                        errorMessages.insert("Invalid index: '" + token + "'.");
                                        invalidInput = true;
                                    }
                                }
                            } catch (const std::invalid_argument&) {
									errorMessages.insert("Invalid input: '" + token + "'.");
									invalidInput = true;
                            }
                        }               

                        selectedIsoDirsFiltered.clear();
                        for (size_t index : selectedIndices) {
                            selectedIsoDirsFiltered.push_back(filteredIsoDirs[index]);
                        }

                        if (!selectedIsoDirsFiltered.empty()) {
                            selectedIsoDirs = selectedIsoDirsFiltered;
                            skipEnter = false;
                            isFiltered = true;
                            historyPattern = false;
                            break; // Exit filter loop to process unmount
                        } else {
                            clearScrollBuffer();
                            verbose = false;
                            std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                            std::cout << "\n\033[1;32m↵ to continue...";
                            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                        }
                    }
                }

                if (!selectedIsoDirsFiltered.empty() && isFiltered) {
					clearScrollBuffer();
					std::cout << "\033[1mPlease wait...\033[1m\n";
					isFiltered = true;
					historyPattern = false;
                    break;
                }
            }
        }

        // Check if the user wants to unmount all ISOs
        if (std::strcmp(input, "00") == 0) {
			free(input);
            selectedIsoDirs = isoDirs;
        } else if (!isFiltered) {
            // Parse the user input to determine which ISOs to unmount
            std::set<size_t> selectedIndices;
            std::istringstream iss(input);
            free(input);
            for (std::string token; iss >> token;) {
                try {
                    size_t dashPos = token.find('-');
                    if (dashPos != std::string::npos) {
                        size_t start = std::stoi(token.substr(0, dashPos)) - 1;
                        size_t end = std::stoi(token.substr(dashPos + 1)) - 1;
                        if (start < isoDirs.size() && end < isoDirs.size()) {
                            for (size_t i = std::min(start, end); i <= std::max(start, end); ++i) {
                                selectedIndices.insert(i);
                            }
                        } else {
                            errorMessages.insert("Invalid range: '" + token + "'.");
                            invalidInput = true;
                        }
                    } else {
                        size_t index = std::stoi(token) - 1;
                        if (index < isoDirs.size()) {
                            selectedIndices.insert(index);
                        } else {
                            errorMessages.insert("Invalid index: '" + token + "'.");
                            invalidInput = true;
                        }
                    }
                } catch (const std::invalid_argument&) {
                    errorMessages.insert("Invalid input: '" + token + "'.");
                    invalidInput = true;
                }
            }

            if (!selectedIndices.empty()) {
                for (size_t index : selectedIndices) {
                    selectedIsoDirs.push_back(isoDirs[index]);
                }
            } else {
                clearScrollBuffer();
                if (noValid) {
					verbose = false;
                    std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                    std::cout << "\n\033[1;32m↵ to continue...";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                }
                noValid = true;
            }
        }

        // If there are selected ISOs, proceed to unmount them
        if (!selectedIsoDirs.empty()) {
			unsigned int numThreads = std::min(static_cast<int>(selectedIsoDirs.size()), static_cast<int>(maxThreads));
			ThreadPool pool(numThreads);
			std::vector<std::future<void>> futuresUmount;
			futuresUmount.reserve(numThreads);
    
			// Divide the selected ISOs into batches for parallel processing
			size_t batchSize = (selectedIsoDirs.size() + maxThreads - 1) / maxThreads;
			std::vector<std::vector<std::string>> batches;
			for (size_t i = 0; i < selectedIsoDirs.size(); i += batchSize) {
				batches.emplace_back(selectedIsoDirs.begin() + i, std::min(selectedIsoDirs.begin() + i + batchSize, selectedIsoDirs.end()));
			}
    
			std::atomic<int> completedIsos(0);
			int totalIsos = static_cast<int>(selectedIsoDirs.size());
			std::atomic<bool> isComplete(false);

			// Start the progress bar in a separate thread
			std::thread progressThread(displayProgressBar, std::ref(completedIsos), std::cref(totalIsos), std::ref(isComplete));

			// Enqueue unmount tasks for each batch of ISOs
			for (const auto& batch : batches) {
				{
					std::lock_guard<std::mutex> highLock(Mutex4High);
					futuresUmount.emplace_back(pool.enqueue([batch, &unmountedFiles, &unmountedErrors, &completedIsos]() {
						for (const auto& iso : batch) {
							unmountISO({iso}, unmountedFiles, unmountedErrors);
							completedIsos.fetch_add(1, std::memory_order_relaxed);
						}
					}));
				}
			}
			// Wait for all unmount tasks to complete
			for (auto& future : futuresUmount) {
				future.wait();
			}

			// Signal completion and wait for progress thread to finish
			isComplete.store(true);
			progressThread.join();
            if (verbose) {
				printUnmountedAndErrors(invalidInput, unmountedFiles, unmountedErrors, errorMessages);
			} else {
				unmountedFiles.clear();
				unmountedErrors.clear();
				errorMessages.clear();
			}
				

            // Prompt the user to press Enter to continue
            if (!skipEnter && verbose) {
				std::cout << "\n\n\033[1;32m↵ to continue...";
				std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

            }
            clearScrollBuffer();
            skipEnter = false;
            isFiltered = false;
        }
    }
}
