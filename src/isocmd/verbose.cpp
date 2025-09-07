// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


// Global mutex to protect the verbose sets
std::mutex globalSetsMutex;


// Main verbose print function for results
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int printType) {
    // Ignore SIGINT (Ctrl+C) to prevent interruptions
    signal(SIGINT, SIG_IGN);
    
    // Disable Ctrl+D to avoid accidental termination (assuming this function is defined elsewhere)
    disable_ctrl_d();

    // Clear terminal scroll buffer to ensure clean output display
    clearScrollBuffer(); 

	// Lambda function to move elements from an unordered_set to a sorted vector and print them
	auto printSortedSet = [](std::unordered_set<std::string>& set, bool isError = false) {
		if (!set.empty()) {
			// Move elements from the set into a vector
			std::vector<std::string> vec(
				std::make_move_iterator(set.begin()), 
				std::make_move_iterator(set.end())
			);
			
			// Sort vector contents in a case-insensitive manner
			sortFilesCaseInsensitive(vec);
        
			std::cout << "\n";
			
			// Print elements to either stdout or stderr based on isError flag
			for (const auto& item : vec) {
				if (isError) {
					// Print errors in bold red
					std::cerr << "\033[1;91m" << item << "\033[0m\033[1m\n";
				} else {
					// Print regular entries normally
					std::cout << item << "\n";
				}
			}
		}
	};

    // Select the printing behavior based on printType
    switch (printType) {
        case 0: // Unmounted state: Print primary and secondary normally, errors in red
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, true); 
            std::cout << "\n";
            break;
        }
        case 1: // Operation state: Print everything normally
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, false);
            std::cout << "\n";
            break;
        }
        case 2: // Mounted state: Print primary normally, all others (tertiary, secondary, errors) in red
        {
            printSortedSet(primarySet, false);
            printSortedSet(tertiarySet, true);
            printSortedSet(secondarySet, true);
            printSortedSet(errorSet, true);
            std::cout << "\n";
            break;
        }
        case 3: // Conversion state: Print in a specific order
        {
            printSortedSet(secondarySet, false);   // Success outputs
            printSortedSet(tertiarySet, false);    // Skipped outputs
            printSortedSet(errorSet, false);       // Failed outputs
            printSortedSet(primarySet, false);     // Processed errors
            std::cout << "\n";
            break;
        }
    }
    
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";  
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}



// Function to clear and de-allocate verbose sets
void resetVerboseSets(std::unordered_set<std::string>& processedErrors,std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts) {
    // Clear the verbose sets
	processedErrors.clear();
	successOuts.clear();
	skippedOuts.clear();
	failedOuts.clear();

}



//Function to disblay errors from tokenization
void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages) {
    // Display user input errors at the top
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
        for (const auto& err : uniqueErrorMessages) {
            std::cout << err << "\n";
        }
        uniqueErrorMessages.clear();
    }
}


// CP/MV/RM

// Function to handle error reporting for Cp/Mv/Rm
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile,const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc) {
    
    std::string errorMsg;
    
    if (errorType == "same_file") {
        errorMsg = "\033[1;91mCannot " + operation + " file to itself: \033[1;93m'" +
                   srcDir + "/" + srcFile + "'\033[1;91m.\033[0m";
    }
    else if (errorType == "invalid_dest") {
        errorMsg = "\033[1;91mError " + operation + ": \033[1;93m'" + 
                   (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[1;91m to '" + 
                   destDir + "': " + errorDetail + "\033[1;91m.\033[0;1m";
    }
    else if (errorType == "source_missing") {
        errorMsg = "\033[1;91mSource file no longer exists: \033[1;93m'" +
                   (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[1;91m.\033[0;1m";
    }
    else if (errorType == "overwrite_failed") {
        errorMsg = "\033[1;91mFailed to overwrite: \033[1;93m'" +
                   destDir + "/" + srcFile + "'\033[1;91m - " + 
                   errorDetail + ".\033[0;1m";
    }
    else if (errorType == "file_exists") {
        errorMsg = "\033[1;91mError " + operation + ": \033[1;93m'" + 
                   (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[1;91m to '" + 
                   destDir + "/': File exists (\033[1;93menable overwrites\033[1;91m).\033[0;1m";
    }
    else if (errorType == "remove_after_move") {
        errorMsg = "\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                   (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[1;91m - " +
                   errorDetail + "\033[0m";
    }
    else if (errorType == "missing_file") {
        errorMsg = "\033[1;35mMissing: \033[1;93m'" +
                   (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[1;35m.\033[0;1m";
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


// ISO DATABASE

// Function to count differences between new and old ISO files
int countDifferentEntries(const std::vector<std::string>& allIsoFiles, const std::vector<std::string>& globalIsoFileList) {
    // Use string_view to avoid extra allocations
    std::unordered_set<std::string_view> globalSet;
    globalSet.reserve(globalIsoFileList.size());  // Reserve memory to avoid rehashing

    for (const auto& file : globalIsoFileList) {
        globalSet.insert(file);  // Insert string_views pointing to existing strings
    }

    int count = 0;
    for (const auto& file : allIsoFiles) {
        if (globalSet.find(file) == globalSet.end()) {
            count++;
        }
    }

    return count;
}


// Function that provides verbose output for refreshForDatabase
void verboseForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    
    auto printInvalidPaths = [&]() {
        if (invalidPaths.empty()) return;
        if (totalFiles == 0 && validPaths.empty()) {
            std::cout << "\r\033[0;1mTotal files processed: 0\n" << std::flush;
        }
        std::cout << "\n\033[0;1mInvalid paths omitted from search: \033[1;91m";
        for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
            std::cout << "'" << *it << "'";
            std::cout << (++it != invalidPaths.end() ? " " : "");
        }
        std::cout << "\033[0;1m.\n";
    };

    auto printErrorMessages = [&]() {
        if (uniqueErrorMessages.empty()) return;
        for (const auto& error : uniqueErrorMessages) std::cout << error;
        std::cout << "\n";
    };

    if (promptFlag && (!uniqueErrorMessages.empty() || !invalidPaths.empty())) {
        printInvalidPaths();
        printErrorMessages();
    }

    const bool saveSuccess = g_operationCancelled ? false : saveToDatabase(allIsoFiles, newISOFound);
    const auto end_time = std::chrono::high_resolution_clock::now();
    
    if (!promptFlag) return;

    const double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << "\033[1m\nTotal time taken: " << std::fixed << std::setprecision(1) 
              << total_elapsed << " seconds\033[0;1m\n";

    if (g_operationCancelled) {
        std::cout << "\n\033[1;93mDatabase refresh cancelled.\033[0;1m\n";
    } else if (!allIsoFiles.empty() && newISOFound.load() && !saveSuccess) {
        std::cout << "\n\033[1;91mDatabase refresh failed: Unable to access the database file.\033[0;1m\n";
    } else if (validPaths.empty()) {
        std::cout << "\n\033[1;91mDatabase refresh failed: Lack of valid paths.\033[0;1m\n";
	} else if (!allIsoFiles.empty() && !newISOFound.load() && !saveSuccess){
        std::cout << "\n\033[1;92mDatabase refresh:\033[1;93m No new ISO found\033[0;1m\n";
    } else if (allIsoFiles.empty()){
        std::cout << "\n\033[1;92mDatabase refresh:\033[1;91m No ISO found\033[0;1m\n";
    } else if (!allIsoFiles.empty() && saveSuccess && newISOFound.load()){
		int result = countDifferentEntries(allIsoFiles, globalIsoFileList);
		loadFromDatabase(globalIsoFileList);
		std::cout << "\n\033[1;92mDatabase refreshed successfully: \033[1;95m" << result << "\033[1;92m new ISO imported\033[0;1m\n";
	}

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string initialDir = ""; // Create a std::string object
	refreshForDatabase(initialDir, promptFlag, maxDepth, filterHistory, newISOFound);
}


// IMAGE FILE SEARCHING VERBOSITY

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
