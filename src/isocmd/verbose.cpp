// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Main verbose print function for results
void verbosePrint(const std::set<std::string>& primarySet, const std::set<std::string>& secondarySet = {}, const std::set<std::string>& tertiarySet = {}, const std::set<std::string>& quaternarySet = {}, const std::set<std::string>& errorSet = {}, int printType = 0) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    clearScrollBuffer(); // Assuming this function is defined elsewhere

    // Helper lambda to print a set with optional color and output stream
    auto printSet = [](const std::set<std::string>& set, bool isError = false, bool addNewLineBefore = false) {
        if (!set.empty()) {
            if (addNewLineBefore) {
                std::cout << "\n";
            }
            for (const auto& item : set) {
                if (isError) {
                    // Red color for errors
                    std::cerr << "\n\033[1;91m" << item << "\033[0m\033[1m";
                } else {
                    std::cout << "\n" << item;
                }
            }
        }
    };

    // Determine print behavior based on type
    switch (printType) {
        case 0: // Unmounted
            // Unmounted: primarySet = unmounted files, secondarySet = unmounted errors, errorSet = error messages
            printSet(primarySet);
            printSet(secondarySet, false, !primarySet.empty());
            printSet(errorSet, true, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 1: // Operation
            // Operation: primarySet = operation ISOs, secondarySet = operation errors, errorSet = unique error messages
            printSet(primarySet, false);
            printSet(secondarySet, false, !primarySet.empty());
            printSet(errorSet, false, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 2: // Mounted
			// Mounted: primarySet = mounted files, secondarySet = skipped messages, 
			// tertiarySet = mounted fails, errorSet = unique error messages
			printSet(primarySet);
			printSet(tertiarySet, true, !primarySet.empty());
			if (primarySet.empty() && !tertiarySet.empty() && !secondarySet.empty()) {
				std::cout << "\n";
			}
			printSet(secondarySet, true, !primarySet.empty());
			printSet(errorSet, true, !primarySet.empty() || !secondarySet.empty() || !tertiarySet.empty());
			std::cout << "\n\n";
			break;

        case 3: // Conversion
            // Conversion: 
            // primarySet = processed errors
            // secondarySet = success outputs
            // tertiarySet = skipped outputs
            // quaternarySet = failed outputs
            // errorSet = deleted outputs
            std::cout << "\n";
            auto printWithNewline = [](const std::set<std::string>& outs) {
                for (const auto& out : outs) {
                    std::cout << out << "\033[0;1m\n";
                }
                if (!outs.empty()) {
                    std::cout << "\n";
                }
            };

            printWithNewline(secondarySet);   // Success outputs
            printWithNewline(tertiarySet);    // Skipped outputs
            printWithNewline(quaternarySet);  // Failed outputs
            printWithNewline(errorSet);       // Deleted outputs
            printWithNewline(primarySet);     // Processed errors
            break;
    }

    // Continuation prompt
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// CACHE

// Function that provides verbose output for manualRefreshCache
void verboseIsoCacheRefresh(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::set<std::string>& invalidPaths, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& historyPattern, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
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
		saveSuccess = saveCache(allIsoFiles, maxCacheSize, newISOFound);
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
void verboseFind(std::set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::set<std::string>& processedErrorsFind) {
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
void verboseSearchResults(const std::string& fileExtension, std::set<std::string>& fileNames, std::set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();

    auto end_time = std::chrono::high_resolution_clock::now();

    // Case: Files were found
    if (!fileNames.empty() && !g_operationCancelled.load()) {
        std::cout << "\n\n\033[1;92mFound " << fileNames.size() << " matching files.\033[1;93m " << currentCacheOld << " matching entries cached in RAM from previous searches.\033[0;1m\n\n";
    }

    // Case: No new files were found, but files exist in cache
    if (!newFilesFound && !files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n\033[1;91mNo new " << fileExtension << " files found. \033[1;92m";
        std::cout << files.size() << " matching entries are cached in RAM from previous searches, \033[1;94mls\033[1;92m ↵ in FolderPath prompt to display .\033[0;1m\n\n";
    }

    // Case: No files were found
    if (files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n\033[1;91mNo " << fileExtension << " files found in the specified paths or matching entries cached in RAM.\n\033[0;1m\n";
    }
    
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    std::cout << "\033[1mTime Elapsed: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n\n";
    
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
    return;
}
