// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>
#include <readline/history.h>

// Project Headers
#include "../databaseOps.h"
#include "../display.h"
#include "../inputHandling.h"
#include "../rendering.h"
#include "../pausePrompts.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Prints categorized operation results, sorted case-insensitively.
 *
 * Manages terminal state and prints sorted batches of success, warning and
 * error messages.
 *
 * - **Signal safety:** ignores SIGINT and disables Ctrl+D so results are
 *   read before returning to the main menu.
 * - **No copies:** sorts and prints std::string_view views of the input sets.
 * - **Case-insensitive:** std::lexicographical_compare with std::tolower.
 * - **Stream routing:** error-type items go to std::cerr with colors from
 *   VerboseAndDatabaseTheme.
 * - **Resize:** the whole print body is a render lambda handed to
 *   pressEnterToContinueWithRepaint(), so a resize during the wait repaints
 *   the results instead of clearing the screen and leaving only the prompt.
 *
 * @param primarySet   Main data set (e.g. successful unmounts or conversions).
 * @param secondarySet Supporting data (e.g. already unmounted or skipped
 *                     items).
 * @param tertiarySet  Additional context (e.g. specific warning categories).
 * @param errorSet     Explicit failure strings.
 * @param printType    Layout mode: 0 (Umount), 1 (Basic Ops), 2 (Mount),
 *                     3 (Conversion).
 */
void verbosePrint(std::unordered_set<std::string>& primarySet,
            std::unordered_set<std::string>& secondarySet,
            std::unordered_set<std::string>& tertiarySet,
            std::unordered_set<std::string>& errorSet,
            int printType) {

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    auto render = [&] {
        const VerboseAndDatabaseTheme vt = getVerboseTheme();
        SyncOutput sync;
        clearScrollBuffer();

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
    };

    pressEnterToContinueWithRepaint(render);
}

/**
 * @brief Prints and clears any unique error messages from tokenization.
 *
 * Used to surface verbose input errors produced by dry-runs (cp/mv/rm).
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
 * @brief Reports the results of an ISO file-selection operation.
 *
 * Examines verboseSets: prints a "no valid input" notice, or delegates to
 * verbosePrint when @p verbose is set.
 *
 * - **"No valid input" check:** if uniqueErrorTokenMessages is non-empty and
 *   operationFailed, operationSkipped and operationCompleted are all empty,
 *   clears the scroll buffer, prints the notice, and waits for Enter.
 * - **Verbose output:** if @p verbose is true, clears the scroll buffer and
 *   calls verbosePrint. Mount operations pass operationSkipped and use layout
 *   mode 2; all others pass an empty set and use mode 1.
 * - **History clear:** after a destructive operation (mv, rm, umount), if
 *   @p isFiltered and @p umountMvRmBreak are both true, Readline history is
 *   cleared so stale indices cannot be re-run.
 *
 * @param operation      Action string ("mount", "mv", "rm", "umount", ...);
 *                       drives the verbosePrint layout mode and the
 *                       history-clear gate.
 * @param verbose        If true, triggers the detailed results screen.
 * @param isFiltered     Whether a search filter is active; gates the
 *                       history-clear path with @p umountMvRmBreak.
 * @param umountMvRmBreak True after a destructive operation; triggers the
 *                        history clear on the next results pass.
 */
void handleSelectIsoFilesResults(const std::string& operation, bool& verbose,
                                 bool& isFiltered, bool& umountMvRmBreak) {

    const auto c = getVerboseTheme();
    bool isMount = false;

    if (operation == "mount") isMount = true;

    if (!verboseSets.uniqueErrorTokenMessages.empty() &&
        verboseSets.operationFailed.empty() &&
        verboseSets.operationSkipped.empty() &&
        verboseSets.operationCompleted.empty()) {

        auto render = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << "\n" << (c.red)
                      << "No valid input provided."
                      << UI::Palette::BoldReset << "\n";
        };

        pressEnterToReturnWithRepaint(render);
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
 * @brief Counts entries in @p allIsoFiles that are not in
 * @p globalIsoFileList.
 *
 * @return Number of new entries.
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
 * @brief Finalizes an ISO database refresh: reports results and syncs global
 * state.
 *
 * Last stage of the database update pipeline. Computes elapsed time, logs
 * errors, persists the new list, and refreshes the UI cache.
 *
 * - **Signal guarding:** SIGINT is ignored and Ctrl+D disabled at entry;
 *   neither is restored before return.
 * - **Cache sync:** if @p newISOFound is true on entry, calls
 *   loadFromDatabase to reconcile globalIsoFileList with disk before any
 *   output is produced.
 * - **Persistence:** calls saveToDatabase (passing &newISOFound, which may be
 *   mutated) unless GlobalState::g_operationCancelled is set.
 * - **User feedback:** prints elapsed time, then a color-coded outcome from
 *   one of five branches: Cancelled; save failed with files and delta; no
 *   valid paths; files present but no delta (save not attempted or failed);
 *   no ISOs found; or successful import with the delta count from
 *   countDifferentEntries.
 * - **Total files processed:** the results screen clears the terminal, which
 *   wipes the live progress counter, so the final count is reprinted at the
 *   top whenever at least one valid path was scanned. When @p validPaths is
 *   empty nothing was scanned: the invalid-path block emits its own
 *   "Total files processed: 0" line instead.
 * - **Invalid path / error reporting:** printed before the save step. When
 *   @p totalFiles is zero and @p validPaths is empty, also emits a "Total
 *   files processed: 0" line before the invalid-path list. Invalid paths are
 *   stored uncolored; the red color is applied here.
 * - **Resize:** the one-shot side effects (load, save, elapsed time, delta
 *   count) run before the wait; only the printing is replayed by the render
 *   lambda handed to pressEnterToContinueWithRepaint(). The prompt owns
 *   SIGWINCH: on each resize it clears the screen with
 *   "\033[H\033[2J\033[3J" (the third parameter drops the scrollback the
 *   previous frame was pushed into on VTE-based terminals), calls render,
 *   then reprints the prompt line. No ListResizeWatcher is installed here —
 *   it would race the prompt's own handler and cannot fire during
 *   pressEnterToImpl's raw read anyway. If @p repaintCallerHeader is
 *   non-empty, render calls it before printing the block, so the caller's
 *   screen header (e.g. the "FolderPaths ..." prompt line the user just
 *   answered) survives a resize; otherwise the block starts from a cleared
 *   screen with no caller context.
 *
 * @param allIsoFiles         Discovered ISO paths to be saved.
 * @param totalFiles          Atomic count of files scanned; reprinted after
 *                            the screen clear, and gates the zero-case line.
 * @param validPaths          Base directories successfully traversed; empty
 *                            triggers the "Lack of valid paths" branch.
 * @param invalidPaths        Directories skipped due to permissions or
 *                            existence errors.
 * @param uniqueErrorMessages Deduplicated system-level I/O errors.
 * @param newISOFound         On entry, whether the scan produced delta changes
 *                            (gates the loadFromDatabase call). Passed by
 *                            pointer to saveToDatabase, which may modify it.
 * @param start_time          Timestamp of the refresh origin, used to compute
 *                            elapsed time.
 * @param repaintCallerHeader Optional callback that reprints the caller's
 *                            screen header (e.g. the FolderPaths prompt line)
 *                            on a resize during the wait. When empty, the
 *                            block starts from a cleared screen with no
 *                            caller context.
 */
void saveAndReportResultsForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles,
                                    std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths,
                                    std::unordered_set<std::string>& uniqueErrorMessages, bool& newISOFound,
                                    const std::chrono::high_resolution_clock::time_point& start_time,
                                    const std::function<void()>& repaintCallerHeader) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    if (newISOFound) loadFromDatabase(GlobalState::globalIsoFileList);

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    const bool saveSuccess = GlobalState::g_operationCancelled ? false : saveToDatabase(allIsoFiles, &newISOFound);
    const auto end_time = std::chrono::high_resolution_clock::now();
    const double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();

    const int deltaCount =
        (!allIsoFiles.empty() && saveSuccess && newISOFound)
            ? countDifferentEntries(allIsoFiles, GlobalState::globalIsoFileList)
            : 0;

    const size_t totalProcessed = totalFiles.load();

    auto render = [&] {
        SyncOutput sync;
        clearScrollBuffer();

        // Reprint the caller's screen header (e.g. the FolderPaths prompt
        // line) before the block, so a resize during the confirmation wait
        // doesn't lose the question the user just answered. The callback is
        // optional; when empty, the block starts from a cleared screen.
        if (repaintCallerHeader) {
            repaintCallerHeader();
        }

        std::cout << "\n";

        if (!validPaths.empty()) {
            std::cout << "\r" << color << "Total files processed: " << totalProcessed << "\n";
        }

        auto printInvalidPaths = [&]() {
            if (invalidPaths.empty()) return;
            if (totalProcessed == 0 && validPaths.empty()) {
                std::cout << "\r" << color << "Total files processed: 0\n" << std::flush;
            }
            std::cout << "\n" << color << "Invalid paths omitted from search: " << vt.red;
            for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
                std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
            }
            std::cout << color << "\n";
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
            std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << deltaCount << " ISO imported" << vt.green << "]" << vt.bold << "\n";
        }
    };

    pressEnterToContinueWithRepaint(render);
}

/**
 * @brief Logs deduplicated filesystem errors and invalid search paths.
 *
 * Formats and displays issues found during a crawl (invalid paths, I/O
 * errors), then clears both input sets for the next search cycle.
 *
 * - **Signal suppression:** SIGINT is ignored and Ctrl+D disabled at entry;
 *   neither is restored before return.
 * - **"Total files processed: 0":** printed to std::cout only when
 *   @p directoryPaths is empty and @p invalidDirectoryPaths is non-empty.
 * - **Invalid path output:** the header "Invalid paths omitted from search: "
 *   goes to std::cout; the path values and trailing "." go to std::cerr.
 *   Paths are stored uncolored; the red set by the header applies to them.
 * - **Error message output:** entries in @p processedErrorsFind go to
 *   std::cout, separated by newlines (no trailing newline after the last
 *   entry).
 * - **Cleanup:** both @p processedErrorsFind and @p invalidDirectoryPaths are
 *   cleared unconditionally before return.
 *
 * @param invalidDirectoryPaths Inaccessible or invalid paths; cleared on exit.
 * @param directoryPaths        Source directories from the crawl; only
 *                              .empty() is checked, to gate the
 *                              "Total files processed: 0" line.
 * @param processedErrorsFind   Deduplicated formatted error strings; cleared on
 *                              exit.
 */
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();
    std::cout << "\n";

    if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
        std::cout << "\r" << color << "Total files processed: 0" << std::flush;
    }

    if (!invalidDirectoryPaths.empty()) {
        std::cout << "\n\n" << color << "Invalid paths omitted from search: " << vt.red;
        for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
            std::cerr << "'" << *it << "'" << (std::next(it) != invalidDirectoryPaths.end() ? " " : "");
        }
        std::cerr << color;
    }

    if (!processedErrorsFind.empty()) {
        std::cout << "\n\n";
        auto it = processedErrorsFind.begin();
        while (it != processedErrorsFind.end()) {
            std::cout << *it;
            ++it;
            if (it != processedErrorsFind.end()) {
                std::cout << "\n";
            }
        }
    }

    processedErrorsFind.clear();
    invalidDirectoryPaths.clear();
}

/**
 * @brief Renders a color-coded summary of image discovery results and cache
 * deltas.
 *
 * Captures end_time immediately on entry, then prints, in order:
 * -# the "Total files processed" line (only when at least one valid directory
 *    was scanned; with no valid directories verboseFind prints its own
 *    "Total files processed: 0" line),
 * -# the captured verboseFind output (invalid paths omitted from search,
 *    traversal errors, and the "search interrupted by user" message),
 * -# one of three result branches, each gated on
 *    !GlobalState::g_operationCancelled.load():
 *    - **Files found** (!fileNames.empty()): found vs @p currentCacheOld.
 *    - **No new files, cache non-empty** (!newFilesFound && !files.empty() &&
 *      !list): 0 found vs files.size() with an "ls ↵" hint.
 *    - **Cache empty** (files.empty() && !list): 0 found and 0 cached.
 * -# the elapsed time.
 *
 * Steps 1, 2 and 4 are unconditional, so a cancelled search still shows the
 * counter, the interruption message and the elapsed time.
 *
 * **Resize:** verboseFind mutates its set arguments (clears them), so it runs
 * exactly once, before the wait, with its std::cout/std::cerr writes
 * redirected into an ostringstream. The captured text, the file count and the
 * elapsed time are replayed by the render lambda on resize. The prompt is
 * drawn by pressEnterToContinueWithRepaint(render), which owns SIGWINCH:
 * on each resize it clears the screen with "\033[H\033[2J\033[3J" (the third
 * parameter also drops the scrollback the previous frame was pushed into on
 * VTE-based terminals), calls render, then reprints the prompt line. No
 * ListResizeWatcher is installed here — it would race the prompt's own
 * handler and cannot fire during pressEnterToImpl's raw read anyway. If
 * @p repaintCallerHeader is non-empty, render calls it before printing the
 * block, so the caller's screen header (the "FolderPaths ..." prompt line
 * the user just answered) survives a resize; otherwise the block starts
 * from a cleared screen with no caller context.
 *
 * @param fileExtension         Target format label (e.g. ".bin/.img").
 * @param fileNames             Files discovered this run; not modified.
 * @param invalidDirectoryPaths Passed to verboseFind (cleared there).
 * @param newFilesFound         Gates the "no new files" branch; distinct from
 *                              @p fileNames being non-empty.
 * @param list                  Suppresses the second and third branches when
 *                              true; does not suppress the first.
 * @param currentCacheOld       Cache count before this search; shown with
 *                              @p fileNames.size() in the first branch.
 * @param files                 Current working cache; used for branch
 *                              selection and display.
 * @param start_time            Search start; elapsed is computed against
 *                              end_time captured at entry.
 * @param processedErrorsFind   Passed to verboseFind (cleared there).
 * @param directoryPaths        Passed to verboseFind for the
 *                              "Total files processed: 0" gate check; also
 *                              decides whether the real count is printed here.
 * @param totalFiles            Aggregate number of regular files processed by
 *                              the scan, filled in by findFiles.
 * @param repaintCallerHeader   Optional callback that reprints the caller's
 *                              screen header (e.g. the FolderPaths prompt
 *                              line) on a resize during the wait. When empty,
 *                              the block starts from a cleared screen with no
 *                              caller context.
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
                          const std::vector<std::string>& directoryPaths,
                          const std::atomic<size_t>& totalFiles,
                          const std::function<void()>& repaintCallerHeader) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const VerboseAndDatabaseTheme vt = getVerboseTheme();

    const bool hadDirectories = !directoryPaths.empty();

    std::ostringstream verboseFindBuf;
    {
        std::streambuf* oldOut = std::cout.rdbuf(verboseFindBuf.rdbuf());
        std::streambuf* oldErr = std::cerr.rdbuf(verboseFindBuf.rdbuf());
        verboseFind(invalidDirectoryPaths, directoryPaths, processedErrorsFind);
        std::cout.rdbuf(oldOut);
        std::cerr.rdbuf(oldErr);
    }
    std::string verboseFindOutput = verboseFindBuf.str();

    if (hadDirectories && verboseFindOutput.rfind("\n\n\n", 0) == 0) {
        verboseFindOutput.erase(0, 1);
    }

    const bool hasFindContent = verboseFindOutput.size() > 1;

    const size_t totalProcessed = totalFiles.load();

    const auto end_time = std::chrono::high_resolution_clock::now();
    const double total_elapsed_time =
        std::chrono::duration<double>(end_time - start_time).count();

    auto render = [&] {
        SyncOutput sync;
        clearScrollBuffer();

        // Reprint the caller's screen header (the FolderPaths prompt line)
        // before the block, so a resize during the confirmation wait doesn't
        // lose the question the user just answered.
        if (repaintCallerHeader) {
            repaintCallerHeader();
        }

        if (hadDirectories) {
            std::cout << "\n" << "\r" << color << "Total files processed: " << totalProcessed << vt.reset;
        }

        std::cout << verboseFindOutput;

        if (!fileNames.empty() && !GlobalState::g_operationCancelled.load()) {
            std::cout << (hasFindContent ? "\n\n" : "\n")
                      << vt.green << fileNames.size() << " "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.green << "files found" << vt.yellow << "\n"
                      << currentCacheOld << " "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.yellow << "cached entries" << vt.reset << vt.bold << "\n\n";
        }

        if (!newFilesFound && !files.empty() && !list && !GlobalState::g_operationCancelled.load()) {
            std::cout << (hasFindContent ? "\n\n" : "\n")
                      << vt.red << "0 "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.red << "files found " << vt.yellow << "\n"
                      << files.size() << " "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.yellow << "cached entries | "
                      << vt.blue << "ls "
                      << vt.yellow << "↵ to list" << vt.reset << vt.bold << "\n\n";
        }

        if (files.empty() && !list && !GlobalState::g_operationCancelled.load()) {
            std::cout << (hasFindContent ? "\n\n" : "\n")
                      << vt.red << "0" << vt.orange << " {" << fileExtension << "} "
                      << vt.red << "files found\n"
                      << vt.yellow << "0" << vt.orange << " {" << fileExtension << "} "
                      << vt.yellow << "cached entries\n"
                      << vt.reset << vt.bold << "\n";
        }

        std::cout << color << "Time Elapsed: " << std::fixed << std::setprecision(1)
                  << total_elapsed_time << "s" << vt.bold << "\n";
    };

    pressEnterToContinueWithRepaint(render);
    clearScrollBuffer();
}
