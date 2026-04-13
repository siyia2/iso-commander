// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"

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
void handleSelectIsoFilesResults(std::unordered_set<std::string>& uniqueErrorMessages, 
                                 std::unordered_set<std::string>& operationFiles, 
                                 std::unordered_set<std::string>& operationFails, 
                                 std::unordered_set<std::string>& skippedMessages, 
                                 const std::string& operation, bool& verbose, bool isMount, 
                                 bool& isFiltered, bool& umountMvRmBreak, bool isUnmount, 
                                 bool& needsClrScrn) {
    
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");
    
    if (!uniqueErrorMessages.empty() && operationFiles.empty() && 
         operationFails.empty() && skippedMessages.empty()) {
        
        clearScrollBuffer();
        needsClrScrn = true;
        
        std::cout << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "No valid input provided." 
                  << originalColors::boldAlt << "\n\n";

        std::cout << color << "↵ to continue..." << reset; 
        
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } 
    else if (verbose) {
        clearScrollBuffer();
        needsClrScrn = true;
        
        std::unordered_set<std::string> conditionalSet = isMount ? skippedMessages : std::unordered_set<std::string>{};
        verbosePrint(operationFiles, operationFails, conditionalSet, uniqueErrorMessages, isMount ? 2 : 1);
    }

    if ((operation == "mv" || operation == "rm" || operation == "umount") && isFiltered && umountMvRmBreak) {
        clear_history();
        needsClrScrn = true;
    }

    if (!isUnmount && globalIsoFileList.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        
        std::cout << "\n" << (isOrig ? originalColors::yellow : theme->warning) 
                  << "No ISO available for " << operation << "." 
                  << originalColors::boldAlt << "\n\n";
        
        std::cout << color << "↵ to continue..." << reset; 

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }
}

/**
 * @brief Routes the user input to the specific logic for mounting, writing, or file manipulation.
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
void processOperationForSelectedIsoFiles(const std::string& inputString, bool isMount, bool isUnmount, bool write, bool& isFiltered, const std::vector<std::string>& filteredFiles, std::vector<std::string>& isoDirs, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& skippedMessages, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool& filterHistory, std::atomic<bool>& newISOFound) {
    
    clearScrollBuffer();
    needsClrScrn = true;
    bool verbose = false;
    
    if (isMount || isUnmount) {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : 
                                                    (isUnmount ? isoDirs : globalIsoFileList);
        
        if (isUnmount) {
            umountMvRmBreak = true;
        }
        
        processInputForMountOrUmount(inputString, activeList, operationFiles, skippedMessages, 
                            operationFails, uniqueErrorMessages, umountMvRmBreak, verbose, isUnmount);
    } else if (write) {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
        writeToUsb(inputString, activeList, uniqueErrorMessages);
    } else {
        isAtISOList.store(false);
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
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
 * * @param timeoutS for polling set to 1s.
 * @param isAtISOList Flag indicating if list view is active.
 * @param isImportRunning Flag preventing refresh during active imports.
 * @param updateHasRun Atomic trigger for a refresh.
 */
void refreshListAfterAutoUpdate(int timeoutS, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateHasRun, bool& umountMvRmBreak, std::vector<std::string>& filteredFiles, bool& isFiltered, std::string& listSubtype, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, size_t& originalPage, std::atomic<bool>& newISOFound) {
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutS));

        if (!isImportRunning.load()) {

            if (isAtISOList.load() && !isFiltered) {
                loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, isImportRunning);
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
 * @brief The main menu loop for ISO operations (mount, umount, cp, mv, rm, write).
 * * Provides a paginated, filterable interface for selecting ISO files and executing 
 * system operations.
 */
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    std::unordered_set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
    std::vector<std::string> filteredFiles;
    static std::vector<std::string> isoDirs; 
    
    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;
    
    globalIsoFileList.reserve(100);
    filteredFiles.reserve(100);
    isoDirs.reserve(100);
    
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool umountMvRmBreak = false;
    bool filterHistory = false;
    size_t currentPage = 0;
    size_t originalPage = currentPage;

    std::string operationColor = std::string(
		operation == "rm"     ? originalColors::red    :
		operation == "cp"     ? originalColors::green  :
		operation == "mv"     ? originalColors::yellow :
		operation == "mount"  ? originalColors::green  :
		operation == "write"  ? originalColors::yellow :
		operation == "umount" ? originalColors::yellow : originalColors::rl_boldAlt
	);
                                 
    bool isMount = (operation == "mount");
    bool isUnmount = (operation == "umount");
    bool write = (operation == "write");
    bool isConversion = false;
    
    std::string listSubtype = isMount ? "mount" : (write ? "write" : "cp_mv_rm");
    
    while (true) {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        resetVerboseSets(operationFiles, skippedMessages, operationFails, uniqueErrorMessages);
        filterHistory = false;
        clear_history();
        
        if (!isFiltered) originalPage = currentPage;
        
        if (!isUnmount) {
            removeNonExistentPathsFromDatabase(globalIsoFileList);
            isAtISOList.store(true);
        }
        
        if (needsClrScrn) {
            if (!isUnmount) {
                if (!loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, isImportRunning))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, isImportRunning))
                    break;
            }
            
            std::cout << "\n\n";
            umountMvRmBreak = false;
        }
        if (updateHasRun.load() && !isUnmount && !globalIsoFileList.empty()) {
            std::thread(refreshListAfterAutoUpdate, 1, std::ref(isAtISOList), 
                        std::ref(isImportRunning), std::ref(updateHasRun), std::ref(umountMvRmBreak),
                        std::ref(filteredFiles), std::ref(isFiltered), std::ref(listSubtype), std::ref(pendingIndices), 
                        std::ref(hasPendingProcess), std::ref(currentPage), std::ref(originalPage), std::ref(newISOFound)).detach();
        }
        
        std::cout << "\033[1A\033[K";

        // Helper to wrap raw ANSI strings for readline
		auto wrap = [](std::string_view s) -> std::string {
			return "\001" + std::string(s) + "\002";
		};

		const ListTheme* theme = getActiveTheme();
		const bool isOriginal = (globalTheme == "original");

		// Wrap themed colors, keep originalColors::rl_ as-is
		std::string colorIso    = isOriginal ? std::string(originalColors::rl_green) : wrap(theme->accent);
		std::string colorMuted  = isOriginal ? std::string(originalColors::rl_blue)  : wrap(theme->muted);
		std::string colorFilter = isOriginal ? std::string(originalColors::rl_cyan)  : wrap(theme->accent);
		std::string colorReset  = isOriginal ? std::string(originalColors::rl_boldAlt) : wrap(originalColors::boldAlt);

		// operationColor usually comes from a raw theme member, so wrap it
		std::string safeOpColor = wrap(operationColor);

		// Build the prompt
		// Prefix calculation now uses the safely wrapped colorFilter
		std::string prefix = isFiltered ? (colorFilter + "F⊳ ") : "";

		std::string prompt = 
			prefix + 
			colorIso       + "ISO" + 
			colorMuted     + " ↵ for " + 
			safeOpColor    + operation + 
			colorMuted     + ", ? ↵ for help, < ↵ to return: " + 
			colorReset;

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        
        if (!input.get()) break;
            
        std::string inputString(input.get());
        
        if (inputString[0] == ';' || (inputString[0] == '/' && inputString[1] == ';') || std::count(inputString.begin(), inputString.end(), '/') > 1 || inputString.find(";;") != std::string::npos) {
			continue;
		}
		if (input.get()[0] == '\0') {
            needsClrScrn = false;
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
                currentPage = 0;
                return;
            }
        }
        
        if (inputString == "proc" && pendingIndices.empty()) {
            hasPendingProcess = false;
            continue;
        }
            
        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        const std::vector<std::string>& currentList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
        size_t totalPages = (ITEMS_PER_PAGE != 0) ? ((currentList.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE) : 0;
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
}

/**
 * @brief Interactive file selection and conversion controller for disk image formats.
 *
 * Provides a terminal-based interface for browsing, filtering, and selecting
 * BIN/IMG, MDF, NRG, CHD and DAA image files. Supports pagination, batch selection,
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
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        fileExtensionWithOutDots = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileExtensionWithOutDots = "MDF";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileExtensionWithOutDots = "NRG";
    } else if (fileType == "chd") {
        fileExtension = ".chd";
        fileExtensionWithOutDots = "CHD";
    } else if (fileType == "daa") {
        fileExtension = ".daa";
        fileExtensionWithOutDots = "DAA";
    } else {
        fileExtension = "";
        fileExtensionWithOutDots = "FILES";
    }
    
    while (true) {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        bool verbose = false; 
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        
        if (!isFiltered) originalPage = currentPage;
        
        clear_history();
        if (needsClrScrn) {
			loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list, pendingIndices, hasPendingProcess, currentPage, isImportRunning);
			std::cout << "\n\n";
		}
		
		std::cout << "\033[1A\033[K";
        
        auto wrap = [](std::string_view s) -> std::string {
            return "\001" + std::string(s) + "\002";
        };

        const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");

        std::string colorMuted     = isOriginal ? std::string(originalColors::rl_blue)   : wrap(theme->muted);
        std::string colorFilter    = isOriginal ? std::string(originalColors::rl_cyan)   : wrap(theme->accent);
        std::string colorHighlight = isOriginal ? std::string(originalColors::rl_orange) : wrap(theme->highlight);
        std::string colorReset     = isOriginal ? std::string(originalColors::rl_boldAlt)  : wrap(originalColors::boldAlt);

        std::string prefix = isFiltered ? (colorFilter + "F⊳ ") : "";

        std::string prompt = 
            prefix + 
            colorHighlight + fileExtensionWithOutDots + 
            colorMuted     + " ↵ for " + 
            colorHighlight + "convert2iso" + 
            colorMuted     + ", ? ↵ for help, < ↵ to return: " + 
            colorReset;
        
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        if (!rawInput) break;
        
        std::string inputString(rawInput.get());
        
        if (inputString == "<") {
            clearScrollBuffer();
            if (isFiltered) {
                if (fileType == "bin" || fileType == "img") {
                    files = binImgFilesCache;
                } else if (fileType == "mdf") {
                    files = mdfMdsFilesCache;
                } else if (fileType == "nrg") {
                    files = nrgFilesCache;
                } else if (fileType == "chd") {
                    files = chdFilesCache;
                } else if (fileType == "daa") {
                    files = daaFilesCache;
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
            hasPendingProcess = false;
            continue;
        }
        
        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        if (inputString[0] == ';' || (inputString[0] == '/' && inputString[1] == ';') || std::count(inputString.begin(), inputString.end(), '/') > 1 || inputString.find(";;") != std::string::npos) {
            continue;
        }

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue; 
        }

        std::atomic<bool> isAtISOList{false};
        
        size_t totalPages = (ITEMS_PER_PAGE != 0) ? ((files.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE) : 0;
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
            handleFilteringConvert2ISO(inputString, files, fileExtensionWithOutDots, isFiltered, needsClrScrn, filterHistory, need2Sort, currentPage);
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
}
