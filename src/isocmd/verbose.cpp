// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @brief Performs a high-visibility print of operation results categorized by sets.
 * @details Handles signal management, terminal cleanup, and sorted output of 
 * success, warning, and error strings based on a specific operation context.
 * * @param primarySet Main data set (usually processed items).
 * @param secondarySet Supporting data set (usually successes).
 * @param tertiarySet Additional context (usually skipped items).
 * @param errorSet Set of error strings.
 * @param printType UI mode: 0 (Unmounted), 1 (Ops), 2 (Mounted), 3 (Conversion).
 */
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int printType) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer(); 
    
    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    auto printSortedSet = [&](std::unordered_set<std::string>& set, bool isError = false) {
        if (!set.empty()) {
            std::vector<std::string> vec(
                std::make_move_iterator(set.begin()), 
                std::make_move_iterator(set.end())
            );
            
            sortFilesCaseInsensitive(vec);
            std::cout << "\n";
            
            for (const auto& item : vec) {
                if (isError) {
                    std::cerr << vt.red << item << vt.reset << "\n";
                } else {
                    std::cout << item << "\n";
                }
            }
        }
    };

    switch (printType) {
        case 0:
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, true); 
            std::cout << "\n";
            break;
        }
        case 1:
        {
            printSortedSet(primarySet, false);
            printSortedSet(secondarySet, false);
            printSortedSet(errorSet, false);
            std::cout << "\n";
            break;
        }
        case 2:
        {
            printSortedSet(primarySet, false);
            printSortedSet(tertiarySet, true);
            printSortedSet(secondarySet, true);
            printSortedSet(errorSet, true);
            std::cout << "\n";
            break;
        }
        case 3:
        {
            printSortedSet(secondarySet, false);
            printSortedSet(tertiarySet, false);
            printSortedSet(errorSet, false);
            printSortedSet(primarySet, false);
            std::cout << "\n";
            break;
        }
    }
    
    std::cout << color << "↵ to continue..." << vt.reset; 
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Clears and deallocates memory for all verbose reporting sets.
 */
void resetVerboseSets(std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts) {
    processedErrors.clear();
    successOuts.clear();
    skippedOuts.clear();
    failedOuts.clear();
}

/**
 * @brief Displays a collection of unique error messages generated during tokenization.
 */
void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages) {
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
        for (const auto& err : uniqueErrorMessages) {
            std::cout << err << "\n";
        }
        uniqueErrorMessages.clear();
    }
}

/**
 * @brief Generates and logs color-coded error messages for file operations (CP, MV, RM).
 * @details Utilizes the current theme to construct a human-readable error string 
 * and updates atomic operation counters.
 */
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile, 
                       const std::string& destDir, const std::string& errorDetail, const std::string& operation, 
                       std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, 
                       std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc) {
    
    const VerboseAndDatabaseTheme vt = getVerboseTheme();
    const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    std::string errorMsg;
    errorMsg.reserve(256);

    if (errorType == "same_file") {
        errorMsg.append(vt.red).append("Cannot ").append(operation).append(" file to itself: ")
                .append(vt.red).append("'").append(srcDir).append("/").append(srcFile).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "invalid_dest") {
        errorMsg.append(vt.red).append("Error ").append(operation).append(": ")
                .append(vt.red).append("'").append(displaySrc).append("'")
                .append(vt.reset).append(vt.red).append(" to '").append(destDir).append("': ").append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(vt.red).append("Source file no longer exists: ")
                .append(vt.red).append("'").append(displaySrc).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(vt.red).append("Failed to overwrite: ")
                .append(vt.red).append("'").append(destDir).append("/").append(srcFile).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(vt.red).append("Error ").append(operation).append(": ")
                .append(vt.red).append("'").append(displaySrc).append("'")
                .append(vt.reset).append(vt.red).append(" to '").append(destDir).append("/': File exists (")
                .append(vt.red).append("enable overwrites")
                .append(vt.reset).append(vt.red).append(").")
                .append(vt.reset);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(vt.red).append("Move completed but failed to remove source file: ")
                .append(vt.red).append("'").append(displaySrc).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail)
                .append(vt.reset);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(vt.purple).append("Missing: ")
                .append(vt.red).append("'").append(displaySrc).append("'")
                .append(vt.reset).append(vt.purple).append(".")
                .append(vt.reset);
    }
    else {
        errorMsg.append(vt.red).append("Error: ").append(errorDetail)
                .append(vt.reset);
    }

    verboseErrors.push_back(std::move(errorMsg));
    failedTasks->fetch_add(1, std::memory_order_acq_rel);
    operationSuccessful.store(false, std::memory_order_release);
    batchInsertFunc();
}

/**
 * @brief Identifies items present in the current search that do not exist in the cached list.
 * @return Integer count of new/different entries found.
 */
int countDifferentEntries(const std::vector<std::string>& allIsoFiles, const std::vector<std::string>& globalIsoFileList) {
    std::unordered_set<std::string_view> globalSet;
    globalSet.reserve(globalIsoFileList.size());

    for (const auto& file : globalIsoFileList) {
        globalSet.insert(file);
    }

    int count = 0;
    for (const auto& file : allIsoFiles) {
        if (globalSet.find(file) == globalSet.end()) {
            count++;
        }
    }

    return count;
}

/**
 * @brief Handles verbose output and result logic for the ISO database refresh process.
 * @details Summarizes time taken, files imported, and displays any path errors encountered.
 */
void verboseForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    loadFromDatabase(globalIsoFileList);

    auto printInvalidPaths = [&]() {
        if (invalidPaths.empty()) return;
        if (totalFiles == 0 && validPaths.empty()) {
            std::cout << "\r" << vt.bold << "Total files processed: 0\n" << std::flush;
        }
        std::cout << "\n" << vt.bold << "Invalid paths omitted from search: " << vt.red;
        for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
            std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
        }
        std::cout << vt.bold << ".\n";
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
    std::cout << vt.bold << "\nTotal time taken: " << std::fixed << std::setprecision(1)
              << total_elapsed << " seconds\n";

    if (g_operationCancelled) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "Cancelled" << vt.green << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && newISOFound.load() && !saveSuccess) {
        std::cout << "\n" << vt.red << "Database Refresh failed: [" << vt.yellow << "Unable to access the database file" << vt.red << "]" << vt.bold << "\n";
    } else if (validPaths.empty()) {
        std::cout << "\n" << vt.red << "Database refresh failed: [" << vt.yellow << "Lack of valid paths" << vt.red << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && !newISOFound.load() && !saveSuccess) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No new ISO found" << vt.green << "]" << vt.bold << "\n";
    } else if (allIsoFiles.empty()) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No ISO found" << vt.green << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && saveSuccess && newISOFound.load()) {
        int result = countDifferentEntries(allIsoFiles, globalIsoFileList);
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << result << " ISO imported" << vt.green << "]" << vt.bold << "\n";
    }

    std::cout << color << "\n↵ to continue..." << vt.reset; 
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
}

/**
 * @brief Prints directory paths and specific errors encountered during a filesystem search.
 */
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
        std::cout << "\r" << vt.bold << "Total files processed: 0" << std::flush;
    }

    if (!invalidDirectoryPaths.empty()) {
        std::cout << "\n\n" << vt.bold << "Invalid paths omitted from search: " << vt.red;
        for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
            std::cerr << "'" << *it << "'" << (std::next(it) != invalidDirectoryPaths.end() ? " " : "");
        }
        std::cerr << vt.bold << ".";
    }

    if (!processedErrorsFind.empty()) {
        std::cout << "\n\n";
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

/**
 * @brief Displays a summary of image file search results, including cache status and time elapsed.
 */
void verboseSearchResults(const std::string& fileExtension,
                          std::unordered_set<std::string>& fileNames,
                          std::unordered_set<std::string>& invalidDirectoryPaths,
                          bool newFilesFound,
                          bool list,
                          int currentCacheOld,
                          const std::vector<std::string>& files,
                          const std::chrono::high_resolution_clock::time_point& start_time,
                          std::unordered_set<std::string>& processedErrorsFind,
                          std::vector<std::string>& directoryPaths) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    auto end_time = std::chrono::high_resolution_clock::now();

    // Case: Files were found
    if (!fileNames.empty() && !g_operationCancelled.load()) {
        std::cout << "\n\n"
                  << vt.green << fileNames.size() << " "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.green << "files found" << vt.yellow << "\n"
                  << currentCacheOld << " "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.yellow << "cached entries" << vt.reset << vt.bold << "\n\n";
    }

    // Case: No new files were found, but files exist in cache
    if (!newFilesFound && !files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n"
                  << vt.red << "0 "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.red << "files found " << vt.yellow << "\n"
                  << files.size() << " "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.yellow << "cached entries | "
                  << vt.blue << "ls "
                  << vt.yellow << "↵ to list" << vt.reset << vt.bold << "\n\n";
    }

    // Case: No files were found
    if (files.empty() && !list && !g_operationCancelled.load()) {
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout << "\n\n"
                  << vt.red << "0" << vt.orange << " {" << fileExtension << "} "
                  << vt.red << "files found\n"
                  << vt.yellow << "0" << vt.orange << " {" << fileExtension << "} "
                  << vt.yellow << "cached entries\n"
                  << vt.reset << vt.bold << "\n";
    }

    auto total_elapsed_time =
        std::chrono::duration<double>(end_time - start_time).count();
    std::cout << vt.bold << "Time Elapsed: " << std::fixed << std::setprecision(1)
              << total_elapsed_time << " seconds" << vt.bold << "\n\n";

    std::cout << color << "↵ to continue..." << vt.reset << vt.bold;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}
