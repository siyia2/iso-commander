// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"


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
    
    std::cout << color << "↵ to continue..." << reset; 
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
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile, 
                       const std::string& destDir, const std::string& errorDetail, const std::string& operation, 
                       std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, 
                       std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc) {

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // Simplify color selection using your new struct
    std::string_view errLabel     = isOriginal ? originalColors::red     : theme->secondary;
    std::string_view errPath      = isOriginal ? originalColors::yellow  : theme->warning;
    std::string_view missingLabel = isOriginal ? originalColors::purple  : theme->secondary;
    
    const std::string displaySrc  = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    std::string errorMsg;
    errorMsg.reserve(256); // Increased slightly to prevent reallocs with long paths

    // Logic Mapping
    if (errorType == "same_file") {
        errorMsg.append(errLabel).append("Cannot ").append(operation).append(" file to itself: ")
                .append(errPath).append("'").append(srcDir).append("/").append(srcFile).append("'")
                .append(originalColors::reset).append(errLabel).append(".")
                .append(originalColors::reset);
    }
    else if (errorType == "invalid_dest") {
        errorMsg.append(errLabel).append("Error ").append(operation).append(": ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::reset).append(errLabel).append(" to '").append(destDir).append("': ").append(errorDetail).append(".")
                .append(originalColors::reset).append(originalColors::boldAlt);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(errLabel).append("Source file no longer exists: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::reset).append(errLabel).append(".")
                .append(originalColors::reset).append(originalColors::boldAlt);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(errLabel).append("Failed to overwrite: ")
                .append(errPath).append("'").append(destDir).append("/").append(srcFile).append("'")
                .append(originalColors::reset).append(errLabel).append(" - ").append(errorDetail).append(".")
                .append(originalColors::reset).append(originalColors::boldAlt);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(errLabel).append("Error ").append(operation).append(": ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::reset).append(errLabel).append(" to '").append(destDir).append("/': File exists (")
                .append(errPath).append("enable overwrites")
                .append(originalColors::reset).append(errLabel).append(").")
                .append(originalColors::reset).append(originalColors::boldAlt);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(errLabel).append("Move completed but failed to remove source file: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::reset).append(errLabel).append(" - ").append(errorDetail)
                .append(originalColors::reset);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(missingLabel).append("Missing: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::reset).append(missingLabel).append(".")
                .append(originalColors::reset).append(originalColors::boldAlt);
    }
    else {
        errorMsg.append(errLabel).append("Error: ").append(errorDetail)
                .append(originalColors::reset).append(originalColors::boldAlt);
    }

    // State Updates
    verboseErrors.push_back(std::move(errorMsg));
    failedTasks->fetch_add(1, std::memory_order_acq_rel);
    operationSuccessful.store(false, std::memory_order_release);
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

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // Map to global struct
    std::string_view errLabel    = isOriginal ? originalColors::red       : theme->secondary;
    std::string_view warnLabel   = isOriginal ? originalColors::yellow    : theme->warning;
    std::string_view okLabel     = isOriginal ? originalColors::green     : theme->accent;
    std::string_view importColor = isOriginal ? originalColors::magenta   : theme->highlight;
    std::string_view boldLabel   = isOriginal ? originalColors::boldAlt   : theme->muted;

    loadFromDatabase(globalIsoFileList);

    auto printInvalidPaths = [&]() {
        if (invalidPaths.empty()) return;
        if (totalFiles == 0 && validPaths.empty()) {
            std::cout << "\r" << boldLabel << "Total files processed: 0\n" << std::flush;
        }
        std::cout << "\n" << boldLabel << "Invalid paths omitted from search: " << errLabel;
        for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
            std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
        }
        std::cout << boldLabel << ".\n";
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
    std::cout << boldLabel << "\nTotal time taken: " << std::fixed << std::setprecision(1)
              << total_elapsed << " seconds\n";

    // Result Logic
    if (g_operationCancelled) {
        std::cout << "\n" << okLabel << "Database Refresh: [" << warnLabel << "Cancelled" << okLabel << "]" << boldLabel << "\n";
    } else if (!allIsoFiles.empty() && newISOFound.load() && !saveSuccess) {
        std::cout << "\n" << errLabel << "Database Refresh failed: [" << warnLabel << "Unable to access the database file" << errLabel << "]" << boldLabel << "\n";
    } else if (validPaths.empty()) {
        std::cout << "\n" << errLabel << "Database refresh failed: [" << warnLabel << "Lack of valid paths" << errLabel << "]" << boldLabel << "\n";
    } else if (!allIsoFiles.empty() && !newISOFound.load() && !saveSuccess) {
        std::cout << "\n" << okLabel << "Database Refresh: [" << warnLabel << "No new ISO found" << okLabel << "]" << boldLabel << "\n";
    } else if (allIsoFiles.empty()) {
        std::cout << "\n" << okLabel << "Database Refresh: [" << warnLabel << "No ISO found" << okLabel << "]" << boldLabel << "\n";
    } else if (!allIsoFiles.empty() && saveSuccess && newISOFound.load()) {
        int result = countDifferentEntries(allIsoFiles, globalIsoFileList);
        std::cout << "\n" << okLabel << "Database Refresh: [" << importColor << result << " ISO imported" << okLabel << "]" << boldLabel << "\n";
    }

    std::cout << color << "\n↵ to continue..." << reset; 
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string initialDir = ""; 
    refreshForDatabase(initialDir, promptFlag, maxDepth, filterHistory, newISOFound);
}


// IMAGE FILE SEARCHING VERBOSITY

// Function to print invalid directory paths from search
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view boldLabel = isOriginal ? originalColors::boldAlt : theme->muted;
    std::string_view errLabel  = isOriginal ? "\033[31m"              : theme->secondary; // Keeping your specific non-bold red

    if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
        std::cout << "\r" << boldLabel << "Total files processed: 0" << std::flush;
    }

    if (!invalidDirectoryPaths.empty()) {
        std::cout << "\n\n" << boldLabel << "Invalid paths omitted from search: " << errLabel;
        for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
            std::cerr << "'" << *it << "'" << (std::next(it) != invalidDirectoryPaths.end() ? " " : "");
        }
        std::cerr << boldLabel << ".";
    }

    if (!processedErrorsFind.empty()) {
        std::cout << "\n\n";
        for (const auto& error : processedErrorsFind) {
            std::cout << error << "\n";
        }
    }

    processedErrorsFind.clear();
    invalidDirectoryPaths.clear();
}


// Function that handles verbose results and timing from select select_and_convert_files_to_iso
void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::unordered_set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view okLabel   = isOriginal ? originalColors::green  : theme->accent;
    std::string_view errLabel  = isOriginal ? originalColors::red    : theme->secondary;
    std::string_view warnLabel = isOriginal ? originalColors::yellow : theme->warning;
    std::string_view extColor  = isOriginal ? originalColors::orange : theme->highlight;
    std::string_view lsColor   = isOriginal ? originalColors::blue   : theme->primary;
    std::string_view boldLabel = isOriginal ? originalColors::bold   : theme->muted;

    auto end_time = std::chrono::high_resolution_clock::now();

    if (g_operationCancelled.load()) return;

    // Case: Files were found
    if (!fileNames.empty()) {
        std::cout << "\n\n"
                  << okLabel   << fileNames.size() << " "
                  << extColor  << "{" << fileExtension << "} "
                  << okLabel   << "files found" << warnLabel << "\n"
                  << currentCacheOld << " "
                  << extColor  << "{" << fileExtension << "} "
                  << warnLabel << "cached entries" << originalColors::boldAlt << "\n\n";
    }

    // Case: No new files found, but cache exists
    if (!newFilesFound && !files.empty() && !list) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n"
                  << errLabel  << "0 "
                  << extColor  << "{" << fileExtension << "} "
                  << errLabel  << "files found " << warnLabel << "\n"
                  << files.size() << " "
                  << extColor  << "{" << fileExtension << "} "
                  << warnLabel << "cached entries | "
                  << lsColor   << "ls "
                  << warnLabel << "↵ to list" << originalColors::boldAlt << "\n\n";
    }

    // Case: Total emptiness
    if (files.empty() && !list) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n"
                  << errLabel  << "0" << extColor << " {" << fileExtension << "} " << errLabel << "files found\n"
                  << warnLabel << "0" << extColor << " {" << fileExtension << "} " << warnLabel << "cached entries\n"
                  << originalColors::boldAlt << "\n";
    }

    auto total_elapsed_time = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << boldLabel << "Time Elapsed: " << std::fixed << std::setprecision(1)
              << total_elapsed_time << " seconds" << originalColors::boldAlt << "\n\n";
    
    std::cout << color << "↵ to continue..." << reset; 
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}
