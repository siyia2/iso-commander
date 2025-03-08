// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Main verbose print function for results
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int printType) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    clearScrollBuffer(); // Assuming this function is defined elsewhere

	// Lambda to move a set to a sorted vector and print
	auto printSortedSet = [](std::unordered_set<std::string>& set, bool isError = false) {
		if (!set.empty()) {
			std::vector<std::string> vec(
				std::make_move_iterator(set.begin()), 
				std::make_move_iterator(set.end())
			);
			sortFilesCaseInsensitive(vec);  // Changed this line to use the function directly
        
			std::cout << "\n";
			for (const auto& item : vec) {
				if (isError) {
					std::cerr << "\033[1;91m" << item << "\033[0m\033[1m\n";
				} else {
					std::cout << item << "\n";
				}
			}
		}
	};

    switch (printType) {
        case 0: // Unmounted
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, true);
            std::cout << "\n";
            break;
        }
        case 1: // Operation
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, false);
            std::cout << "\n";
            break;
        }
        case 2: // Mounted
        {
            printSortedSet(primarySet, false);
            printSortedSet(tertiarySet, true);
            printSortedSet(secondarySet, true);
            printSortedSet(errorSet, true);
            std::cout << "\n";
            break;
        }
        case 3: // Conversion
        {
            printSortedSet(secondarySet, false);   // Success outputs
            printSortedSet(tertiarySet, false);    // Skipped outputs
            printSortedSet(errorSet, false);       // Failed outputs
            printSortedSet(primarySet, false);     // Processed errors
            std::cout << "\n";
            break;
        }
    }
    
    // Continuation prompt
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to clear and de-allocate verbos sets
void resetVerboseSets(std::unordered_set<std::string>& processedErrors,std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts) {
    
    std::unordered_set<std::string>().swap(processedErrors);
    std::unordered_set<std::string>().swap(successOuts);
    std::unordered_set<std::string>().swap(skippedOuts);
    std::unordered_set<std::string>().swap(failedOuts);

}



// Function to handle error reporting for Cp/Mv/Rm
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile,const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc) {
    
    std::string errorMsg;
    
    if (errorType == "same_file") {
        errorMsg = "\033[1;91mCannot " + operation + " file to itself: \033[1;93m'" +
                   srcDir + "/" + srcFile + "'\033[1;91m.\033[0m";
    }
    else if (errorType == "invalid_dest") {
        errorMsg = "\033[1;91mError " + operation + ": \033[1;93m'" + 
                   srcDir + "/" + srcFile + "'\033[1;91m to '" + 
                   destDir + "': " + errorDetail + "\033[1;91m.\033[0;1m";
    }
    else if (errorType == "source_missing") {
        errorMsg = "\033[1;91mSource file no longer exists: \033[1;93m'" +
                   srcDir + "/" + srcFile + "'\033[1;91m.\033[0;1m";
    }
    else if (errorType == "overwrite_failed") {
        errorMsg = "\033[1;91mFailed to overwrite: \033[1;93m'" +
                   destDir + "/" + srcFile + "'\033[1;91m - " + 
                   errorDetail + ".\033[0;1m";
    }
    else if (errorType == "file_exists") {
        errorMsg = "\033[1;91mError " + operation + ": \033[1;93m'" + 
                   srcDir + "/" + srcFile + "'\033[1;91m to '" + 
                   destDir + "/': File exists (enable overwrites)\033[1;91m.\033[0;1m";
    }
    else if (errorType == "remove_after_move") {
        errorMsg = "\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                   srcDir + "/" + srcFile + "'\033[1;91m - " +
                   errorDetail + "\033[0m";
    }
    else if (errorType == "missing_file") {
        errorMsg = "\033[1;35mMissing: \033[1;93m'" +
                   srcDir + "/" + srcFile + "'\033[1;35m.\033[0;1m";
    }
    else {
        // Generic error case
        errorMsg = "\033[1;91mError: " + errorDetail + "\033[0;1m";
    }
    
    verboseErrors.push_back(errorMsg);
    failedTasks->fetch_add(1, std::memory_order_acq_rel);
    operationSuccessful.store(false);
    
    // Call the batch insert function to handle message batching
    batchInsertFunc();
}


// CACHE

// Function that provides verbose output for manualRefreshCache
void verboseIsoCacheRefresh(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
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
    manualRefreshCache(initialDir, promptFlag, maxDepth, filterHistory, newISOFound);
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
