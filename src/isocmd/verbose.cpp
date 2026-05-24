// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <termios.h>
#include <signal.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>

// Project Headers
#include "../caches.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../inputHandling.h"
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
 *
 * Orchestrates a specialized results screen by managing terminal state and
 * displaying sorted batches of success, warning, and error messages.
 *
 * @details **Implementation Highlights:**
 * - **Signal Safety:** Ignores @c SIGINT and @c Ctrl+D to ensure results are
 *   read before returning to the main menu.
 * - **Memory Efficiency:** Uses @c std::string_view to sort and print data
 *   without additional heap allocations or string copying.
 * - **Case-Insensitive Logic:** Implements @c std::lexicographical_compare with
 *   @c std::tolower to provide a user-friendly sorted order.
 * - **Stream Routing:** Directs error-type items to @c std::cerr with
 *   color-coding from @c VerboseAndDatabaseTheme.
 *
 * @param primarySet   Main data set (e.g., successful unmounts or conversions).
 * @param secondarySet Supporting data (e.g., already unmounted or skipped items).
 * @param tertiarySet  Additional context (e.g., specific warning categories).
 * @param errorSet     Set of explicit failure strings.
 * @param printType    Layout mode: 0 (Umount), 1 (Basic Ops), 2 (Mount), 3 (Conversion).
 */
void verbosePrint(std::unordered_set<std::string>& primarySet,
            std::unordered_set<std::string>& secondarySet,
            std::unordered_set<std::string>& tertiarySet,
            std::unordered_set<std::string>& errorSet,
            int printType) {

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
        // Needed to clear verbose input errors from dry-runs in cp/mv/rm
        verboseSets.uniqueErrorTokenMessages.clear();
    }
}

/**
 * @brief Generates and logs color-coded error messages for filesystem operations.
 *
 * Constructs a human-readable error string tailored to the operation (CP, MV, or RM)
 * and the specific failure type. The output is formatted according to the current
 * @c VerboseAndDatabaseTheme.
 *
 * @details **Efficiency & Safety:**
 * - Uses @c std::string_view to minimize string copying.
 * - Employs @c std::string::reserve to reduce reallocations during formatting.
 * - Thread-safe updates of task counters using @c std::memory_order_acq_rel.
 *
 * @param errorType   Category of failure (e.g., "same_file", "source_missing").
 * @param srcDir      Path view of the source directory.
 * @param srcFile     Filename view of the source file.
 * @param destDir     Path view of the destination directory.
 * @param errorDetail Specific system error or errno description.
 * @param operation   The user-facing label for the action ("copy", "move", "delete").
 * @param[out] verboseErrors The log container where the formatted error is stored.
 * @param[in,out] failedTasks Atomic counter incremented upon error.
 * @param[in,out] operationSuccessful Atomic flag set to false on failure.
 * @param batchInsertFunc Callback triggered to notify the UI of a new error entry.
 */
void reportErrorCpMvRm(std::string_view errorType,
                       std::string_view srcDir,
                       std::string_view srcFile,
                       std::string_view destDir,
                       std::string_view errorDetail,
                       std::string_view operation,
                       OperationReporter& reporter) {

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    // Build displaySrc once
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
                .append(vt.red).append("'").append(vt.yellow).append(srcDir).append("/").append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".").append(vt.reset);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(vt.red).append("Source file no longer exists: ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".").append(vt.reset);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(vt.red).append("Failed to overwrite: ")
                .append(vt.red).append("'").append(vt.yellow).append(destDir).append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail).append(".").append(vt.reset);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(vt.red).append("Error ").append(operation).append(": ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" to '").append(vt.yellow).append(destDir).append("': File exists ")
                .append(vt.red).append("(overwrite with -o)").append(vt.reset).append(vt.red).append(".").append(vt.reset);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(vt.red).append("Move completed but failed to remove source file: ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail).append(".").append(vt.reset);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(vt.purple).append("Missing: ")
                .append(vt.purple).append("'").append(vt.yellow).append(displaySrc).append(vt.purple).append("'.");
    }
    else {
        errorMsg.append(vt.red).append("Error: ").append(errorDetail).append(vt.reset);
    }

    reporter.verboseErrors.push_back(std::move(errorMsg));

    if (reporter.failedTasks)
        reporter.failedTasks->fetch_add(1, std::memory_order_acq_rel);

    reporter.operationSuccessful.store(false, std::memory_order_release);

    if (reporter.batchInsertMessages)
        reporter.batchInsertMessages();
}

/**
 * @brief Processes and displays the results of ISO file selection operations.
 *
 * Evaluates the state of @c verboseSets to determine if an operation (Mount, Unmount,
 * Move, or Remove) succeeded, failed, or received invalid input.
 *
 * @details **Logic flow:**
 * - **Error Handling:** Detects "No valid input" if only errors exist without successes.
 * - **Verbose Output:** Delegates to @c verbosePrint if the @p verbose flag is set,
 *   switching layout modes based on @p isMount.
 * - **State Management:** Sets @p needsClrScrn to signal the caller to refresh the TUI.
 * - **History Management:** Clears Readline history on destructive filtered operations
 *   (mv, rm, umount) to prevent re-running commands on non-existent indices.
 *
 * @param operation      The current action string (e.g., "mv", "rm", "umount").
 * @param verbose        If true, triggers the detailed results screen.
 * @param isMount        Determines the @c verbosePrint layout mode (Mount vs Ops).
 * @param isFiltered     Indicates if a search filter is currently active.
 * @param umountMvRmBreak Flag used to trigger a screen break/history clear.
 * @param isUnmount      Special case flag to skip the "No ISO available" check.
 * @param[out] needsClrScrn Set to true if the function performed terminal output.
 */
void handleSelectIsoFilesResults(const std::string& operation, bool& verbose,
                                 bool& isFiltered, bool& umountMvRmBreak) {

    const auto c = getVerboseTheme();
    bool isMount = false;

    if (operation == "mount") isMount = true;

    if (!verboseSets.uniqueErrorTokenMessages.empty() && verboseSets.operationFailed.empty() &&
         verboseSets.operationFailed.empty() && verboseSets.operationSkipped.empty()) {

        clearScrollBuffer();

        std::cout << "\n" << (c.red)
                  << "No valid input provided."
                  << UI::Palette::BoldReset << "\n";

        pressEnterToContinue();
    }
    else if (verbose) {
        clearScrollBuffer();

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
    }
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
 * @brief Finalizes the ISO database refresh by reporting results and syncing global state.
 *
 * Acts as the terminal stage of the database update pipeline. It calculates performance
 * metrics, logs errors, persists the new list to disk, and refreshes the UI cache.
 *
 * @details **Workflow & Safety:**
 * - **Signal Guarding:** Temporarily ignores @c SIGINT and disables @c Ctrl+D to prevent
 *   database corruption during the critical write-to-disk phase.
 * - **Persistence:** Dispatches data to @c saveToDatabase unless the operation was
 *   explicitly cancelled via @c GlobalState::g_operationCancelled.
 * - **Cache Sync:** If new files are found, it triggers @c loadFromDatabase to reconcile
 *   the in-memory @c globalIsoFileList with the newly saved disk state.
 * - **User Feedback:** Provides a color-coded summary of elapsed time, file counts,
 *   and specific failure reasons (e.g., lack of valid paths or locked database).
 *
 * @param allIsoFiles      Full list of discovered ISO paths to be saved.
 * @param totalFiles       Atomic counter of total files scanned (including non-ISOs).
 * @param validPaths       Collection of base directories successfully traversed.
 * @param invalidPaths     Set of directories skipped due to permissions or existence errors.
 * @param uniqueErrorMessages Deduplicated log of system-level I/O errors.
 * @param start_time       Point of origin for the refresh operation for duration calculation.
 * @param newISOFound      Boolean toggle indicating if the scan resulted in delta changes.
 */
void saveAndReportResultsForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles,
                                    std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths,
                                    std::unordered_set<std::string>& uniqueErrorMessages, bool& newISOFound,
                                    const std::chrono::high_resolution_clock::time_point& start_time) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    if (newISOFound) loadFromDatabase(GlobalCaches::globalIsoFileList);

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

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

    if ((!uniqueErrorMessages.empty() || !invalidPaths.empty())) {
        printInvalidPaths();
        printErrorMessages();
    }

    const bool saveSuccess = GlobalState::g_operationCancelled ? false : saveToDatabase(allIsoFiles, &newISOFound);
    const auto end_time = std::chrono::high_resolution_clock::now();

    const double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << vt.bold << "\nTotal time taken: " << std::fixed << std::setprecision(1)
              << total_elapsed << " seconds\n";

    if (GlobalState::g_operationCancelled) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "Cancelled" << vt.green << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && newISOFound && !saveSuccess) {
        std::cout << "\n" << vt.red << "Database Refresh failed: [" << vt.yellow << "Unable to access the database file" << vt.red << "]" << vt.bold << "\n";
    } else if (validPaths.empty()) {
        std::cout << "\n" << vt.red << "Database refresh failed: [" << vt.yellow << "Lack of valid paths" << vt.red << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && !newISOFound && !saveSuccess) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No new ISO found" << vt.green << "]" << vt.bold << "\n";
    } else if (allIsoFiles.empty()) {
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No ISO found" << vt.green << "]" << vt.bold << "\n";
    } else if (!allIsoFiles.empty() && saveSuccess && newISOFound) {
        int result = countDifferentEntries(allIsoFiles, GlobalCaches::globalIsoFileList);
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << result << " ISO imported" << vt.green << "]" << vt.bold << "\n";
    }

    pressEnterToContinue();
    return;
}

/**
 * @brief Logs deduplicated filesystem errors and invalid search paths to the terminal.
 *
 * This diagnostic utility formats and displays issues encountered during a crawl,
 * such as permission-denied errors or non-existent directories. It ensures a
 * clean UI by handling line breaks and punctuation dynamically.
 *
 * @details **Operational Side Effects:**
 * - **Signal Immunity:** Temporarily ignores @c SIGINT to prevent display corruption
 *   during the error-dumping phase.
 * - **Memory Cleanup:** Automatically @b clears both @p processedErrorsFind and
 *   @p invalidDirectoryPaths upon completion to prepare for the next search cycle.
 * - **Stream Redirection:** Invalid paths are sent to @c std::cerr to distinguish
 *   structural errors from standard output summaries.
 *
 * @param[in,out] invalidDirectoryPaths Set of paths that were inaccessible or invalid.
 * @param[in]     directoryPaths        The list of source directories (used for state checks).
 * @param[in,out] processedErrorsFind   Deduplicated collection of formatted error strings.
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
 * @brief Renders a color-coded summary of image discovery results and cache deltas.
 *
 * Provides the user with a detailed report following a search for specific image
 * extensions. It highlights the difference between newly discovered files and
 * existing cached entries.
 *
 * @details **Reporting Features:**
 * - **Delta Visualization:** Clearly distinguishes between @p fileNames (newly found)
 *   and @p currentCacheOld (existing state).
 * - **Dynamic Hints:** If no new files are found but a cache exists, it provides
 *   a context-aware "ls" command hint to the user.
 * - **Error Aggregation:** Integrates with @c verboseFind to list invalid directory
 *   paths and system I/O errors encountered during the crawl.
 * - **Terminal Locking:** Disables interrupt signals and EOF inputs during the
 *   reporting phase to ensure the "Press Enter to Continue" prompt is respected.
 *
 * @param fileExtension      The target format extension (e.g., ".bin/.img").
 * @param fileNames          Set of unique file paths discovered in the current run.
 * @param invalidDirectoryPaths Set of paths that failed permission/existence checks.
 * @param newFilesFound      Toggle indicating if the current crawl added new data.
 * @param list               Flag to suppress output if the list view is already active.
 * @param currentCacheOld    The size of the cache prior to the current search.
 * @param files              The current working list of cached files.
 * @param start_time         Timestamp of the search initiation for duration logic.
 * @param processedErrorsFind Log of specific filesystem errors.
 * @param directoryPaths     List of base directories that were scanned.
 */
void verboseImageSearchResults(const std::string& fileExtension,
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
