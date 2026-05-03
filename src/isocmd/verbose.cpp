// SPDX-License-Identifier: GPL-3.0-or-later

// Project Headers
#include "../caches.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../inputHandling.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Waits for the user to press Enter or Ctrl+D before allowing another attempt.
 *
 * Uses raw fd read to correctly handle both Enter and Ctrl+D regardless of
 * tty VEOF state, avoiding buffering issues with std::cin in canonical mode.
 */
void pressEnterToTry() {
    enable_ctrl_d();
    std::cout << color << "\n↵ to try again..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == 4) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

/**
 * @brief Waits for the user to press Enter or Ctrl+D before returning to a previous menu or state.
 *
 * Uses raw fd read to correctly handle both Enter and Ctrl+D regardless of
 * tty VEOF state, avoiding buffering issues with std::cin in canonical mode.
 */
void pressEnterToReturn() {
    enable_ctrl_d();
    std::cout << color << "\n↵ to return..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == 4) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

/**
 * @brief Waits for the user to press Enter or Ctrl+D before continuing program execution.
 *
 * Uses raw fd read to correctly handle both Enter and Ctrl+D regardless of
 * tty VEOF state, avoiding buffering issues with std::cin in canonical mode.
 */
void pressEnterToContinue() {
    enable_ctrl_d();
    std::cout << color << "\n↵ to continue..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == 4) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

/**
 * @brief Performs a high-visibility print of operation results categorized by sets.
 * @details Handles signal management, terminal cleanup, and sorted output of
 * success, warning, and error strings based on a specific operation context.
 * Strings are sorted case-insensitively and printed via string_view to avoid
 * heap allocation or copying from the underlying sets.
 *
 * @param primarySet Main data set (usually processed items).
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
        if (set.empty()) return;

        std::vector<std::string_view> views;
        views.reserve(set.size());
        for (const auto& s : set)
            views.emplace_back(s);

        std::sort(views.begin(), views.end(), [](std::string_view a, std::string_view b) {
            return std::lexicographical_compare(
                a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) {
                    return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
                }
            );
        });

        std::cout << "\n";
        for (std::string_view item : views) {
            if (isError)
                std::cerr << vt.red << item << vt.reset << "\n";
            else
                std::cout << item << "\n";
        }
    };

    switch (printType) {
        case 0:
            printSortedSet(primarySet);
            printSortedSet(secondarySet);
            printSortedSet(errorSet, true);
            break;
        case 1:
            printSortedSet(primarySet);
            printSortedSet(secondarySet);
            printSortedSet(errorSet);
            break;
        case 2:
            printSortedSet(primarySet);
            printSortedSet(tertiarySet, true);
            printSortedSet(secondarySet, true);
            printSortedSet(errorSet, true);
            break;
        case 3:
            printSortedSet(secondarySet);
            printSortedSet(tertiarySet);
            printSortedSet(errorSet);
            printSortedSet(primarySet);
            break;
    }

    clearGlobalVerboseSets();
    
    pressEnterToContinue();
}

/**
 * @brief Displays a collection of unique error messages generated during tokenization.
 */
void displayErrors() {
    if (!verboseSets.uniqueErrorTokenMessages.empty()) {
        std::cout << "\n";
        for (const auto& err : verboseSets.uniqueErrorTokenMessages) {
            std::cout << err << "\n";
        }
        verboseSets.uniqueErrorTokenMessages.clear();
    }
}

/**
 * @brief Processes and displays the results of ISO file selection operations.
 * * Handles error messaging, verbose output, and screen refresh logic after
 * a mount, unmount, or file operation has been attempted.
 * * @param uniqueErrorMessages Set of unique error strings collected during the operation.
 * @param operationFiles Set of files successfully operated on.
 * @param operationFails Set of files that failed the operation.
 * @param skippedMessages Set of messages for files that were skipped.
 * @param operation String representing the current operation (e.g., "mv", "rm").
 * @param verbose Boolean flag for detailed output.
 * @param isMount Boolean indicating if the operation was a mount.
 * @param isFiltered Boolean indicating if a filter is currently active.
 * @param umountMvRmBreak Flag to trigger screen break on destructive actions.
 * @param isUnmount Boolean indicating if the operation was an unmount.
 * @param needsClrScrn Output flag to signal if the screen should be cleared.
 */
void handleSelectIsoFilesResults(const std::string& operation, bool& verbose, bool isMount, 
                                 bool& isFiltered, bool& umountMvRmBreak, bool isUnmount, 
                                 bool& needsClrScrn) {
    
    const auto c = getVerboseTheme();
    
    if (!verboseSets.uniqueErrorTokenMessages.empty() && verboseSets.operationFailed.empty() && 
         verboseSets.operationFailed.empty() && verboseSets.operationSkipped.empty()) {
        
        clearScrollBuffer();
        needsClrScrn = true;
        
        std::cout << "\n" << (c.red) 
                  << "No valid input provided." 
                  << UI::Palette::BoldReset << "\n";

        pressEnterToContinue();
    } 
    else if (verbose) {
        clearScrollBuffer();
        needsClrScrn = true;
       
       std::unordered_set<std::string> emptySet;
       verbosePrint(
			verboseSets.operationCompleted,
			verboseSets.operationFailed,
			isMount ? verboseSets.operationSkipped : emptySet,
			verboseSets.uniqueErrorTokenMessages,
			isMount ? 2 : 1
		);
    }

    if ((operation == "mv" || operation == "rm" || operation == "umount") && isFiltered && umountMvRmBreak) {
        clear_history();
        needsClrScrn = true;
    }

    if (!isUnmount && GlobalCaches::globalIsoFileList.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        
        std::cout << "\n" << (c.red) 
                  << "No ISO available for " << operation << "." 
                  << UI::Palette::BoldReset << "\n\n";
        
        pressEnterToContinue();
        return;
    }
}

/**
 * @brief Generates and logs color-coded error messages for file operations (CP, MV, RM).
 * @details Utilizes the current theme to construct a human-readable error string 
 * using std::string_view for zero-copy efficiency. Updates atomic task counters 
 * and triggers the batch message insertion callback.
 * 
 * @param errorType The category of error (e.g., "same_file", "invalid_dest").
 * @param srcDir Source directory view.
 * @param srcFile Source filename view.
 * @param destDir Destination directory view.
 * @param errorDetail Specific system error message or description.
 * @param operation The action being performed ("copy", "move", "delete").
 */
void reportErrorCpMvRm(std::string_view errorType, std::string_view srcDir, std::string_view srcFile, 
                       std::string_view destDir, std::string_view errorDetail, std::string_view operation, 
                       std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, 
                       std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc) {
    
    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    // build displaySrc once using append to avoid extra temporary strings
    std::string displaySrc;
    if (!displayConfig::toggleNamesOnly) {
        displaySrc.reserve(srcDir.size() + srcFile.size() + 1);
        displaySrc.append(srcDir).append("/").append(srcFile);
    } else {
        displaySrc = std::string(srcFile);
    }

    std::string errorMsg;
    errorMsg.reserve(256 + displaySrc.size() + errorDetail.size());

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

bool saveToDatabase(const std::vector<std::string>& globalIsoFileList, std::atomic<bool>& newISOFound);

/**
 * @brief Handles verbose output and result logic for the ISO database refresh process.
 * @details Summarizes time taken, files imported, and displays any path errors encountered.
 */
void saveAndReportResultsForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();
	
	if (newISOFound.load()) {
		loadFromDatabase(GlobalCaches::globalIsoFileList);
		newISOFound.store(false);
	}
    
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

    const bool saveSuccess = GlobalState::g_operationCancelled ? false : saveToDatabase(allIsoFiles, newISOFound);
    const auto end_time = std::chrono::high_resolution_clock::now();

    if (!promptFlag) return;

    const double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << vt.bold << "\nTotal time taken: " << std::fixed << std::setprecision(1)
              << total_elapsed << " seconds\n";

    if (GlobalState::g_operationCancelled) {
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
        int result = countDifferentEntries(allIsoFiles, GlobalCaches::globalIsoFileList);
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << result << " ISO imported" << vt.green << "]" << vt.bold << "\n";
    }

    pressEnterToContinue();
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
    if (!fileNames.empty() && !GlobalState::g_operationCancelled.load()) {
        std::cout << "\n\n"
                  << vt.green << fileNames.size() << " "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.green << "files found" << vt.yellow << "\n"
                  << currentCacheOld << " "
                  << vt.orange << "{" << fileExtension << "} "
                  << vt.yellow << "cached entries" << vt.reset << vt.bold << "\n\n";
    }

    // Case: No new files were found, but files exist in cache
    if (!newFilesFound && !files.empty() && !list && !GlobalState::g_operationCancelled.load()) {
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
    if (files.empty() && !list && !GlobalState::g_operationCancelled.load()) {
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
              << total_elapsed_time << " seconds" << vt.bold << "\n";

    pressEnterToContinue();
    clearScrollBuffer();
}
