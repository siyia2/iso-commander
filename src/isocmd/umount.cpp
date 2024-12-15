// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF

// Function to print isoDirs formatting
void listMountedISOs(std::vector<std::string>& isoDirs) {
    // ANSI color codes
    static const char* redBold = "\033[31;1m";
    static const char* greenBold = "\033[32;1m";
    static const char* blueBold = "\033[94;1m";
    static const char* magentaBold = "\033[95;1m";
    static const char* resetColor = "\033[0;1m";

    size_t maxIndex = isoDirs.size();
    size_t numDigits = std::to_string(maxIndex).length();

    // Precompute padded index strings to avoid recalculating them
    std::vector<std::string> indexStrings(maxIndex);
    for (size_t i = 0; i < maxIndex; ++i) {
        indexStrings[i] = std::to_string(i + 1);
        if (indexStrings[i].length() < numDigits) {
            indexStrings[i].insert(0, numDigits - indexStrings[i].length(), ' ');
        }
    }

    // Use an ostringstream for buffered output
    std::ostringstream output;

    // Iterate through ISO directories
    for (size_t i = 0; i < isoDirs.size(); ++i) {
        // Extract the directory name after the last '/'
        size_t lastSlashPos = isoDirs[i].find_last_of('/');
        std::string dirName = (lastSlashPos != std::string::npos) 
                                ? isoDirs[i].substr(lastSlashPos + 1) 
                                : isoDirs[i];

        // Extract the part after the first '_'
        size_t underscorePos = dirName.find('_');
        std::string afterUnderscore = (underscorePos != std::string::npos) 
                                        ? dirName.substr(underscorePos + 1) 
                                        : dirName;

        // Alternate colors for sequences
        const char* sequenceColor = (i % 2 == 0) ? redBold : greenBold;

        // Append formatted output to the buffer
        output << sequenceColor
               << indexStrings[i]
               << ". "
               << blueBold
               << "/mnt/iso_"
               << magentaBold
               << afterUnderscore
               << resetColor
               << "\n";
    }

    // Flush the buffered output at once
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
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::mutex& Mutex4Low) {
    // Check for root privileges
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            {
				std::lock_guard<std::mutex> lowLock(Mutex4Low);
				unmountedErrors.emplace(errorMessage.str());
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
                errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "\033[1;93m'\033[1;91m.\033[0;1m {notAnISO}";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.emplace(errorMessage.str());
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
					unmountedFiles.emplace(removedDirInfo);
				}
            }
        } else {
            for (const auto& isoDir : directoriesToRemove) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDir << "'\033[1;91m.\033[0m";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.emplace(errorMessage.str());
					}
                }
            }
        }
    }
}


// Function to print unmounted ISOs and errors
void printUnmountedAndErrors(std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::set<std::string>& errorMessages) {
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

    if (!errorMessages.empty()) {
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
void unmountISOs(bool& historyPattern, bool& verbose) {
    std::vector<std::string> isoDirs;
    std::set<std::string> errorMessages, unmountedFiles, unmountedErrors;
    std::mutex umountMutex;
    std::mutex Mutex4Low; // Mutex for low-level processing
    const std::string isoPath = "/mnt";
    bool isFiltered = false;
    bool clr = true;
    std::vector<std::string> filteredIsoDirs;
    filteredIsoDirs.reserve(100);
    isoDirs.reserve(100);

    while (true) {
	// Reset flags and prepare for selection
	unmountedFiles.clear();
        unmountedErrors.clear();
        errorMessages.clear();
        
        verbose = false;
        historyPattern = false;
        std::vector<std::string> selectedIsoDirs;
        selectedIsoDirs.reserve(maxThreads);
        
        // Populate or use existing ISO directories
        if (!isFiltered) {
            isoDirs.clear();
            for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
                if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                    isoDirs.push_back(entry.path().string());
                }
            }
            sortFilesCaseInsensitive(isoDirs);
        } else {
            // If using filtered directories, ensure they're sorted
            sortFilesCaseInsensitive(filteredIsoDirs);
        }

        // Determine which directory list to use
        std::vector<std::string>& currentIsoDirs = isFiltered ? filteredIsoDirs : isoDirs;

        // Clear screen and list ISOs if specified
        if (clr) {
            clearScrollBuffer();
            listMountedISOs(currentIsoDirs);
            if (!isFiltered){
				std::cout << "\n\n";
			}
        }

        if (isoDirs.empty() && !isFiltered) {
            clearScrollBuffer();
            std::cout << "\033[1A\033[K";
            std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_*' pattern found.\033[0m\033[1m\n";
            std::cout << "\n\033[1;32m↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }

        std::string prompt;
        if (isFiltered) {
            std::cout << "\033[K";
            prompt = "\n\001\033[1;96m\002Filtered \001\033[1;92m\002ISO"
                     "\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
        } else {
            std::cout << "\033[1A\033[K";
            prompt = "\001\033[1;92m\002ISO"
                     "\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
        }

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());

        if (!inputString.empty() && inputString != "/") {
            clearScrollBuffer();
            std::cout << "\033[1m\n";
        }

        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                filteredIsoDirs.clear(); // Clear the filtered list when returning to unfiltered mode
                continue;
            } else {
                return;
            }
        } else if (inputString == "/") {
            std::vector<std::string> baseSearchList = isFiltered ? filteredIsoDirs : isoDirs;
            std::vector<std::string> lastSuccessfulFilteredIsoDirs = filteredIsoDirs; // Initialize with unfiltered list
            static bool hadSuccessfulFilter = false;

            while (true) {
				// Verbose output is to be disabled unless specified by progressbar function downstream
                verbose = false;
                
                unmountedFiles.clear();
                unmountedErrors.clear();
                errorMessages.clear();
                
                clear_history();
                historyPattern = true;
                loadHistory(historyPattern);
                std::cout << "\033[1A\033[K";

                std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";
                std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);

                if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                    historyPattern = false;
                    clear_history();
                    // Revert to the last successful filter or the full list
                    if (hadSuccessfulFilter && !lastSuccessfulFilteredIsoDirs.empty()) {
                        filteredIsoDirs = lastSuccessfulFilteredIsoDirs;
                        clr = true;
                        isFiltered = true;
                    } else {
                        filteredIsoDirs = isoDirs;
                        historyPattern = false;
                        hadSuccessfulFilter = false;
                        clr = false;
                        isFiltered = false;
                    }
                    break;
                }

                std::string inputSearch(searchQuery.get());
                std::cout << "\033[1m\n"; 
                    

                // Call the new filterMountPoints function
                filteredIsoDirs = filterFiles(baseSearchList, inputSearch);

                if (filteredIsoDirs.size() == isoDirs.size()) {
                    // No filtering applied
                    std::cout << "\033[1A\033[K";
                    lastSuccessfulFilteredIsoDirs.clear();
                    filteredIsoDirs.clear();
                    isFiltered = false;
                    break;
                } else if (filteredIsoDirs.empty()) {
                    // No matches found; continue the loop
                    std::cout << "\033[1A\033[K";
                    continue;
                } else {
                    // Successful filter
                    if (lastSuccessfulFilteredIsoDirs != filteredIsoDirs) {
						add_history(searchQuery.get());
						saveHistory(historyPattern);
 
                        lastSuccessfulFilteredIsoDirs = filteredIsoDirs;
                        hadSuccessfulFilter = true;
                        clr = true;
                        isFiltered = true;
                    }
                      historyPattern = false;
					  clear_history();
                    break; // Exit loop after successful filtering
                }
            }
        } else {
			std::vector<std::string>& currentDirs = isFiltered ? filteredIsoDirs : isoDirs;
			// Continue with other logic
			std::vector<int> selectedIndices;

            if (inputString == "00") {
                selectedIsoDirs = currentDirs;
            } else {
            
				// Tokenize the input string
				tokenizeInput(inputString, currentDirs, errorMessages, selectedIndices);
			}
			
			for (int index : selectedIndices) {
				// Subtract 1 to convert from 1-based to 0-based indexing
				selectedIsoDirs.push_back(currentDirs[index - 1]);
			}
            

            if (!selectedIsoDirs.empty()) {
                unsigned int numThreads = std::min(static_cast<int>(selectedIsoDirs.size()), static_cast<int>(maxThreads));
                ThreadPool pool(numThreads);
                std::vector<std::future<void>> futuresUmount;
                futuresUmount.reserve(numThreads);

                size_t maxBatchSize = 100;  // Maximum number of items per batch

                // Calculate batch size ensuring it's at most maxBatchSize
                size_t batchSize = std::min(maxBatchSize, (selectedIsoDirs.size() + numThreads - 1) / numThreads);

                // Ensure batchSize is at least 1
                batchSize = std::max(batchSize, static_cast<size_t>(1));

                std::vector<std::vector<std::string>> batches;
                for (size_t i = 0; i < selectedIsoDirs.size(); i += batchSize) {
                    batches.emplace_back(selectedIsoDirs.begin() + i,
                                         std::min(selectedIsoDirs.begin() + i + batchSize,
                                         selectedIsoDirs.end()));
                }

                std::atomic<size_t> completedIsos(0);
                size_t totalIsos = selectedIsoDirs.size();
                std::atomic<bool> isComplete(false);
                std::thread progressThread(displayProgressBar, std::ref(completedIsos), std::cref(totalIsos), std::ref(isComplete), std::ref(verbose));

                for (auto& batch : batches) {
                    futuresUmount.emplace_back(pool.enqueue([batch = std::move(batch), &unmountedFiles, &unmountedErrors, &completedIsos, &umountMutex, &Mutex4Low]() {
                        std::lock_guard<std::mutex> umountLock(umountMutex);
                        std::for_each(batch.begin(), batch.end(), [&](const auto& iso) {
                            unmountISO({iso}, unmountedFiles, unmountedErrors, Mutex4Low);
                            completedIsos.fetch_add(1, std::memory_order_relaxed);
                        });
                    }));
                }

                for (auto& future : futuresUmount) {
                    future.wait();
                }

                isComplete.store(true, std::memory_order_release);
                progressThread.join();

				filteredIsoDirs.clear();

                if (verbose) {
                    printUnmountedAndErrors(unmountedFiles, unmountedErrors, errorMessages);
                    std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                } else {
                    unmountedFiles.clear();
                    unmountedErrors.clear();
                    errorMessages.clear();
                }

                isFiltered = false;
            } else {
                clearScrollBuffer();
                std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                std::cout << "\n\033[1;32m↵ to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
            clr = true;
        }
    }
}
