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
#include <vector>

// C / System Headers
#include <termios.h>
#include <signal.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>
#include <readline/history.h>

// Project Headers
#include "../databaseOps.h"
#include "../display.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Waits for the user to press Enter, Esc or Ctrl+D before allowing another attempt.
 *
 * Switches the terminal to raw mode (no canonical buffering, no echo) so that
 * Enter, Esc and Ctrl+D are detected immediately without requiring a newline.
 * Restores the original terminal state before returning.
 */
void pressEnterToTry() {
    enable_ctrl_d();
    struct termios raw, saved;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    std::cout << color << "\n↵ to try again..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == '\r' || ch == 4 || ch == 27) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
}

/**
 * @brief Waits for the user to press Enter, Esc or Ctrl+D before returning to a previous menu or state.
 *
 * Switches the terminal to raw mode (no canonical buffering, no echo) so that
 * Enter, Esc and Ctrl+D are detected immediately without requiring a newline.
 * Restores the original terminal state before returning.
 */
void pressEnterToReturn() {
    enable_ctrl_d();
    struct termios raw, saved;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    std::cout << color << "\n↵ to return..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == '\r' || ch == 4 || ch == 27) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
}

/**
 * @brief Waits for the user to press Enter, Esc or Ctrl+D before continuing program execution.
 *
 * Switches the terminal to raw mode (no canonical buffering, no echo) so that
 * Enter, Esc and Ctrl+D are detected immediately without requiring a newline.
 * Restores the original terminal state before returning.
 */
void pressEnterToContinue() {
    enable_ctrl_d();
    struct termios raw, saved;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    std::cout << color << "\n↵ to continue..." << UI::Palette::BoldReset;
    std::cout.flush();
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\n' || ch == '\r' || ch == 4 || ch == 27) break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
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
 * @brief Processes and displays the results of an ISO file selection operation.
 *
 * Evaluates @c verboseSets to determine whether the operation produced valid
 * output, then either prints an error notice or delegates to @c verbosePrint.
 *
 * @details **Logic flow:**
 * - **"No valid input" check:** If @c uniqueErrorTokenMessages is non-empty while
 *   @c operationFailed, @c operationSkipped, and @c operationCompleted are all
 *   empty, clears the scroll buffer, prints an error notice, and waits for the
 *   user to press Enter. Note: the condition checks @c operationFailed twice
 *   (likely a latent bug — @c operationSkipped is the intended second check).
 * - **Verbose output:** If @p verbose is true, clears the scroll buffer and calls
 *   @c verbosePrint. For mount operations, passes @c operationSkipped through and
 *   selects layout mode @c 2; for all other operations, passes an empty set and
 *   selects layout mode @c 1.
 * - **History clear:** After a destructive operation (@c mv, @c rm, or @c umount),
 *   if @p isFiltered and @p umountMvRmBreak are both true, clears Readline history
 *   to prevent re-running commands against indices that no longer exist.
 *
 * @param operation      The current action string ("mount", "mv", "rm", "umount", etc.);
 *                       used to derive the @c verbosePrint layout mode and to gate
 *                       the history-clear path.
 * @param verbose        If true, triggers the detailed results screen via @c verbosePrint.
 * @param isFiltered     Indicates whether a search filter is currently active; gates
 *                       the history-clear path alongside @p umountMvRmBreak.
 * @param umountMvRmBreak When true (set by the caller after a destructive operation
 *                        completes), triggers the history clear on the next results pass.
 */
void handleSelectIsoFilesResults(const std::string& operation, bool& verbose,
                                 bool& isFiltered, bool& umountMvRmBreak) {

    const auto c = getVerboseTheme();
    bool isMount = false;

    if (operation == "mount") isMount = true;

    if (!verboseSets.uniqueErrorTokenMessages.empty() && verboseSets.operationFailed.empty() &&
         verboseSets.operationFailed.empty() && verboseSets.operationSkipped.empty() &&
         verboseSets.operationCompleted.empty()) {

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
 * @brief Generates and logs color-coded error messages for filesystem operations.
 *
 * Constructs a human-readable error string for the given @p errorType, appends it
 * to @p verboseErrors, increments @p failedTasks, clears @p operationSuccessful,
 * and invokes @p batchInsertFunc to notify the UI.
 *
 * @details **Error types and formatting:**
 * - @c "same_file"        — red; always uses full @c srcDir/srcFile path regardless
 *                           of @c displayConfig::toggleNamesOnly.
 * - @c "source_missing"   — red; uses @c displaySrc.
 * - @c "overwrite_failed" — red; uses @c destDir + @c srcFile (no separator) + @p errorDetail.
 * - @c "file_exists"      — red; uses @c displaySrc and @c destDir; hints at @c -o flag.
 * - @c "remove_after_move"— red; uses @c displaySrc + @p errorDetail.
 * - @c "missing_file"     — purple (distinct from all other branches); uses @c displaySrc.
 * - default               — red; emits @p errorDetail directly.
 *
 * **Display path:** @c displaySrc is @c srcDir/srcFile when
 * @c displayConfig::toggleNamesOnly is false, or @c srcFile alone when true.
 * This toggle is applied in all branches except @c "same_file".
 *
 * **Thread safety:** @p failedTasks is incremented with @c memory_order_acq_rel;
 * @p operationSuccessful is stored @c false with @c memory_order_release.
 *
 * @param errorType          Category of failure: "same_file", "source_missing",
 *                           "overwrite_failed", "file_exists", "remove_after_move",
 *                           "missing_file", or any other value (falls through to
 *                           generic error using @p errorDetail).
 * @param srcDir             Path view of the source directory.
 * @param srcFile            Filename view of the source file.
 * @param destDir            Path view of the destination; used as-is in
 *                           @c "file_exists", and concatenated directly with
 *                           @p srcFile (no separator) in @c "overwrite_failed".
 * @param errorDetail        System error description; used by @c "overwrite_failed",
 *                           @c "remove_after_move", and the default branch.
 * @param operation          Operation label appended into @c "same_file" and
 *                           @c "file_exists" messages.
 * @param verboseErrors      Container to which the formatted error string is appended.
 * @param failedTasks        Non-null pointer to atomic counter; incremented
 *                           unconditionally via @c fetch_add.
 * @param operationSuccessful Atomic flag set to @c false unconditionally.
 * @param batchInsertFunc    Callback invoked after the error is logged to notify
 *                           the UI of a new entry.
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
                .append(vt.red).append("'").append(vt.yellow).append(srcDir).append("/").append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(vt.red).append("Source file no longer exists: ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(vt.red).append("Failed to overwrite: ")
                .append(vt.red).append("'").append(vt.yellow).append(destDir).append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(vt.red).append("Error ").append(operation).append(": ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" to '").append(vt.yellow).append(destDir).append("': File exists ")
                .append(vt.red).append("(overwrite with -o")
                .append(vt.reset).append(vt.red).append(").")
                .append(vt.reset);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(vt.red).append("Move completed but failed to remove source file: ")
                .append(vt.red).append("'").append(vt.yellow).append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ").append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(vt.purple).append("Missing: ")
				.append(vt.purple).append("'")
				.append(vt.yellow).append(displaySrc)
				.append(vt.purple).append("'.");
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
 * @brief Finalizes the ISO database refresh by reporting results and syncing global state.
 *
 * Acts as the terminal stage of the database update pipeline. It calculates performance
 * metrics, logs errors, persists the new list to disk, and refreshes the UI cache.
 *
 * @details **Workflow & Safety:**
 * - **Signal Guarding:** Suppresses @c SIGINT and disables @c Ctrl+D at function entry
 *   for the remainder of the call; neither is restored before return.
 * - **Cache Sync:** If @p newISOFound is true on entry, calls @c loadFromDatabase to
 *   reconcile the in-memory @c globalIsoFileList with the saved disk state before
 *   any output is produced.
 * - **Persistence:** Calls @c saveToDatabase (passing @c &newISOFound, which it may
 *   mutate) unless @c GlobalState::g_operationCancelled is set.
 * - **User Feedback:** Prints elapsed time, then a color-coded outcome from one of
 *   five branches: Cancelled; save failed with files and delta; no valid paths;
 *   files present but no delta (save not attempted or failed); no ISOs found;
 *   or successful import with delta count from @c countDifferentEntries.
 * - **Invalid path / error reporting:** Prints invalid paths and error messages before
 *   the save step. When @p totalFiles is zero and @p validPaths is empty, also emits
 *   a "Total files processed: 0" line before the invalid-path list.
 *
 * @param allIsoFiles         Full list of discovered ISO paths to be saved.
 * @param totalFiles          Atomic count of total files scanned; gates the
 *                            "Total files processed: 0" diagnostic line.
 * @param validPaths          Base directories successfully traversed; an empty set
 *                            triggers the "Lack of valid paths" failure branch.
 * @param invalidPaths        Directories skipped due to permissions or existence errors.
 * @param uniqueErrorMessages Deduplicated log of system-level I/O errors.
 * @param newISOFound         On entry: whether the scan produced delta changes, gating
 *                            the @c loadFromDatabase call. Passed by pointer to
 *                            @c saveToDatabase, which may modify it.
 * @param start_time          Timestamp of the refresh operation's origin, used to
 *                            compute total elapsed time.
 */
void saveAndReportResultsForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles,
                                    std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths,
                                    std::unordered_set<std::string>& uniqueErrorMessages, bool& newISOFound,
                                    const std::chrono::high_resolution_clock::time_point& start_time) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    if (newISOFound) loadFromDatabase(GlobalState::globalIsoFileList);

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    auto printInvalidPaths = [&]() {
        if (invalidPaths.empty()) return;
        if (totalFiles == 0 && validPaths.empty()) {
            std::cout << "\r" << color << "Total files processed: 0\n" << std::flush;
        }
        std::cout << "\n" << color << "Invalid paths omitted from search: " << vt.red;
        for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
            std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
        }
        std::cout << color << ".\n";
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
    std::cout << color << "\nTime Elapsed: " << std::fixed << std::setprecision(1)
              << total_elapsed << "s\n";

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
        int result = countDifferentEntries(allIsoFiles, GlobalState::globalIsoFileList);
        std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << result << " ISO imported" << vt.green << "]" << vt.bold << "\n";
    }

    pressEnterToContinue();
    return;
}

/**
 * @brief Logs deduplicated filesystem errors and invalid search paths to the terminal.
 *
 * Formats and displays issues encountered during a crawl (invalid paths, I/O errors),
 * then clears both input sets to prepare for the next search cycle.
 *
 * @details
 * - **Signal suppression:** Calls @c signal(SIGINT, SIG_IGN) and @c disable_ctrl_d()
 *   at entry; neither is restored before return.
 * - **"Total files processed: 0":** Printed to @c std::cout only when
 *   @p directoryPaths is empty and @p invalidDirectoryPaths is non-empty.
 * - **Invalid path output:** The header @c "Invalid paths omitted from search: " goes
 *   to @c std::cout; the path values and trailing @c '.' go to @c std::cerr.
 * - **Error message output:** Entries in @p processedErrorsFind are printed to
 *   @c std::cout, separated by newlines between entries (no trailing newline after
 *   the last entry).
 * - **Cleanup:** Both @p processedErrorsFind and @p invalidDirectoryPaths are cleared
 *   unconditionally before return.
 *
 * @param invalidDirectoryPaths Paths that were inaccessible or invalid; cleared on exit.
 * @param directoryPaths        Source directories from the crawl; only @c .empty() is
 *                              checked, to gate the "Total files processed: 0" line.
 * @param processedErrorsFind   Deduplicated formatted error strings; cleared on exit.
 */
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
        std::cout << "\r" << color << "Total files processed: 0" << std::flush;
    }

    if (!invalidDirectoryPaths.empty()) {
        std::cout << "\n\n" << color << "Invalid paths omitted from search: " << vt.red;
        for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
            std::cerr << "'" << *it << "'" << (std::next(it) != invalidDirectoryPaths.end() ? " " : "");
        }
        std::cerr << color << ".";
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
 * Captures @c end_time immediately on entry, then selects one of three display
 * branches based on search outcome. Elapsed time and @c pressEnterToContinue are
 * always printed unconditionally at the end, regardless of which branch fired or
 * whether the operation was cancelled.
 *
 * @details **Display branches** (all gated on
 * @c !GlobalState::g_operationCancelled.load()):
 * - **Files found** (@c !fileNames.empty()): prints found count vs @p currentCacheOld.
 *   Does NOT call @c verboseFind. Not gated on @p list.
 * - **No new files, cache non-empty** (@c !newFilesFound && !files.empty() && !list):
 *   calls @c verboseFind, then prints 0 found vs @c files.size() with an @c "ls ↵"
 *   hint.
 * - **Cache empty** (@c files.empty() && !list): calls @c verboseFind, then prints
 *   0 found and 0 cached.
 *
 * If @c g_operationCancelled is set, all three branches are skipped; only elapsed
 * time and the Enter prompt are shown.
 *
 * After @c pressEnterToContinue, @c clearScrollBuffer() is called.
 *
 * @param fileExtension         Target format label used in output (e.g., ".bin/.img").
 * @param fileNames             Files discovered in the current run; @c .size() and
 *                              @c .empty() are used; not modified here.
 * @param invalidDirectoryPaths Passed to @c verboseFind (cleared there).
 * @param newFilesFound         Gates the "no new files" branch; distinct from
 *                              @p fileNames being non-empty.
 * @param list                  Suppresses the second and third branches when true;
 *                              does not suppress the first branch.
 * @param currentCacheOld       Cache entry count before this search; displayed
 *                              alongside @p fileNames.size() in the first branch.
 * @param files                 Current working cache; @c .size() and @c .empty()
 *                              used for branch selection and display.
 * @param start_time            Search start timestamp; elapsed time is computed
 *                              against @c end_time captured at function entry.
 * @param processedErrorsFind   Passed to @c verboseFind (cleared there).
 * @param directoryPaths        Passed to @c verboseFind for the "Total files
 *                              processed: 0" gate check.
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
    std::cout << color << "Time Elapsed: " << std::fixed << std::setprecision(1)
              << total_elapsed_time << "s" << vt.bold << "\n";

    pressEnterToContinue();
    clearScrollBuffer();
}
