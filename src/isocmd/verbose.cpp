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

    auto printSortedSet = [](std::unordered_set<std::string>& set, bool isError = false) {
        if (!set.empty()) {
            std::vector<std::string> vec(
                std::make_move_iterator(set.begin()), 
                std::make_move_iterator(set.end())
            );
            
            sortFilesCaseInsensitive(vec);
            std::cout << "\n";
            
            for (const auto& item : vec) {
				if (isError) {
					// Red for the error message, then reset to your high-fidelity bold
					std::cerr << originalColors::red << item << originalColors::boldAlt  << "\n";
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
    
    std::cout << color << "↵ to continue..." << reset; 
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

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view errLabel     = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath      = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view missingLabel = isOriginal ? originalColors::purple   : theme->secondary;
    
    const std::string displaySrc  = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    std::string errorMsg;
    errorMsg.reserve(256);

    if (errorType == "same_file") {
        errorMsg.append(errLabel).append("Cannot ").append(operation).append(" file to itself: ")
                .append(errPath).append("'").append(srcDir).append("/").append(srcFile).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(".")
                .append(originalColors::boldAlt);
    }
    else if (errorType == "invalid_dest") {
        errorMsg.append(errLabel).append("Error ").append(operation).append(": ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(" to '").append(destDir).append("': ").append(errorDetail).append(".")
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(errLabel).append("Source file no longer exists: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(".")
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(errLabel).append("Failed to overwrite: ")
                .append(errPath).append("'").append(destDir).append("/").append(srcFile).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(" - ").append(errorDetail).append(".")
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(errLabel).append("Error ").append(operation).append(": ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(" to '").append(destDir).append("/': File exists (")
                .append(errPath).append("enable overwrites")
                .append(originalColors::boldAlt).append(errLabel).append(").")
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(errLabel).append("Move completed but failed to remove source file: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::boldAlt).append(errLabel).append(" - ").append(errorDetail)
                .append(originalColors::boldAlt);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(missingLabel).append("Missing: ")
                .append(errPath).append("'").append(displaySrc).append("'")
                .append(originalColors::boldAlt).append(missingLabel).append(".")
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
    }
    else {
        errorMsg.append(errLabel).append("Error: ").append(errorDetail)
                .append(originalColors::boldAlt).append(originalColors::boldAlt);
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

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

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
    refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
}

/**
 * @brief Prints directory paths and specific errors encountered during a filesystem search.
 */
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view boldLabel = isOriginal ? originalColors::boldAlt : theme->muted;
	std::string_view errLabel  = isOriginal ? originalColors::red     : theme->secondary;

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

/**
 * @brief Displays a summary of image file search results, including cache status and time elapsed.
 */
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
    std::string_view boldLabel = isOriginal ? originalColors::boldAlt   : theme->muted;

    auto end_time = std::chrono::high_resolution_clock::now();

    if (g_operationCancelled.load()) return;

    if (!fileNames.empty()) {
        std::cout << "\n\n"
                  << okLabel   << fileNames.size() << " "
                  << extColor  << "{" << fileExtension << "} "
                  << okLabel   << "files found" << warnLabel << "\n"
                  << currentCacheOld << " "
                  << extColor  << "{" << fileExtension << "} "
                  << warnLabel << "cached entries" << originalColors::boldAlt << "\n\n";
    }

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
