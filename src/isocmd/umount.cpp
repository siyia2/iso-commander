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
	std::cout << "\n";
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
	
	// Calls prevent_clear_screen and tab completion
    rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
    rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
	
    const std::string ISO_PATH = "/mnt";

    std::vector<std::string> isoDirs;
    std::vector<std::string> filteredIsoDirs;
    std::set<std::string> unmountedFiles;
    std::set<std::string> unmountedErrors;
    std::set<std::string> errorMessages;

    bool isFiltered = false;

    auto loadMountedISOs = [&]() {
        isoDirs.clear();
        for (const auto& entry : std::filesystem::directory_iterator(ISO_PATH)) {
            if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                isoDirs.push_back(entry.path().string());
            }
        }
        std::sort(isoDirs.begin(), isoDirs.end(), 
            [](const std::string& a, const std::string& b) {
                return strcasecmp(a.c_str(), b.c_str()) < 0;
            });
    };

    auto displayMountedISOs = [&]() {
        clearScrollBuffer();
        listMountedISOs(isFiltered ? filteredIsoDirs : isoDirs);
    };

    auto handleNoMountedISOs = []() {
        clearScrollBuffer();
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_*' pattern found.\033[0m\033[1m\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    auto performUnmount = [&](const std::vector<std::string>& selectedIsoDirs) {
		clearScrollBuffer();
        std::mutex umountMutex, lowLevelMutex;
        unsigned int numThreads = std::min(static_cast<unsigned int>(selectedIsoDirs.size()), maxThreads);
        ThreadPool pool(numThreads);
        std::vector<std::future<void>> unmountFutures;

        std::atomic<size_t> completedIsos(0);
        size_t totalIsos = selectedIsoDirs.size();
        std::atomic<bool> isComplete(false);

        std::thread progressThread(displayProgressBar, 
            std::ref(completedIsos), 
            std::cref(totalIsos), 
            std::ref(isComplete), 
            std::ref(verbose)
        );

        for (const auto& iso : selectedIsoDirs) {
            unmountFutures.emplace_back(pool.enqueue([&]() {
                std::lock_guard<std::mutex> lock(umountMutex);
                unmountISO({iso}, unmountedFiles, unmountedErrors, lowLevelMutex);
                completedIsos.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        for (auto& future : unmountFutures) {
            future.wait();
        }

        isComplete.store(true, std::memory_order_release);
        progressThread.join();

        if (verbose) {
            printUnmountedAndErrors(unmountedFiles, unmountedErrors, errorMessages);
            std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    };

	auto filterISOs = [&](const std::string& filterTerms) {
		// If filtering results in an empty list, keep the previous filtered list
		std::vector<std::string> newFilteredIsoDirs = filterFiles(isFiltered ? filteredIsoDirs : isoDirs, filterTerms);
    
		if (newFilteredIsoDirs.empty()) {
			std::cout << "\033[1A\033[K";
			return false;
		}
    
		// Update the filtered list
		filteredIsoDirs = newFilteredIsoDirs;
		isFiltered = true;
		return true;
	};

    while (true) {
        // Reset state
        unmountedFiles.clear();
        unmountedErrors.clear();
        errorMessages.clear();

        // Load mounted ISOs if not filtered
        if (!isFiltered) {
            loadMountedISOs();
        }

        // Display ISOs
        displayMountedISOs();

        // Handle case with no ISOs
        if (isoDirs.empty() && !isFiltered) {
            handleNoMountedISOs();
            return;
        }

        // Get user input
        std::string prompt = isFiltered 
            ? "\n\001\033[1;96m\002Filtered \001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 "
            : "\n\001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get() ? input.get() : "");

        // Handle empty input
        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                filteredIsoDirs.clear();
                continue;
            }
            return;
        }

        // Handle filtering
        if (inputString == "/") {
		clear_history();
        historyPattern = true;
        loadHistory(historyPattern);
        std::cout << "\033[1A\033[K";
        std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";

        while (true) {
            std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);
            std::string searchInput(searchQuery.get() ? searchQuery.get() : "");

            if (searchInput.empty()) {
                // Exit filter mode on blank Enter
                break;
            }
            if (filterISOs(searchInput)) {
				add_history(searchQuery.get());
				saveHistory(historyPattern);
				
                // Exit loop if valid filter terms yield results
                break;
            }   
        }
         historyPattern = false;
		clear_history();
        continue; // Return to the main prompt
    }

        // Prepare for unmounting
        std::vector<std::string>& currentDirs = isFiltered ? filteredIsoDirs : isoDirs;
        std::vector<int> selectedIndices;
        std::vector<std::string> selectedIsoDirs;

        // Handle selection logic
        if (inputString == "00") {
            selectedIsoDirs = currentDirs;
        } else {
            tokenizeInput(inputString, currentDirs, errorMessages, selectedIndices);
            
            for (int index : selectedIndices) {
                selectedIsoDirs.push_back(currentDirs[index - 1]);
            }
        }

        // Perform unmounting
        if (!selectedIsoDirs.empty()) {
            performUnmount(selectedIsoDirs);
            filteredIsoDirs.clear();
            isFiltered = false;
        } else {
			clearScrollBuffer();
            std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
            std::cout << "\n\033[1;32m↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }
}
