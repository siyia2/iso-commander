// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <readline/history.h>
#include <readline/readline.h>

import std;
import isocmd.core.cancellation;
import isocmd.core.globalMutexes;
import isocmd.database.databaseOps;
import isocmd.input.history;
import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.search.search;
import isocmd.threadpool.threadpool;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.display.readlineResize;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;
import isocmd.util.stringManipulation;

namespace fs = std::filesystem;

/**
 * @brief Database operations and file scanning for ISO files.
 *
 * Scans directories for ISO files, imports them into the ISO database, and
 * drives the interactive directory-prompt UI. Disc-image (BIN/IMG/MDF/NRG/CHD/
 * DAA/GBI) RAM-cache handling lives in the companion image source file.
 */

/**
 * @brief Handles special user commands such as help (?) and database statistics or clear commands.
 *
 * Recognizes "?" (help), "*stats"/"!clr"/"!clr_paths"/"!clr_filter" (delegated
 * to @c dispatchDatabaseCommands), and whitespace-only input (swallowed). Any other
 * input is treated as regular path data.
 *
 * @param input               The trimmed user input string.
 * @param repaintCallerPrompt Passed through to @c dispatchDatabaseCommands; invoked on
 *                            resize repaints of the statistics/clear
 *                            confirmation screens.
 * @return true if a command was recognized and handled (signaling the loop to
 *         continue), false if the input is regular path data.
 */
static bool handleDatabaseCommands(const std::string& input, const std::function<void()>& repaintCallerPrompt) {
    if (input == "?") {
        bool isCpMv = false, import2ISO = true;
        helpSearches(isCpMv, import2ISO, false);
        return true;
    }
    if (input == "*stats" || input == "!clr" || input == "!clr_paths" || input == "!clr_filter") {
        std::string mutableInput = input; // Create a non-const copy for dispatchDatabaseCommands
        dispatchDatabaseCommands(mutableInput, repaintCallerPrompt);
        return true;
    }
    if (input.find_first_not_of(" \t\n\r") == std::string::npos) {
        return true;
    }
    return false;
}

/**
 * @brief Parses semicolon-delimited directory paths from the input, validates,
 *        normalizes, removes duplicates, and filters out parent-covered paths.
 *
 * Each token is trimmed before validation; empty tokens are skipped and
 * invalid tokens are recorded in @p invalidPaths. Valid tokens are appended
 * (in entered order) to @p cleanedInput, joined with ';'. A non-empty
 * @p cleanedInput is inserted into readline history. The normalized,
 * deduplicated, sorted path list is then filtered so no path is a descendant
 * of an earlier parent entry.
 *
 * @param input        The raw trimmed input string containing semicolon-separated paths.
 * @param validPaths   Output vector that receives the sorted, unique, and filtered valid paths.
 * @param invalidPaths Output set that collects any paths that failed validation.
 * @return std::string The cleaned input string containing only successfully validated paths,
 *         used for history tracking.
 */
static std::string parseAndFilterPaths(
    const std::string& input,
    std::vector<std::string>& validPaths,
    std::unordered_set<std::string>& invalidPaths
) {
    std::istringstream iss(input);
    std::string path;
    std::unordered_set<std::string> uniqueNormalizedPaths;
    std::string cleanedInput;

    auto normalizePath = [](const std::string& p) {
        std::string n = fs::path(p).lexically_normal().string();
        if (n.size() > 1 && n.back() == '/')
            n.pop_back();
        return n;
    };

    while (std::getline(iss, path, ';')) {
        path = trimWhitespace(path);
        if (path.empty()) continue;
        if (!isValidDirectory(path)) {
            invalidPaths.insert(path);
            continue;
        }

        if (!cleanedInput.empty()) cleanedInput += ';';
        cleanedInput += path;

        std::string normPath = normalizePath(path);
        if (uniqueNormalizedPaths.insert(normPath).second) {
            validPaths.push_back(normPath);
        }
    }

    if (!cleanedInput.empty()) {
        add_history(cleanedInput.c_str());
    }

    std::sort(validPaths.begin(), validPaths.end());

    std::vector<std::string> filteredPaths;
    for (const auto& p : validPaths) {
        if (filteredPaths.empty() || !p.starts_with(filteredPaths.back() + "/")) {
            filteredPaths.push_back(p);
        }
    }
    validPaths = std::move(filteredPaths);
    return cleanedInput;
}

/**
 * @brief Dispatches and executes parallel directory scans across the static thread pool
 *        for all validated paths.
 *
 * Resets @c GlobalState::g_operationCancelled before enqueuing one @c traverse
 * task per path, then blocks until every submitted future completes. Each
 * task shares @p allIsoFiles, @p uniqueErrorMessages, @p totalFiles, and the
 * two output mutexes.
 *
 * @param validPaths           The filtered list of valid directory paths to traverse.
 * @param maxDepth             Maximum directory recursion depth (-1 for unlimited).
 * @param promptFlag           Passed to traverse() to control progress and error reporting.
 * @param allIsoFiles          Output vector collecting all discovered ISO files.
 * @param uniqueErrorMessages  Output set collecting unique traversal error messages.
 * @param totalFiles           Atomic counter tracking the total number of files processed.
 */
static void executeParallelTraversal(
    const std::vector<std::string>& validPaths,
    int maxDepth,
    bool promptFlag,
    std::vector<std::string>& allIsoFiles,
    std::unordered_set<std::string>& uniqueErrorMessages,
    std::atomic<size_t>& totalFiles
) {
    auto& pool = getStaticThreadPool();
    std::vector<std::future<void>> futures;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;
    GlobalState::g_operationCancelled.store(false);

    for (const auto& validPath : validPaths) {
        futures.emplace_back(
            pool.enqueue([path = validPath, &allIsoFiles, &uniqueErrorMessages, &totalFiles,
                          &processMutex, &traverseErrorMutex, maxDepth, promptFlag]() {
                traverse(path, allIsoFiles, uniqueErrorMessages, totalFiles,
                         processMutex, traverseErrorMutex, maxDepth, promptFlag);
            })
        );
    }

    for (auto& future : futures) {
        future.wait();
    }
}

/**
 * @brief Constructs the prompt string for database directory scanning based on the current theme.
 *
 * @param dt The verbose and database theme configuration.
 * @return Formatted prompt string with ANSI color codes.
 */
static std::string constructDatabasePrompt(const VerboseAndDatabaseTheme& dt) {
    std::string prompt("\001");
    prompt
        .append(dt.green)
        .append("\002FolderPaths\001")
        .append(dt.blue)
        .append("\002 ↵ to search for ")
        .append(dt.green)
        .append("\002.iso\001")
        .append(dt.blue)
        .append("\002 entries and import into \001")
        .append(dt.green)
        .append("\002local\001")
        .append(dt.blue)
        .append("\002 DB, ? for help:\n\001")
        .append(dt.reset)
        .append("\002");
    return prompt;
}

/**
 * @brief Processes user input paths, executes parallel traversal, saves history, and reports results.
 *
 * Parses and filters input paths, and if any valid paths remain, runs a
 * parallel traversal, restores input state, and reports the results through
 * saveAndReportResultsForDatabase(). When no valid paths remain but invalid
 * ones were provided, reports the invalid-path-only result directly. Always
 * resets the cancellation flag before reporting so the results screen is not
 * immediately suppressed.
 *
 * @param input              Cleaned and trimmed user input string.
 * @param promptFlag         Passed to traverse() to control progress and error reporting.
 * @param maxDepth           Maximum directory recursion depth (-1 for unlimited).
 * @param filterHistory      Whether to load and save readline history.
 * @param newISOFound        Reference to flag indicating if new ISOs were found.
 * @param repaintCallerPrompt Callback that reprints the caller's prompt header and
 *                           typed line. Forwarded to
 *                           @c saveAndReportResultsForDatabase as
 *                           @c repaintCallerHeader, so the question the user
 *                           answered survives a SIGWINCH during the
 *                           confirmation wait. Not invoked on the first draw.
 */
static void processPathTraversalAndReporting(const std::string& input,
                                             bool promptFlag,
                                             int maxDepth,
                                             bool filterHistory,
                                             bool& newISOFound,
                                             const std::function<void()>& repaintCallerPrompt) {
    std::vector<std::string> validPaths;
    std::unordered_set<std::string> invalidPaths;
    std::unordered_set<std::string> uniqueErrorMessages;
    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};

    std::cout << "\033[3H\033[J\n";
    disableInput();

    const auto start_time = monotonicNowNs();

    std::string cleanedInput = parseAndFilterPaths(input, validPaths, invalidPaths);

    if (validPaths.empty()) {
        flushStdin();
        restoreInput();
        resetReadlinePagination();
        if (!invalidPaths.empty()) {
            GlobalState::g_operationCancelled.store(false);
            DatabaseRefreshResult refreshResult{
                .allIsoFiles          = allIsoFiles,
                .validPaths           = validPaths,
                .invalidPaths         = invalidPaths,
                .uniqueErrorMessages  = uniqueErrorMessages,
                .totalFiles           = totalFiles,
                .newISOFound          = newISOFound,
                .start_time_ns        = start_time,
            };
            saveAndReportResultsForDatabase(refreshResult, repaintCallerPrompt);
        }
        return;
    }

    executeParallelTraversal(validPaths, maxDepth, promptFlag, allIsoFiles, uniqueErrorMessages, totalFiles);

    flushStdin();
    restoreInput();
    resetReadlinePagination();

    std::cout << "\r" << color << "Total files processed: " << totalFiles;

    if (!invalidPaths.empty() || !validPaths.empty()) {
        std::cout << "\n";
    }

    if (!validPaths.empty() && !input.empty()) {
        saveHistory(filterHistory);
    }

    DatabaseRefreshResult refreshResult{
        .allIsoFiles         = allIsoFiles,
        .validPaths          = validPaths,
        .invalidPaths        = invalidPaths,
        .uniqueErrorMessages = uniqueErrorMessages,
        .totalFiles          = totalFiles,
        .newISOFound         = newISOFound,
        .start_time_ns       = start_time,
    };
    saveAndReportResultsForDatabase(refreshResult, repaintCallerPrompt);
}

/**
 * @brief Prompts for directories and imports discovered ISO files into the database.
 *
 * Accepts semicolon-delimited directory paths, trims and validates each token,
 * removes duplicate normalized paths and paths covered by an earlier parent
 * directory, then scans the remaining directories in parallel up to
 * @p maxDepth. Discovered files and scan errors are passed to
 * saveAndReportResultsForDatabase().
 *
 * The prompt supports readline history, help and database commands. Invalid
 * paths are excluded from the history entry; valid paths are stored trimmed
 * but otherwise as entered. All scan tasks are joined before the iteration
 * ends, including after cancellation, because they reference iteration-local
 * data.
 *
 * @par Repaint
 * The line the user typed is kept as a std::string copy. Whenever the screen
 * is repainted after the prompt (the results report, database commands such
 * as "*stats" and "!clr*", or a terminal resize while waiting), the prompt
 * header is re-printed, followed by the typed line in the theme color on its
 * own line directly under it, so the verbose output below always starts on a
 * fresh line and the question the user answered stays visible. The typed
 * line is not restored into the next prompt, which always starts empty.
 *
 * @param promptFlag    Passed to traverse() to control progress and error
 *                      reporting.
 * @param maxDepth      Maximum directory recursion depth (-1 for unlimited).
 * @param filterHistory Whether to load and save readline history.
 * @param newISOFound   Passed to saveAndReportResultsForDatabase(), which
 *                      forwards it to saveToDatabase().
 */
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, bool& newISOFound) {
    PendingReadlineTextClearGuard pendingTextGuard;

    auto rearmSetup = [&]() {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSearches();
        rl_attempted_completion_function = my_special_completion_entry;
        GlobalState::g_operationCancelled.store(false);
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 1;
    };

    while (true) {
        rearmSetup();
        clearScrollBuffer();
        SearchReadlineGuard guard;
        try {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();

            loadHistory(filterHistory);
            RestoreReadlineBuffer();

            const std::string prompt = constructDatabasePrompt(dt);

            const auto repaintPrompt = [&] {
                SyncOutput sync;
                clearScrollBuffer();
                std::cout << promptHeaderForRepaint(prompt) << "\n";  // now on the input line, like the normal path
                repaintCompletionListing();                           // saves/restores at the input line
                std::cout << "\033[A";                                // back to end of header; watcher prints the '\n'
            };

            std::string typedLine;

            const auto repaintCallerPrompt = [&] {
                const std::string header = promptHeaderForRepaint(prompt);
                std::cout << header;

                size_t end = header.size();
                while (end > 0) {
                    const char c = header[end - 1];
                    if (c == '\001' || c == '\002') { --end; continue; }
                    if (c == 'm') {
                        const size_t esc = header.rfind('\033', end - 1);
                        if (esc != std::string::npos && esc + 1 < end && header[esc + 1] == '[') {
                            end = esc;
                            continue;
                        }
                    }
                    break;
                }
                if (end == 0 || header[end - 1] != '\n') std::cout << "\n";

                std::cout << UI::Palette::BoldReset << typedLine << dt.reset << "\n";
            };

            RlLine searchQuery = readlineWithResize(prompt, repaintPrompt);
            if (!searchQuery) return;

            typedLine = searchQuery.get();

            armPendingInputRestore();
            rl_startup_hook = nullptr;

            std::string input = trimWhitespace(searchQuery.get());
            if (input == "\x1b") return;

            if (handleDatabaseCommands(input, repaintCallerPrompt)) {
                continue;
            }

            processPathTraversalAndReporting(input, promptFlag, maxDepth, filterHistory, newISOFound, repaintCallerPrompt);

        } catch (const std::exception& e) {
            const VerboseAndDatabaseTheme dt = getDatabaseTheme();

            auto render = [&] {
                SyncOutput sync;
                std::cerr << "\n" << dt.red << "Unable to access ISO database: "
                          << e.what() << dt.reset << std::endl;
            };
            pressEnterToContinueWithRepaint(render);
        }
    }
}

/**
 * @brief Recursively traverses a directory to find ISO files.
 *
 * Walks the directory tree depth-first, collecting regular files with a
 * case-insensitive ".iso" extension and aggregating filesystem errors.
 * Discovered paths are appended to the shared output in batches.
 *
 * Cancellation is checked per entry; the first observation clears
 * @p uniqueErrorMessages and inserts a single "interrupted by user" notice
 * (guarded by a per-call atomic). Progress counting only runs when
 * @p promptFlag is true, throttled to one update per 100 files. Filesystem
 * errors are recorded only when @p promptFlag is true.
 *
 * @param path                Starting directory.
 * @param isoFiles            Output vector for discovered ISO paths.
 * @param uniqueErrorMessages Set of unique error messages.
 * @param totalFiles          Atomic counter of regular files processed for
 *                            progress reporting when @p promptFlag is true.
 * @param traverseFilesMutex  Protects @p isoFiles.
 * @param traverseErrorsMutex Protects @p uniqueErrorMessages.
 * @param maxDepth            Maximum recursion depth (-1 for unlimited).
 * @param promptFlag          Enables progress counting and error reporting.
 */
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles,
              std::unordered_set<std::string>& uniqueErrorMessages,
              std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex,
              std::mutex& traverseErrorsMutex, int maxDepth, bool promptFlag) {

    const size_t BATCH_SIZE = 100;
    std::vector<std::string> localIsoFiles;
    std::atomic<bool> g_CancelledMessageAdded{false};

    const VerboseAndDatabaseTheme dt = getDatabaseTheme();

    auto iequals = [](const std::string_view& a, const std::string_view& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                         [](unsigned char a, unsigned char b) {
                             return std::tolower(a) == std::tolower(b);
                         });
    };

    try {
        auto options = std::filesystem::directory_options::none;
        for (auto it = std::filesystem::recursive_directory_iterator(path, options);
             it != std::filesystem::recursive_directory_iterator(); ++it) {

            if (GlobalState::g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(traverseErrorsMutex);
                    uniqueErrorMessages.clear();

                    std::string msg = "\n" + dt.yellow + "ISO search interrupted by user" + dt.reset;
                    uniqueErrorMessages.insert(msg);
                }
                break;
            }

            if (maxDepth >= 0 && it.depth() > maxDepth) {
                it.disable_recursion_pending();
                continue;
            }

            const auto& entry = *it;

            if (promptFlag && entry.is_regular_file()) {
                uint64_t val = totalFiles.fetch_add(1, std::memory_order_relaxed) + 1;
                if (val % 100 == 0) {
                    std::lock_guard<std::mutex> lock(GlobalMutexes::couNtMutex);
                    std::cout << "\r" << color << "Total files processed: " << val << std::flush;
                }
            }

            if (!entry.is_regular_file()) continue;

            const auto& filePath = entry.path();

            if (!iequals(filePath.extension().string(), ".iso")) continue;

            localIsoFiles.push_back(filePath.string());

            if (localIsoFiles.size() >= BATCH_SIZE) {
                std::lock_guard<std::mutex> lock(traverseFilesMutex);
                isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
                localIsoFiles.clear();
            }
        }

        if (!localIsoFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
        }

    } catch (const std::filesystem::filesystem_error& e) {
        std::string formattedError = "\n" + dt.red + "Error: " + path.string() + " - " +
                                     e.what() + dt.reset;

        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
            uniqueErrorMessages.insert(formattedError);
        }
    }
}
