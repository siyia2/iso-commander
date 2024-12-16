// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF


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
    // Early root privilege check
    if (geteuid() != 0) {
        std::lock_guard<std::mutex> lowLock(Mutex4Low);
        for (const auto& isoDir : isoDirs) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDir
                         << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            unmountedErrors.emplace(errorMessage.str());
        }
        return;
    }

    // Parallel execution for unmounting
    std::vector<std::pair<std::string, int>> unmountResults;
    {
        std::vector<std::thread> threads;
        std::mutex resultMutex;

        for (const auto& isoDir : isoDirs) {
            threads.emplace_back([&](const std::string& dir) {
                int result = umount2(dir.c_str(), MNT_DETACH);
                std::lock_guard<std::mutex> lock(resultMutex);
                unmountResults.emplace_back(dir, result);
            }, isoDir);
        }

        for (auto& thread : threads) {
            thread.join();
        }
    }

    // Process unmount results
    std::vector<std::string> successfulUnmounts;
    std::vector<std::string> failedUnmounts;

    for (const auto& [dir, result] : unmountResults) {
        bool isEmpty = isDirectoryEmpty(dir);
        
        // If unmount fails but directory is empty, treat it as a potential removal
        if (result != 0 && !isEmpty) {
            failedUnmounts.push_back(dir);
        } else {
            successfulUnmounts.push_back(dir);
        }
    }

    // Handle successful and unsuccessful unmounts, with special focus on /mnt/iso_ directories
    {
        std::lock_guard<std::mutex> lowLock(Mutex4Low);
        
        // Try to remove all successful unmount directories
        for (const auto& dir : successfulUnmounts) {
            if (isDirectoryEmpty(dir)) {
                if (rmdir(dir.c_str()) == 0) {
                    std::string removedDirInfo = "\033[0;1mUnmounted: \033[1;92m'" + dir + "\033[1;92m'\033[0m.";
                    unmountedFiles.emplace(removedDirInfo);
                }
            }
        }

        // Additional pass to remove empty /mnt/iso_* directories
        for (const auto& dir : isoDirs) {
            // Check if directory starts with /mnt/iso_ and is empty
            if (dir.find("/mnt/iso_") == 0 && isDirectoryEmpty(dir)) {
                if (rmdir(dir.c_str()) == 0) {
                    std::string removedDirInfo = "\033[0;1mRemoved empty ISO directory: \033[1;92m'" + dir + "\033[1;92m'\033[0m.";
                    unmountedFiles.emplace(removedDirInfo);
                }
            }
        }

        // Handle failed unmounts (now only for non-empty directories)
        for (const auto& dir : failedUnmounts) {
            std::stringstream errorMessage;
            
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << dir 
                         << "\033[1;93m'\033[1;91m.\033[0;1m {notAnISO}";
            unmountedErrors.emplace(errorMessage.str());
        }
    }
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
        sortFilesCaseInsensitive(isoDirs);
    };

    auto displayMountedISOs = [&]() {
        clearScrollBuffer();
        if (filteredIsoDirs.size() == isoDirs.size()) {
				isFiltered = false;
		}
        printList(isFiltered ? filteredIsoDirs : isoDirs, "MOUNTED_ISOS");
    };

    auto handleNoMountedISOs = []() {
        clearScrollBuffer();
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_*' pattern found.\033[0m\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    auto performUnmount = [&](const std::vector<std::string>& selectedIsoDirs) {
		clearScrollBuffer();
		std::cout << "\n\033[0;1m";
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
            verbosePrint(unmountedFiles, unmountedErrors, {}, {}, errorMessages, 0);
            
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
		sortFilesCaseInsensitive(filteredIsoDirs);
		isFiltered = true;
		return true;
	};

    while (true) {
		verbose = false;
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
				if (strcmp(searchQuery.get(), "/") != 0){
					add_history(searchQuery.get());
					saveHistory(historyPattern);
				}
				
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
