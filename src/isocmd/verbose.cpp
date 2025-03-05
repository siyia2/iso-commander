// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Main verbose print function for results
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& quaternarySet, std::unordered_set<std::string>& errorSet, 
int printType) {

    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    clearScrollBuffer(); // Assuming this function is defined elsewhere

    // Lambda to move an unordered_set to a vector, sort it, and print it
    auto printSortedSet = [&](std::unordered_set<std::string>&& set, bool isError = false, bool addNewLineBefore = false) {
        if (!set.empty()) {
            std::vector<std::string> vec(std::make_move_iterator(set.begin()), std::make_move_iterator(set.end()));
            sortFilesCaseInsensitive(vec); // Sort the vector case-insensitively
            if (addNewLineBefore) {
                std::cout << "\n";
            }
            for (const auto& item : vec) {
                if (isError) {
                    std::cerr << "\n\033[1;91m" << item << "\033[0m\033[1m";
                } else {
                    std::cout << "\n" << item;
                }
            }
        }
    };

    switch (printType) {
        case 0: // Unmounted
            printSortedSet(std::move(primarySet));
            printSortedSet(std::move(secondarySet), false, !primarySet.empty());
            printSortedSet(std::move(errorSet), true, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 1: // Operation
            printSortedSet(std::move(primarySet));
            printSortedSet(std::move(secondarySet), false, !primarySet.empty());
            printSortedSet(std::move(errorSet), false, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 2: // Mounted
            printSortedSet(std::move(primarySet));
            printSortedSet(std::move(tertiarySet), true, !primarySet.empty());
            if (primarySet.empty() && !tertiarySet.empty() && !secondarySet.empty()) {
                std::cout << "\n";
            }
            printSortedSet(std::move(secondarySet), true, !primarySet.empty());
            printSortedSet(std::move(errorSet), true, !primarySet.empty() || !secondarySet.empty() || !tertiarySet.empty());
            std::cout << "\n\n";
            break;

        case 3: // Conversion
            std::cout << "\n";
            auto printSortedWithNewline = [&](std::unordered_set<std::string>&& outs) {
                if (!outs.empty()) {
                    std::vector<std::string> vec(std::make_move_iterator(outs.begin()), std::make_move_iterator(outs.end()));
                    sortFilesCaseInsensitive(vec);
                    for (const auto& out : vec) {
                        std::cout << out << "\033[0;1m\n";
                    }
                    std::cout << "\n";
                }
            };

            printSortedWithNewline(std::move(secondarySet));   // Success outputs
            printSortedWithNewline(std::move(tertiarySet));    // Skipped outputs
            printSortedWithNewline(std::move(quaternarySet));  // Failed outputs
            printSortedWithNewline(std::move(errorSet));       // Deleted outputs
            printSortedWithNewline(std::move(primarySet));     // Processed errors
            break;
    }

    // Continuation prompt
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// CACHE

// Function that provides verbose output for manualRefreshCache
void verboseIsoCacheRefresh(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& historyPattern, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
	bool saveSuccess;
	
	// Print invalid paths
    if ((!uniqueErrorMessages.empty() || !invalidPaths.empty()) && promptFlag) {
		if (!invalidPaths.empty()) {
			if (totalFiles == 0 && validPaths.empty()) {
				std::cout << "\r\033[0;1mTotal files processed: 0\n" << std::flush;
			}
			std::cout << "\n\033[0;1mInvalid paths omitted from search: \033[1;91m";
			auto it = invalidPaths.begin();
			while (it != invalidPaths.end()) {
				std::cout << "'" << *it << "'";
				++it;
				if (it != invalidPaths.end()) {
					std::cout << " ";  // Add space between paths, but not after the last one.
				}
			}
			std::cout << "\033[0;1m.\n";
		}
    

		for (const auto& error : uniqueErrorMessages) {
			std::cout << error;
		}
		if (!uniqueErrorMessages.empty()) {
			std::cout << "\n";
		}
	}
	
	if (g_operationCancelled) {
		saveSuccess = false;
	} else {
		saveSuccess = saveCache(allIsoFiles, newISOFound);
	}

    // Stop the timer after completing the cache refresh and removal of non-existent paths
    auto end_time = std::chrono::high_resolution_clock::now();
    
    if (promptFlag) {

    if (!validPaths.empty() || (!invalidPaths.empty() && validPaths.empty())) {
    std::cout << "\n";
	}
	// Calculate and print the elapsed time
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();

    // Print the time taken for the entire process in bold with one decimal place
    std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";

    // Inform the user about the cache refresh status
    if (saveSuccess && !validPaths.empty() && invalidPaths.empty() && uniqueErrorMessages.empty()) {
        std::cout << "\n";
        std::cout << "\033[1;92mCache refreshed successfully.\033[0;1m";
        std::cout << "\n";
    }
    if (saveSuccess && !validPaths.empty() && (!invalidPaths.empty() || !uniqueErrorMessages.empty())) {
        std::cout << "\n";
        std::cout << "\033[1;93mCache refreshed with some errors.\033[0;1m";
        std::cout << "\n";
    }
    if (saveSuccess && validPaths.empty() && !invalidPaths.empty()) {
        std::cout << "\n";
        std::cout << "\033[1;91mCache refresh failed due to lack of valid paths.\033[0;1m";
        std::cout << "\n";
    }
    if (!saveSuccess && !g_operationCancelled) {
        std::cout << "\n";
        std::cout << "\033[1;91mCache refresh failed. Unable to write to the cache file.\033[0;1m";
        std::cout << "\n";
    }
    if (!saveSuccess && g_operationCancelled) {
        std::cout << "\n";
        std::cout << "\033[1;93mCache refresh cancelled.\033[0;1m";
        std::cout << "\n";
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string initialDir = "";
    manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
	}
}


// CONVERSIONS

// Function to print invalid directory paths from search
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
	
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
		std::cout << "\n\n"; // Print two newlines at the start
		auto it = processedErrorsFind.begin(); // Iterator to the first element
		while (it != processedErrorsFind.end()) {
			std::cout << *it; // Dereference the iterator to get the element
			++it; // Move to the next element
			if (it != processedErrorsFind.end()) {
				std::cout << "\n"; // Print newline only if it's not the last element
			}
		}
	}
	processedErrorsFind.clear();
	invalidDirectoryPaths.clear();
}


// Function that handles verbose results and timing from select select_and_convert_files_to_iso
void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::unordered_set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();

    auto end_time = std::chrono::high_resolution_clock::now();

    // Case: Files were found
    if (!fileNames.empty() && !g_operationCancelled.load()) {
        std::cout << "\n\n\033[1;92m" << fileNames.size() << " \033[1;38;5;208m{" << fileExtension << "} \033[1;92mfiles found\033[1;93m\n" << currentCacheOld << " \033[1;38;5;208m{" << fileExtension << "} \033[1;93mcached entries\033[0;1m\n\n";
    }

    // Case: No new files were found, but files exist in cache
    if (!newFilesFound && !files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n\033[1;91m0 \033[1;38;5;208m{" << fileExtension << "} \033[1;91mfiles found \033[1;93m\n";
        std::cout << files.size() << " \033[1;38;5;208m{" << fileExtension << "} \033[1;93mcached entries | \033[1;94mls \033[1;93m↵ to list\033[0;1m\n\n";
    }

    // Case: No files were found
    if (files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n\033[1;91m0\033[1;38;5;208m {" << fileExtension << "} \033[1;91mfiles found\n\033[1;93m0\033[1;38;5;208m {" << fileExtension << "} \033[1;93mcached entries\n\033[0;1m\n";
    }
    
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
    
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
    return;
}
