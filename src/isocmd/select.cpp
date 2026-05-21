// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../caches.h"
#include "../databaseOps.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../select.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Routes validated user input to specific filesystem or mounting logic.
 *
 * This function acts as a dispatcher, determining the "active" file list based on
 * the current UI state (filtered vs. global) and the nature of the operation.
 *
 * @details **State Transitions:**
 * - **List Selection:** Prioritizes @p filteredFiles if @p isFiltered is true.
 *   Otherwise, defaults to @c globalIsoFileList (or @p isoDirs for unmounting).
 * - **UI Locking:** Sets @p isAtISOList to @c false to prevent concurrent UI
 *   refreshes during blocking operations.
 * - **Destructive Safety:** Sets @p umountMvRmBreak to force a view refresh
 *   following operations that modify the source list (move, remove, unmount).
 *
 * @param inputString      Raw user string (usually indices or paths).
 * @param isMount          True if the intent is to mount the selection.
 * @param isUnmount        True if the intent is to unmount the selection.
 * @param write            True if the intent is a USB bitstream write.
 * @param[in,out] isFiltered Updated if the operation invalidates the current filter.
 * @param filteredFiles    The subset of files currently visible in the UI.
 * @param isoDirs          The list of currently active mount points.
 * @param[out] needsClrScrn Set to true to trigger a full TUI redraw.
 * @param operation        String identifier for logging and UI feedback.
 * @param isAtISOList      Atomic toggle to signal the UI is busy processing.
 * @param umountMvRmBreak  Flag to interrupt the persistent loop on destructive actions.
 * @param filterHistory    Flag to clear/update the readline search history.
 * @param newISOFound      Atomic signal for background filesystem watchers.
 */
void processOperationForSelectedIsoFiles(const std::string& inputString, bool isMount, bool isUnmount, bool write, bool& isFiltered, const std::vector<std::string>& filteredFiles,
										 std::vector<std::string>& isoDirs, bool& needsClrScrn,
										 const std::string& operation, std::atomic<bool>& isAtISOList,
										 bool& umountMvRmBreak, bool& filterHistory, std::atomic<bool>& newISOFound) {

    clearScrollBuffer();
    needsClrScrn = true;
    bool verbose = false;

    if (isMount || isUnmount) {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles :
                                                    (isUnmount ? isoDirs : GlobalCaches::globalIsoFileList);

        if (isUnmount) {
            umountMvRmBreak = true;
        }

        processInputForMountOrUmount(inputString, activeList, umountMvRmBreak, verbose, isUnmount);
    } else if (write) {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalCaches::globalIsoFileList;
        writeToUsb(inputString, activeList);
    } else {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalCaches::globalIsoFileList;
        processInputForCpMvRm(inputString, activeList, operation, umountMvRmBreak, filterHistory, verbose, newISOFound);
    }

    handleSelectIsoFilesResults(operation, verbose, isMount, isFiltered, umountMvRmBreak, isUnmount, needsClrScrn);
}

/**
 * @brief Parses and queues semicolon-delimited indices for batch processing.
 *
 * Separates "induction" input (e.g., `1 2 3;`) from direct commands. This allows
 * the user to stage multiple indices into a pending queue without executing
 * them immediately.
 *
 * @details **Parsing Rules:**
 * - **Trigger:** Requires a semicolon (`;`) to initiate induction logic.
 * - **Sanity Check:** Aborts if a forward slash (`/`) is found, assuming the
 *   input is a direct file path rather than an index list.
 * - **Deduplication:** Uses an internal @c std::unordered_set to ensure only
 *   unique indices are added to the @p pendingIndices vector.
 *
 * @param inputString          The raw line from @c readline.
 * @param[out] pendingIndices  Vector where unique staged tokens are appended.
 * @param[out] hasPendingProcess Set to true if at least one new index was queued.
 * @param[out] needsClrScrn    Signals the UI to refresh to show the updated queue.
 * @return **true** if the input was valid induction syntax and was queued.
 * @return **false** if the input should be treated as a standard command.
 */
bool handlePendingInduction(const std::string& inputString, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& needsClrScrn) {
    if (inputString.find(';') == std::string::npos || inputString.find('/') != std::string::npos) {
        return false;
    }

    std::string indicesInput = inputString.substr(0, inputString.find(';'));

    while (!indicesInput.empty() && std::isspace(indicesInput.back())) {
        indicesInput.pop_back();
    }

    if (!indicesInput.empty()) {
        std::istringstream iss(indicesInput);
        std::string token;
        std::unordered_set<std::string> uniqueTokens;

        for (const auto& index : pendingIndices) {
            uniqueTokens.insert(index);
        }

        std::vector<std::string> newIndices;

        while (iss >> token) {
            if (uniqueTokens.find(token) == uniqueTokens.end()) {
                newIndices.push_back(token);
                uniqueTokens.insert(token);
            }
        }

        pendingIndices.insert(pendingIndices.end(), newIndices.begin(), newIndices.end());

        if (!pendingIndices.empty()) {
            hasPendingProcess = true;
            needsClrScrn = true;
            return true;
        }
    }
    return false;
}

/**
 * @brief Executes a batch operation by serializing and routing queued indices.
 *
 * Converts the @p pendingIndices vector into a space-delimited string and
 * dispatches it to @c processOperationForSelectedIsoFiles. This acts as the
 * "commit" phase for staged user selections.
 *
 * @details **Workflow:**
 * - **Command Guard:** Only executes if @p inputString exactly matches "proc".
 * - **Serialization:** Concatenates all queued tokens into a single command-line
 *   compatible string.
 * - **Execution:** Routes the combined input through the standard operation
 *   logic (Mount, Unmount, Write, or CP/MV/RM).
 *
 * @param inputString      User command (must be "proc" to trigger).
 * @param pendingIndices   The collection of staged index tokens.
 * @param hasPendingProcess Flag indicating if a queue currently exists.
 * @param[in] isMount,isUnmount,write Operational context flags.
 * @param[out] needsClrScrn Set to true upon successful routing to refresh the UI.
 * @return **true** if the "proc" command was recognized and executed.
 * @return **false** if the queue was empty or the command was invalid.
 */
bool handlePendingProcess(const std::string& inputString,std::vector<std::string>& pendingIndices,bool& hasPendingProcess,bool isMount,bool isUnmount,
						  bool write,bool isFiltered, std::vector<std::string>& filteredFiles,
						  std::vector<std::string>& isoDirs, bool& needsClrScrn, const std::string& operation,
						  std::atomic<bool>& isAtISOList, bool& umountMvRmBreak,
						  bool& filterHistory, std::atomic<bool>& newISOFound) {

    if (hasPendingProcess && !pendingIndices.empty() && inputString == "proc") {
        std::string combinedIndices = "";
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            combinedIndices += pendingIndices[i];
            if (i < pendingIndices.size() - 1) {
                combinedIndices += " ";
            }
        }

        processOperationForSelectedIsoFiles(combinedIndices, isMount, isUnmount, write, isFiltered,
                     filteredFiles, isoDirs,
                     needsClrScrn, operation, isAtISOList, umountMvRmBreak,
                     filterHistory, newISOFound);

        return true;
    }

    return false;
}

/**
 * @brief Background worker thread that synchronizes the TUI with filesystem changes.
 *
 * Polling function that waits for an update signal and refreshes the display
 * without interrupting the user's current Readline input session.
 *
 * @details **Concurrency & UI Logic:**
 * - **Thread Safety:** Utilizes @c std::shared_ptr<RefreshState> to ensure the
 *   thread accesses valid data even if the parent scope has exited.
 * - **Input Protection:** Only triggers a refresh if @c isImportRunning is false
 *   to avoid race conditions or database locks.
 * - **Readline Integration:** Calls @c rl_on_new_line() and @c rl_redisplay()
 *   to gracefully repaint the prompt underneath the new list output.
 * - **Auto-Termination:** Once the refresh logic executes, the thread clears
 *   atomic flags and terminates (non-looping design).
 *
 * @param timeoutMS        Interval to wait before checking state (in milliseconds).
 * @param isAtISOList      Atomic flag; refresh only occurs if the user is in the list view.
 * @param isImportRunning  Atomic flag preventing refresh during active I/O tasks.
 * @param updateHasRun     Atomic signal used to acknowledge the refresh request.
 * @param newISOFound      Atomic signal cleared upon completion.
 * @param state            Shared state container for UI indices, pagination, and filters.
 */
void refreshListAfterAutoUpdate(int timeoutMS, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning,
                                std::atomic<bool>& updateHasRun, std::atomic<bool>& newISOFound,
                                std::shared_ptr<RefreshState> state) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMS));

        if (!isImportRunning.load()) {
            if (isAtISOList.load()) {
                loadAndDisplayIso(state->filteredFiles, state->isFiltered, state->listSubtype,
                                  state->umountMvRmBreak, state->pendingIndices, state->hasPendingProcess,
                                  state->currentPage, state->originalPage, isImportRunning);
                std::cout << "\n";
                rl_on_new_line();
                rl_redisplay();
            }
            updateHasRun.store(true);
            newISOFound.store(false);
            break;
        }
    }
}

/**
 * @brief Interactive TUI for batch ISO operations (mount, umount, cp, mv, rm, write2usb).
 *
 * Provides a paginated, multi-layer filterable interface for selecting ISO files
 * and executing system operations.
 *
 * @details **Key Behaviors:**
 * - **Dynamic Context:** Automatically toggles between the global ISO database and active
 *   mount points based on the @p operation.
 * - **Thread-Safe Refresh:** Utilizes @c std::shared_ptr<RefreshState> to share UI state
 *   with a detached background refresh thread, preventing use-after-free errors.
 * - **Stacked Filtering:** Supports successive narrowing of results. The @c '<' command
 *   unwinds the filter stack or returns to the previous menu.
 * - **Two-Phase Execution:** Implements an "Induction" model where indices are staged
 *   into @c pendingIndices (delimited by @c ';') and executed via the @c "proc" command.
 * - **Terminal Integrity:** Manages @c Readline keybindings and uses ANSI escape
 *   sequences to maintain a static-feeling interface during input.
 *
 * @param operation         Target system action ("mount", "umount", "rm", etc.).
 * @param updateHasRun      Atomic signal for background list updates.
 * @param isAtISOList       Atomic flag used to gate background thread UI painting.
 * @param isImportRunning   Prevents UI refresh during active database imports.
 * @param newISOFound       Trigger for the refresh thread to repaint the list.
 * @param stopImport        Atomic signal to cancel background import tasks.
 * @param backgroundThreads Container for managing detached worker lifetimes.
 * @param search            Context flag for manual vs. automatic database refreshes.
 */
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList,
std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound, std::atomic<bool>& stopImport, std::vector<std::thread>& backgroundThreads, bool& search) {

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    static std::vector<std::string> isoDirs;

    isoDirs.reserve(1000);

    auto refreshState = std::make_shared<RefreshState>();

    std::vector<std::string>& filteredFiles = refreshState->filteredFiles;
    std::vector<std::string>& pendingIndices = refreshState->pendingIndices;
    bool& isFiltered                         = refreshState->isFiltered;
    bool& hasPendingProcess                  = refreshState->hasPendingProcess;
    bool& umountMvRmBreak                    = refreshState->umountMvRmBreak;
    std::string& listSubtype                 = refreshState->listSubtype;
    size_t& currentPage                      = refreshState->currentPage;
    size_t& originalPage                     = refreshState->originalPage;

    filteredFiles.reserve(1000);
    isFiltered      = false;
    hasPendingProcess = false;
    umountMvRmBreak = false;
    currentPage     = 0;
    originalPage    = 0;

    bool needsClrScrn  = true;
    bool filterHistory = false;

    std::string operationColor = std::string(
        operation == "rm"        ? UI::Palette::Red    :
        operation == "cp"        ? UI::Palette::Green  :
        operation == "mv"        ? UI::Palette::Yellow :
        operation == "mount"     ? UI::Palette::Green  :
        operation == "write2usb" ? UI::Palette::Yellow :
        operation == "umount"    ? UI::Palette::Yellow : UI::Palette::RL_BoldAlt
    );

    bool isMount   = (operation == "mount");
    bool isUnmount = (operation == "umount");
    bool write     = (operation == "write2usb");
    bool isConversion = false;

    listSubtype = isMount ? "mount" : (write ? "write2usb" : "cp_mv_rm");

    while (true) {
		clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSelect();
        GlobalState::g_operationCancelled.store(false);
        filterHistory = false;
        clear_history();

        if (!isFiltered) originalPage = currentPage;

        if (!isUnmount) {
            removeNonExistentPathsFromDatabase(GlobalCaches::globalIsoFileList);
            isAtISOList.store(true);
        }

        if (needsClrScrn) {
            if (!isUnmount) {
                if (!loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, isImportRunning)) {
                    newISOFound.store(false);
                    break;
                }
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, isImportRunning))
                    break;
            }
            // Reset manual-update key for mountpoint-lists
			if (isUnmount) rl_bind_keyseq("R", rl_insert);
            std::cout << "\n\n";
            umountMvRmBreak = false;
        }

        // Atomic flag to ensure only ONE watcher thread is ever detached per import session
        static std::atomic<bool> watcherRunning{false};
		// Launch a detached thread for automatic list updating if startup auto-update or manual list refresh is running
		if (isImportRunning.load() && !isUnmount && !GlobalCaches::globalIsoFileList.empty()) {
			bool watcherExpected = false;
			if (watcherRunning.compare_exchange_strong(watcherExpected, true)) {
				std::thread([refreshState, &isAtISOList, &isImportRunning,
							 &updateHasRun, &newISOFound]() {
					refreshListAfterAutoUpdate(500, isAtISOList, isImportRunning,
											   updateHasRun, newISOFound, refreshState);
					watcherRunning.store(false);  // release when done
				}).detach();
			}
		}

		// Force redraw after manual imports to resolve stuck indicator
        if (refreshState->forceRedraw.load() && !isImportRunning.load() && !isUnmount) {
                            refreshState->forceRedraw.store(false);
                            needsClrScrn = true;
                            continue;
        }

        std::cout << "\033[1A\033[K";

		// Disable PgUp&PgDn when pagination is not enabled
		if (GlobalState::ITEMS_PER_PAGE == 0) {
			rl_bind_keyseq("\\e[5~", rl_insert);
			rl_bind_keyseq("\\e[6~", rl_insert);
		}

		const ReadlineAndPromptTheme pt = getPromptTheme();

		if (isFiltered) rl_bind_keyseq("*", rl_insert);

		std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

		std::string prompt =
			prefix +
			pt.iso         + "ISO" +
			pt.primary     + " ↵ for " + "\001" +
			operationColor + "\002" + operation +
			pt.primary     + ", ? help, < return: " +
			pt.reset;

        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);

        if (!rawInput) break;

        std::string inputString(rawInput.get());

        if (inputString[0] == ';' || (inputString[0] == '/' && inputString[1] == ';') || std::count(inputString.begin(), inputString.end(), '/') > 1 || inputString.find(";;") != std::string::npos) {
			needsClrScrn = false;
			continue;
		}
		if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        if (inputString[0] == 'R' && isImportRunning.load()) {
			std::cout << "\033[1B\033[K";
			needsClrScrn = false;
			continue;
		}

		// Initiate a manual list refresh
		if (inputString == "R" && !isUnmount && !GlobalCaches::globalIsoFileList.empty()) {
			bool expected = false;
			if (!isImportRunning.compare_exchange_strong(expected, true)) {
				// Already running — swallow the keypress silently
				needsClrScrn = false;
				continue;
			}
			needsClrScrn = true;
			search = false;
			backgroundThreads.emplace_back([&isImportRunning, &newISOFound, &stopImport, &refreshState] {
                auto makeDeadline = [] {
                    return std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                };
                // Wait for any previous watcher to finish first
                auto deadline = makeDeadline();
                while (watcherRunning.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Now wait for the new watcher to launch
                deadline = makeDeadline();
                while (!watcherRunning.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                backgroundDatabaseImport(isImportRunning, newISOFound, stopImport);
                refreshState->forceRedraw.store(true);
            });
			updateHasRun.store(true);
			continue;
		}

        if (inputString == "<") {
            if (isFiltered) {
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                needsClrScrn = true;
                continue;
            } else {
				reset_custom_keybindingsForSelect();
                currentPage = 0;
                return;
            }
        }

        if (inputString == "proc" && pendingIndices.empty()) {
			std::cout << "\033[1B\033[K";
			needsClrScrn = false;
            hasPendingProcess = false;
            continue;
        }

        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        const std::vector<std::string>& currentList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : GlobalCaches::globalIsoFileList);
        size_t totalPages = (GlobalState::ITEMS_PER_PAGE != 0) ? ((currentList.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE) : 0;
        bool need2Sort = false;

        bool validCommand = processPaginationHelpAndDisplay(inputString, totalPages, currentPage, isFiltered, needsClrScrn, isMount, isUnmount, write, isConversion, need2Sort, isAtISOList);

        if (validCommand) continue;

        bool pendingExecuted = handlePendingProcess(inputString, pendingIndices, hasPendingProcess, isMount, isUnmount, write, isFiltered,
                                                    filteredFiles, isoDirs,
                                                    needsClrScrn, operation, isAtISOList, umountMvRmBreak, filterHistory, newISOFound);
        if (pendingExecuted) {
            continue;
        }

        if (handleFilteringForISO(inputString, filteredFiles, isFiltered, needsClrScrn,
                                    filterHistory, operation, operationColor, isoDirs, isUnmount, currentPage)) {
										std::cout << "\033[1B\033[K";
										// Disable PgUp&PgDn when pagination is not enabled

                continue;
        }

        bool pendingHandled = handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn);
        if (pendingHandled) {
            continue;
        }

        processOperationForSelectedIsoFiles(inputString, isMount, isUnmount, write, isFiltered,
                                           filteredFiles, isoDirs,
                                           needsClrScrn, operation, isAtISOList, umountMvRmBreak,
                                           filterHistory, newISOFound);
    }

    reset_custom_keybindingsForSelect();
}

/**
 * @brief TUI controller for converting proprietary disk images to standard ISO format.
 *
 * Provides a paginated, filterable interface for selecting non-standard disk images
 * (BIN, MDF, NRG, CHD, DAA) and dispatching them to specific conversion backends.
 *
 * @details **Operational Logic:**
 * - **Dynamic Context:** Configures the UI theme and conversion tool (e.g., nrg2iso)
 *   dynamically based on the @p fileType parameter.
 * - **Cache Management:** On filter exit ('<'), it performs "Cache Restoration" by
 *   reloading data from @c GlobalCaches to ensure data integrity.
 * - **Input Sanitization:** Includes specific guards against malformed semicolon
 *   induction strings and empty inputs to prevent UI flickering or logic errors.
 * - **Batch Processing:** Supports staging multiple files via @c handlePendingInduction
 *   before committing the conversion batch with the "proc" command.
 *
 * @param fileType         The format category (e.g., "bin", "mdf", "nrg").
 * @param[in,out] files    The working list of files to display and process.
 * @param newISOFound      Atomic signal to notify the system when a new ISO is created.
 * @param list             UI flag indicating if the list view is active.
 * @param isImportRunning  Atomic gate to prevent concurrent database access.
 */
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list, std::atomic<bool>& isImportRunning) {

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;

    size_t currentPage = 0;
    size_t originalPage = currentPage;

    bool isFiltered = false;
    bool needsClrScrn = true;
    bool filterHistory = false;
    bool need2Sort = true;

    std::string fileExtension;
    std::string fileExtensionWithOutDots;
    std::string operation;
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        operation = "ccd2iso";
        fileExtensionWithOutDots = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileExtensionWithOutDots = "MDF";
        operation = "mdf2iso";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileExtensionWithOutDots = "NRG";
        operation = "nrg2iso";
    } else if (fileType == "chd") {
        fileExtension = ".chd";
        fileExtensionWithOutDots = "CHD";
        operation = "chd2iso";
    } else if (fileType == "daa") {
        fileExtension = ".daa/.gbi";
		fileExtensionWithOutDots = "DAA/GBI";
		operation = "daa2iso";
    } else {
        fileExtension = "";
        fileExtensionWithOutDots = "FILES";
    }

    while (true) {
		clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSelect();
        // Reset manual-update key for image lists
		rl_bind_keyseq("R", rl_insert);
        GlobalState::g_operationCancelled.store(false);
        bool verbose = false;

        if (!isFiltered) originalPage = currentPage;

        clear_history();
        if (needsClrScrn) {
			loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list, pendingIndices, hasPendingProcess, currentPage, isImportRunning);
			std::cout << "\n\n";
		}

		std::cout << "\033[1A\033[K";

		// Disable PgUp&PgDn when pagination is not enabled
		if (GlobalState::ITEMS_PER_PAGE == 0) {
			rl_bind_keyseq("\\e[5~", rl_insert);
			rl_bind_keyseq("\\e[6~", rl_insert);
		}

        const ReadlineAndPromptTheme pt = getPromptTheme();

        if (isFiltered) rl_bind_keyseq("*", rl_insert);

        std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

        std::string prompt =
            prefix +
            pt.highlight + fileExtensionWithOutDots +
            pt.primary   + " ↵ for " +
            pt.highlight + operation +
            pt.primary   + ", ? help, < return: " +
            pt.reset;

        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);

        if (!rawInput) break;

        std::string inputString(rawInput.get());

        if (inputString == "<") {
            clearScrollBuffer();
            if (isFiltered) {
                if (fileType == "bin" || fileType == "img") {
                    files = GlobalCaches::binImgFilesCache;
                } else if (fileType == "mdf") {
                    files = GlobalCaches::mdfMdsFilesCache;
                } else if (fileType == "nrg") {
                    files = GlobalCaches::nrgFilesCache;
                } else if (fileType == "chd") {
                    files = GlobalCaches::chdFilesCache;
                } else if (fileType == "daa") {
                    files = GlobalCaches::daaGbiFilesCache;
                }
                needsClrScrn = true;
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                need2Sort = false;
                continue;
            } else {
                currentPage = 0;
                need2Sort = false;
                break;
            }
        }

        if (inputString == "proc" && pendingIndices.empty()) {
			std::cout << "\033[1B\033[K";
            hasPendingProcess = false;
            needsClrScrn = false;
            continue;
        }

        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        if (inputString[0] == ';' || (inputString[0] == '/' && inputString[1] == ';') || std::count(inputString.begin(), inputString.end(), '/') > 1 || inputString.find(";;") != std::string::npos) {
			needsClrScrn = false;
			continue;
		}

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        std::atomic<bool> isAtISOList{false};

        size_t totalPages = (GlobalState::ITEMS_PER_PAGE != 0) ? ((files.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE) : 0;
        bool validCommand = processPaginationHelpAndDisplay(inputString, totalPages, currentPage, isFiltered, needsClrScrn, false, false, false, true, need2Sort, isAtISOList);

        if (validCommand) continue;

        if (inputString == "proc" && hasPendingProcess && !pendingIndices.empty()) {
            std::string combinedIndices = "";
            for (size_t i = 0; i < pendingIndices.size(); ++i) {
                combinedIndices += pendingIndices[i];
                if (i < pendingIndices.size() - 1) {
                    combinedIndices += " ";
                }
            }

            processInputForConversions(combinedIndices, files,
                                       (fileType == "mdf"),
                                       (fileType == "nrg"),
                                       (fileType == "chd"),
                                       (fileType == "daa"),
                                       verbose, needsClrScrn, newISOFound);

            needsClrScrn = true;
            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages, verboseSets.operationCompleted, verboseSets.operationSkipped, verboseSets.operationFailed, 3);
            }
            continue;
        }

        if (inputString == "/" || (!inputString.empty() && inputString[0] == '/')) {
            handleFilteringConvert2ISO(inputString, files, operation, isFiltered, needsClrScrn, filterHistory, need2Sort, currentPage);
            std::cout << "\033[1B\033[K";
            continue;
        }
        else if (inputString.find(';') != std::string::npos) {
            if (handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn)) {
                continue;
            }
        }
        else {
            processInputForConversions(inputString, files,
                                       (fileType == "mdf"),
                                       (fileType == "nrg"),
                                       (fileType == "chd"),
                                       (fileType == "daa"),
                                       verbose, needsClrScrn, newISOFound);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages, verboseSets.operationCompleted, verboseSets.operationSkipped, verboseSets.operationFailed, 3);
                needsClrScrn = true;
            }
        }
    }
   reset_custom_keybindingsForSelect();
}
