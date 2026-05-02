// SPDX-License-Identifier: GPL-3.0-or-later

// Project Headers
#include "../caches.h"
#include "../concurrency.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../select.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Routes the user input to the specific logic for mounting, unmounting, writing, or file manipulation.
 * * @param inputString Raw user input from readline.
 * @param isMount Operation is a mount.
 * @param isUnmount Operation is an unmount.
 * @param write Operation is a USB write.
 * @param isFiltered Current list state.
 * @param filteredFiles Vector of currently filtered file paths.
 * @param isoDirs Vector of mounted directories.
 * @param operationFiles Set for tracking successful files.
 * @param operationFails Set for tracking failed files.
 * @param uniqueErrorMessages Set for tracking error strings.
 * @param skippedMessages Set for tracking skipped files.
 * @param needsClrScrn Boolean to control screen refreshing.
 * @param operation The operation name string.
 * @param isAtISOList Atomic flag indicating if the UI is at the ISO list.
 * @param umountMvRmBreak Boolean to break list view on specific actions.
 * @param filterHistory Flag for filtering history state.
 * @param newISOFound Atomic flag for background refresh.
 */
void processOperationForSelectedIsoFiles(const std::string& inputString, bool isMount, bool isUnmount, bool write, bool& isFiltered, const std::vector<std::string>& filteredFiles, 
										 std::vector<std::string>& isoDirs, std::unordered_set<std::string>& operationFiles, 
										 std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, 
										 std::unordered_set<std::string>& skippedMessages, bool& needsClrScrn, 
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
        
        processInputForMountOrUmount(inputString, activeList, operationFiles, skippedMessages, 
                            operationFails, uniqueErrorMessages, umountMvRmBreak, verbose, isUnmount);
    } else if (write) {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalCaches::globalIsoFileList;
        writeToUsb(inputString, activeList, uniqueErrorMessages);
    } else {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalCaches::globalIsoFileList;
        processInputForCpMvRm(inputString, activeList, operation, operationFiles, operationFails, 
                             uniqueErrorMessages, umountMvRmBreak, filterHistory, verbose, newISOFound);
    }
    
    handleSelectIsoFilesResults(uniqueErrorMessages, operationFiles, operationFails, skippedMessages, 
                               operation, verbose, isMount, isFiltered, umountMvRmBreak, isUnmount, needsClrScrn);
}

/**
 * @brief Parses input for semicolon-delimited indices to be processed later.
 * * @param inputString Raw user input.
 * @param pendingIndices Vector to store indices for delayed processing.
 * @param hasPendingProcess Flag indicating if pending items exist.
 * @param needsClrScrn Flag to control screen refreshing.
 * @return true If indices were successfully added to the pending queue.
 * @return false If the input format was invalid for induction.
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
 * @brief Triggers the batch processing of indices stored in the pending queue.
 * * @param inputString The command string (expects "proc").
 * @param pendingIndices The queue of file indices.
 * @param hasPendingProcess State flag for pending operations.
 * @return true If the "proc" command was executed.
 * @return false Otherwise.
 */
bool handlePendingProcess(const std::string& inputString,std::vector<std::string>& pendingIndices,bool& hasPendingProcess,bool isMount,bool isUnmount,bool write,bool isFiltered, std::vector<std::string>& filteredFiles,std::vector<std::string>& isoDirs,std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages,std::unordered_set<std::string>& operationFails,std::unordered_set<std::string>& uniqueErrorMessages, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool& filterHistory, std::atomic<bool>& newISOFound) {
    
    if (hasPendingProcess && !pendingIndices.empty() && inputString == "proc") {
        std::string combinedIndices = "";
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            combinedIndices += pendingIndices[i];
            if (i < pendingIndices.size() - 1) {
                combinedIndices += " ";
            }
        }
        
        processOperationForSelectedIsoFiles(combinedIndices, isMount, isUnmount, write, isFiltered, 
                     filteredFiles, isoDirs, operationFiles, 
                     operationFails, uniqueErrorMessages, skippedMessages,
                     needsClrScrn, operation, isAtISOList, umountMvRmBreak, 
                     filterHistory, newISOFound);

        return true;
    }
    
    return false;
}

/**
 * @brief Background thread function to refresh the ISO list when data changes.
 *
 * @param timeoutS Polling interval in seconds (typically 500ms).
 * @param isAtISOList Flag indicating if the list view is currently active.
 * @param isImportRunning Flag preventing refresh during active imports.
 * @param updateHasRun Atomic trigger indicating a refresh is needed.
 * @param newISOFound Atomic flag cleared after the refresh completes.
 * @param state Shared ownership of the display state (filteredFiles, pagination,
 *              etc.), allowing the thread to safely outlive the caller.
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
            updateHasRun.store(false);
            newISOFound.store(false);
            break;
        }
    }
}

/**
 * @brief Interactive file selection for ISO operations (mount, umount, cp, mv, rm, write2usb).
 *
 * Provides a paginated, multi-layer filterable interface for selecting ISO files
 * (or mounted ISOs for umount) and executing system operations. Key behaviours:
 *
 * - Displays either the global ISO file list or the mounted-ISO list depending
 *   on the operation, with background auto-refresh when new ISOs are detected.
 *   Display state is owned via a shared_ptr (RefreshState), allowing a detached
 *   refresh thread to safely outlive the function without dangling references.
 * - Supports stacked filtering with filter history, allowing successive narrowing
 *   of the displayed list; '<' unwinds the filter state or returns to the caller.
 * - Implements a two-phase pending/deferred execution model: selections can be
 *   staged into a pending list and reviewed before the operation is committed.
 * - Pagination and help display are handled inline via processPaginationHelpAndDisplay.
 * - Loops until the user explicitly returns or EOF (Ctrl-D) is received.
 */
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, 
std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound, std::atomic<bool>& stopImport, std::vector<std::thread>& backgroundThreads, bool& search) {
	
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    std::unordered_set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
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
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSelect();
        GlobalState::g_operationCancelled.store(false);
        resetVerboseSets(operationFiles, skippedMessages, operationFails, uniqueErrorMessages);
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
			// Disable PgUp&PgDn when pagination is not enabled
			if (GlobalState::ITEMS_PER_PAGE == 0) {
				rl_bind_keyseq((char *)"\\e[5~", rl_get_previous_history);
				rl_bind_keyseq((char *)"\\e[6~", rl_get_next_history);
			}
			
            std::cout << "\n\n";
            umountMvRmBreak = false;
        }
        // Launch a detached thread for automatic list updating if startup auto-update is running
        if (updateHasRun.load() && !isUnmount && !GlobalCaches::globalIsoFileList.empty()) {
            std::thread(refreshListAfterAutoUpdate, 500,
                        std::ref(isAtISOList), std::ref(isImportRunning),
                        std::ref(updateHasRun), std::ref(newISOFound),
                        refreshState).detach();  // shared_ptr copied into thread, safe to detach
        }
        
        std::cout << "\033[1A\033[K";

		const ReadlineAndPromptTheme pt = getPromptTheme();

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
		
		if (GlobalState::ITEMS_PER_PAGE == 0 && (inputString == "PgUp" || inputString == "PgDn")) {
			std::cout << "\033[1B\033[K";
			needsClrScrn = false;
			continue;
		}
		
		// Initiate a manual list refresh
        if (inputString == "R" && !isImportRunning.load() && !isUnmount && !GlobalCaches::globalIsoFileList.empty()) {
			needsClrScrn =true;
			// Set to false to distinguish from regular auto-update
			search = false;
			isImportRunning.store(true);
			backgroundThreads.emplace_back([&isImportRunning, &newISOFound, &stopImport] { 
				backgroundDatabaseImport(isImportRunning, newISOFound, stopImport); 
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
                                                    filteredFiles, isoDirs, operationFiles, skippedMessages, operationFails, uniqueErrorMessages,
                                                    needsClrScrn, operation, isAtISOList, umountMvRmBreak, filterHistory, newISOFound);
        if (pendingExecuted) {
            continue;
        }

        if (handleFilteringForISO(inputString, filteredFiles, isFiltered, needsClrScrn, 
                                    filterHistory, operation, operationColor, isoDirs, isUnmount, currentPage)) {
										std::cout << "\033[1B\033[K";
                continue;
        }

        bool pendingHandled = handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn);
        if (pendingHandled) {
            continue;
        }

        processOperationForSelectedIsoFiles(inputString, isMount, isUnmount, write, isFiltered, 
                                           filteredFiles, isoDirs, operationFiles, 
                                           operationFails, uniqueErrorMessages, skippedMessages,
                                           needsClrScrn, operation, isAtISOList, umountMvRmBreak, 
                                           filterHistory, newISOFound);
    }
    
    reset_custom_keybindingsForSelect();
}

/**
 * @brief Interactive file selection and conversion controller for disk image formats.
 *
 * Provides a terminal-based interface for browsing, filtering, and selecting
 * BIN/IMG, MDF, NRG, CHD and DAA/GBI image files. Supports pagination, batch selection,
 * and command-based input for triggering conversions.
 *
 * Handles user interaction, cache restoration, pending selection processing,
 * and dispatches selected files to the appropriate conversion utilities.
 */
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list, std::atomic<bool>& isImportRunning) {

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    std::unordered_set<std::string> processedErrors, successOuts, skippedOuts, failedOuts;
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
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        setup_custom_keybindingsForSelect();
        // Reset manual-update key for image lists
		rl_bind_keyseq("R", rl_insert);
        GlobalState::g_operationCancelled.store(false);
        bool verbose = false; 
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        
        if (!isFiltered) originalPage = currentPage;
        
        clear_history();
        if (needsClrScrn) {
			loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list, pendingIndices, hasPendingProcess, currentPage, isImportRunning);
			std::cout << "\n\n";
		}
		
		std::cout << "\033[1A\033[K";

        const ReadlineAndPromptTheme pt = getPromptTheme();
        
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
        
        if (GlobalState::ITEMS_PER_PAGE == 0 && (inputString == "PgUp" || inputString == "PgDn")) {
			std::cout << "\033[1B\033[K";
			needsClrScrn = false;
			continue;
		}
        
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
                                       processedErrors, successOuts, skippedOuts, failedOuts, 
                                       verbose, needsClrScrn, newISOFound);
            
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3);
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
                                       processedErrors, successOuts, skippedOuts, failedOuts, 
                                       verbose, needsClrScrn, newISOFound);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3);
                needsClrScrn = true;
            }
        }
    }
   reset_custom_keybindingsForSelect();
}
