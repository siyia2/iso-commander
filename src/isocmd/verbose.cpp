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
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

namespace {
    /** @brief Set to 1 by the SIGWINCH handler; checked on the main loop. */
    volatile std::sig_atomic_t g_winch = 0;

    /** @brief SIGWINCH handler: just flags that the terminal was resized. */
    void handle_winch(int) { g_winch = 1; }

    /**
     * @brief Prints a blank line then "<prompt>" and blocks until Enter, Esc or
     * Ctrl+D.
     *
     * The terminal is switched to raw mode (no canonical buffering, no echo) so
     * Enter, Esc and Ctrl+D are detected immediately and keystrokes never
     * advance the cursor.
     *
     * The cursor stays immediately after the last character of @p prompt, even
     * across resizes: the cursor is saved (DECSC) *on the prompt's own row*
     * (after the leading blank line, not on it). On SIGWINCH the layout is
     * rebuilt from a known anchor rather than patched in place: the cursor is
     * restored (DECRC), moved up one row to the blank separator, erased from
     * there to end-of-screen (which wipes both the separator and any prompt
     * copy the terminal may have reflowed to an adjacent row), and the blank
     * separator + prompt are reprinted with a fresh DECSC on the prompt's row.
     *
     * Rebuilding from the separator — rather than clearing only from the saved
     * cell — makes the layout self-healing: a one-row DECRC error (a rare
     * reflow artifact) moves the whole block down one row but re-establishes
     * the "blank separator, prompt on saved row" invariant, so the next resize
     * is stable instead of compounding the error. This is what prevents the
     * rare duplicate prompt that accumulated over many resizes when the
     * separator was erased in place.
     *
     * Back-to-back SIGWINCH notifications are collapsed into a single repaint
     * (non-blocking poll + flag clear) so two repaints cannot interleave on the
     * same screen.
     *
     * The original termios state and the previous SIGWINCH disposition are
     * restored before returning, and unread input (e.g. the tail of an arrow-key
     * escape sequence) is flushed.
     *
     * @param prompt Text to show after the leading newline (e.g.
     *               "\u21B5 to continue...").
     *
     * @note Requires a POSIX terminal; intended for interactive use only.
     */
    void pressEnterTo(const std::string& prompt) {
        enable_ctrl_d();

        struct termios raw, saved;
        tcgetattr(STDIN_FILENO, &saved);
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

        // Install SIGWINCH handler (keep the previous one).
        g_winch = 0;
        struct sigaction sa{}, old_sa{};
        sa.sa_handler = handle_winch;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;                 // we want poll() to return EINTR
        sigaction(SIGWINCH, &sa, &old_sa);

        // Prints the prompt text only (no leading '\n'); cursor ends right
        // after the prompt.
        auto drawPrompt = [&]() {
            std::cout << color << prompt << UI::Palette::BoldReset;
            std::cout.flush();
        };

        // Leading blank separator, then save the cursor ON the prompt's row
        // (not on the blank line). Saved cell and prompt text then share a row
        // and reflow together, so DECRC lands on the prompt.
        std::cout << "\n";
        std::cout << "\0337";             // DECSC
        drawPrompt();

        bool done = false;
        while (!done) {
            if (g_winch) {
                g_winch = 0;

                // Collapse back-to-back resize notifications into one repaint:
                // a second SIGWINCH delivered while we were between the flag
                // check and here would otherwise trigger a second repaint that
                // can interleave with this one and leave a stale prompt.
                {
                    struct pollfd drain{ STDIN_FILENO, POLLIN, 0 };
                    poll(&drain, 1, 0);
                }
                g_winch = 0;

                // Rebuild the layout from the blank separator above the saved
                // prompt row, so the result is identical to the first draw:
                //   \0338       → DECRC to the saved prompt row
                //   \033[1A     → up to the blank separator (or, on a rare
                //                 one-row-low DECRC, onto the actual prompt)
                //   \r\033[0J   → erase separator .. end-of-screen (wipes any
                //                 prompt copy the terminal reflowed here)
                //   \n          → reprint the blank separator
                //   \0337       → DECSC on the fresh prompt row
                //   drawPrompt()→ reprint the prompt
                std::cout << "\0338"
                          << "\033[1A"
                          << "\r"
                          << "\033[0J"
                          << "\n"
                          << "\0337";
                drawPrompt();
            }

            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int r = poll(&pfd, 1, 100);   // short timeout so we re-check flag
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) continue;         // timeout: loop to re-check g_winch

            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            if (ch == '\n' || ch == '\r' || ch == 4 || ch == 27) done = true;
        }

        tcflush(STDIN_FILENO, TCIFLUSH);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        sigaction(SIGWINCH, &old_sa, nullptr);
    }
} // namespace

/**
 * @brief Waits for Enter, Esc or Ctrl+D before allowing another attempt.
 *
 * Shows "\u21B5 to try again...". The cursor stays at the end of the prompt
 * across resizes; see pressEnterTo() for the terminal handling.
 */
void pressEnterToTry()      { pressEnterTo("\u21B5 to try again..."); }

/**
 * @brief Waits for Enter, Esc or Ctrl+D before returning to the previous menu
 * or state.
 *
 * Shows "\u21B5 to return...". The cursor stays at the end of the prompt
 * across resizes; see pressEnterTo() for the terminal handling.
 */
void pressEnterToReturn()   { pressEnterTo("\u21B5 to return..."); }

/**
 * @brief Waits for Enter, Esc or Ctrl+D before continuing.
 *
 * Shows "\u21B5 to continue...". The cursor stays at the end of the prompt
 * across resizes; see pressEnterTo() for the terminal handling.
 */
void pressEnterToContinue() { pressEnterTo("\u21B5 to continue..."); }

/**
 * @brief Prints categorized operation results, sorted case-insensitively.
 *
 * Manages terminal state and prints sorted batches of success, warning and
 * error messages.
 *
 * - **Signal safety:** ignores @c SIGINT and disables @c Ctrl+D so results are
 *   read before returning to the main menu.
 * - **No copies:** sorts and prints @c std::string_view views of the input sets.
 * - **Case-insensitive:** @c std::lexicographical_compare with @c std::tolower.
 * - **Stream routing:** error-type items go to @c std::cerr with colors from
 *   @c VerboseAndDatabaseTheme.
 *
 * @param primarySet   Main data set (e.g. successful unmounts or conversions).
 * @param secondarySet Supporting data (e.g. already unmounted or skipped items).
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
 * Examines @c verboseSets: prints a "no valid input" notice, or delegates to
 * @c verbosePrint when @p verbose is set.
 *
 * - **"No valid input" check:** if @c uniqueErrorTokenMessages is non-empty and
 *   @c operationFailed, @c operationSkipped and @c operationCompleted are all
 *   empty, clears the scroll buffer, prints the notice, and waits for Enter.
 * - **Verbose output:** if @p verbose is true, clears the scroll buffer and
 *   calls @c verbosePrint. Mount operations pass @c operationSkipped and use
 *   layout mode @c 2; all others pass an empty set and use mode @c 1.
 * - **History clear:** after a destructive operation (@c mv, @c rm, @c umount),
 *   if @p isFiltered and @p umountMvRmBreak are both true, Readline history is
 *   cleared so stale indices cannot be re-run.
 *
 * @param operation      Action string ("mount", "mv", "rm", "umount", ...);
 *                       drives the @c verbosePrint layout mode and the
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

        clearScrollBuffer();

        std::cout << "\n" << (c.red)
                  << "No valid input provided."
                  << UI::Palette::BoldReset << "\n";

        pressEnterToReturn();
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
 * @brief Counts entries in @p allIsoFiles that are not in @p globalIsoFileList.
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
 * - **Signal guarding:** @c SIGINT is ignored and @c Ctrl+D disabled at entry;
 *   neither is restored before return.
 * - **Cache sync:** if @p newISOFound is true on entry, calls
 *   @c loadFromDatabase to reconcile @c globalIsoFileList with disk before any
 *   output is produced.
 * - **Persistence:** calls @c saveToDatabase (passing @c &newISOFound, which
 *   may be mutated) unless @c GlobalState::g_operationCancelled is set.
 * - **User feedback:** prints elapsed time, then a color-coded outcome from one
 *   of five branches: Cancelled; save failed with files and delta; no valid
 *   paths; files present but no delta (save not attempted or failed); no ISOs
 *   found; or successful import with the delta count from
 *   @c countDifferentEntries.
 * - **Invalid path / error reporting:** printed before the save step. When
 *   @p totalFiles is zero and @p validPaths is empty, also emits a "Total files
 *   processed: 0" line before the invalid-path list.
 *
 * @param allIsoFiles         Discovered ISO paths to be saved.
 * @param totalFiles          Atomic count of files scanned; gates the
 *                            "Total files processed: 0" line.
 * @param validPaths          Base directories successfully traversed; empty
 *                            triggers the "Lack of valid paths" branch.
 * @param invalidPaths        Directories skipped due to permissions or
 *                            existence errors.
 * @param uniqueErrorMessages Deduplicated system-level I/O errors.
 * @param newISOFound         On entry, whether the scan produced delta changes
 *                            (gates the @c loadFromDatabase call). Passed by
 *                            pointer to @c saveToDatabase, which may modify it.
 * @param start_time          Timestamp of the refresh origin, used to compute
 *                            elapsed time.
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
 * @brief Logs deduplicated filesystem errors and invalid search paths.
 *
 * Formats and displays issues found during a crawl (invalid paths, I/O errors),
 * then clears both input sets for the next search cycle.
 *
 * - **Signal suppression:** @c SIGINT is ignored and @c Ctrl+D disabled at
 *   entry; neither is restored before return.
 * - **"Total files processed: 0":** printed to @c std::cout only when
 *   @p directoryPaths is empty and @p invalidDirectoryPaths is non-empty.
 * - **Invalid path output:** the header "Invalid paths omitted from search: "
 *   goes to @c std::cout; the path values and trailing "." go to @c std::cerr.
 * - **Error message output:** entries in @p processedErrorsFind go to
 *   @c std::cout, separated by newlines (no trailing newline after the last
 *   entry).
 * - **Cleanup:** both @p processedErrorsFind and @p invalidDirectoryPaths are
 *   cleared unconditionally before return.
 *
 * @param invalidDirectoryPaths Inaccessible or invalid paths; cleared on exit.
 * @param directoryPaths        Source directories from the crawl; only
 *                              @c .empty() is checked, to gate the
 *                              "Total files processed: 0" line.
 * @param processedErrorsFind   Deduplicated formatted error strings; cleared on
 *                              exit.
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
 * Captures @c end_time immediately on entry, then selects one of three display
 * branches based on outcome. Elapsed time and @c pressEnterToContinue always
 * run at the end, regardless of branch or cancellation.
 *
 * Branches (all gated on @c !GlobalState::g_operationCancelled.load()):
 * - **Files found** (@c !fileNames.empty()): prints found vs
 *   @p currentCacheOld. Does not call @c verboseFind. Not gated on @p list.
 * - **No new files, cache non-empty** (@c !newFilesFound && !files.empty() && !list):
 *   calls @c verboseFind, then prints 0 found vs @c files.size() with an
 *   @c "ls ↵" hint.
 * - **Cache empty** (@c files.empty() && !list): calls @c verboseFind, then
 *   prints 0 found and 0 cached.
 *
 * If @c g_operationCancelled is set, all three branches are skipped; only
 * elapsed time and the Enter prompt are shown.
 *
 * @param fileExtension         Target format label (e.g. ".bin/.img").
 * @param fileNames             Files discovered this run; not modified.
 * @param invalidDirectoryPaths Passed to @c verboseFind (cleared there).
 * @param newFilesFound         Gates the "no new files" branch; distinct from
 *                              @p fileNames being non-empty.
 * @param list                  Suppresses the second and third branches when
 *                              true; does not suppress the first.
 * @param currentCacheOld       Cache count before this search; shown with
 *                              @p fileNames.size() in the first branch.
 * @param files                 Current working cache; used for branch selection
 *                              and display.
 * @param start_time            Search start; elapsed is computed against
 *                              @c end_time captured at entry.
 * @param processedErrorsFind   Passed to @c verboseFind (cleared there).
 * @param directoryPaths        Passed to @c verboseFind for the
 *                              "Total files processed: 0" gate check.
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
    {
        SyncOutput sync;

        // Files were found.
        if (!fileNames.empty() && !GlobalState::g_operationCancelled.load()) {
            std::cout << "\n\n"
                      << vt.green << fileNames.size() << " "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.green << "files found" << vt.yellow << "\n"
                      << currentCacheOld << " "
                      << vt.orange << "{" << fileExtension << "} "
                      << vt.yellow << "cached entries" << vt.reset << vt.bold << "\n\n";
        }

        // No new files, but the cache is non-empty.
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

        // No files were found.
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
    }

    pressEnterToContinue();
    clearScrollBuffer();
}
