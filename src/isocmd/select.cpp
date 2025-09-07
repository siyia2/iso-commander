// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"


// ISO SELECTION


// Function to process results from selectIsoFiles
void handleSelectIsoFilesResults(std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& skippedMessages, const std::string& operation, bool& verbose, bool isMount, bool& isFiltered, bool& umountMvRmBreak, bool isUnmount, bool& needsClrScrn) {
    // Result handling and display
    if (!uniqueErrorMessages.empty() && operationFiles.empty() && operationFails.empty() && skippedMessages.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::cout << "\n\033[1;91mNo valid input provided.\033[0;1m\n\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } else if (verbose) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::unordered_set<std::string> conditionalSet = isMount ? skippedMessages : std::unordered_set<std::string>{};
        std::unordered_set<std::string> emptySet{};
        verbosePrint(operationFiles, operationFails, conditionalSet, uniqueErrorMessages, isMount ? 2 : 1);
    }

    // Reset filter for certain operations
    if ((operation == "mv" || operation == "rm" || operation == "umount") && isFiltered && umountMvRmBreak) {
        clear_history();
        needsClrScrn = true;
    }

    // For non-umount operations, if there are no ISOs in the global list, inform the user.
    if (!isUnmount && globalIsoFileList.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::cout << "\n\033[1;93mNo ISO available for " << operation << ".\033[0m\n\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }
}


// Function to process operations from selectIsoFiles
void processOperationForSelectedIsoFiles(const std::string& inputString, bool isMount, bool isUnmount, bool write, bool& isFiltered, const std::vector<std::string>& filteredFiles, std::vector<std::string>& isoDirs, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& skippedMessages, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool& filterHistory, std::atomic<bool>& newISOFound) {
    
    clearScrollBuffer();
    // Default flags
    needsClrScrn = true;
    bool verbose = false;
    
    if (isMount || isUnmount) {
        isAtISOList.store(false);
        
        // Determine which list to use
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : 
                                                    (isUnmount ? isoDirs : globalIsoFileList);
        
        // Set umountMvRmBreak for unmount operations
        if (isUnmount) {
            umountMvRmBreak = true;
        }
        
        // Use the merged function for both mount and unmount
        processInputForMountOrUmount(inputString, activeList, operationFiles, skippedMessages, 
                            operationFails, uniqueErrorMessages, umountMvRmBreak, verbose, isUnmount);
    } else if (write) {
        isAtISOList.store(false);
        // Use const reference instead of copying
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
        writeToUsb(inputString, activeList, uniqueErrorMessages);
    } else {
        isAtISOList.store(false);
        // Use const reference instead of copying
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
        processInputForCpMvRm(inputString, activeList, operation, operationFiles, operationFails, 
                             uniqueErrorMessages, umountMvRmBreak, filterHistory, verbose, newISOFound);
    }
    
    handleSelectIsoFilesResults(uniqueErrorMessages, operationFiles, operationFails, skippedMessages, 
                               operation, verbose, isMount, isFiltered, umountMvRmBreak, isUnmount, needsClrScrn);
}


// Parse pending indices and remove duplicates
bool handlePendingInduction(const std::string& inputString, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& needsClrScrn) {
    // Check if the input contains a semicolon and does not contain a slash
    if (inputString.find(';') == std::string::npos || inputString.find('/') != std::string::npos) {
        return false;
    }

    // Strip the semicolon from the input
    std::string indicesInput = inputString.substr(0, inputString.find(';'));
    
    // Trim whitespace from the end
    while (!indicesInput.empty() && std::isspace(indicesInput.back())) {
        indicesInput.pop_back();
    }
    
    if (!indicesInput.empty()) {
        // Parse and store the indices
        std::istringstream iss(indicesInput);
        std::string token;
        
        // Use a set to track duplicates
        std::unordered_set<std::string> uniqueTokens;
        
        // Add existing pending indices to the set to ensure no duplicates
        for (const auto& index : pendingIndices) {
            uniqueTokens.insert(index);
        }
        
        // Temporarily store new indices to preserve order
        std::vector<std::string> newIndices;
        
        // Parse new indices and add them to the vector if they are not duplicates
        while (iss >> token) {
            if (uniqueTokens.find(token) == uniqueTokens.end()) { // Check if the token is not already in the set
                newIndices.push_back(token); // Add to the newIndices vector
                uniqueTokens.insert(token); // Add to the set to track duplicates
            }
        }
        
        // Append new indices to pendingIndices
        pendingIndices.insert(pendingIndices.end(), newIndices.begin(), newIndices.end());
        
        if (!pendingIndices.empty()) {
            hasPendingProcess = true;
            needsClrScrn = true;
            return true;
        }
    }
    return false;
}


// Function to handle Pending Execution
bool handlePendingProcess(const std::string& inputString,std::vector<std::string>& pendingIndices,bool& hasPendingProcess,bool isMount,bool isUnmount,bool write,bool isFiltered, std::vector<std::string>& filteredFiles,std::vector<std::string>& isoDirs,std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages,std::unordered_set<std::string>& operationFails,std::unordered_set<std::string>& uniqueErrorMessages, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool& filterHistory, std::atomic<bool>& newISOFound) {
    
    if (hasPendingProcess && !pendingIndices.empty() && inputString == "proc") {
        // Combine all pending indices into a single string as if they were entered normally
        std::string combinedIndices = "";
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            combinedIndices += pendingIndices[i];
            if (i < pendingIndices.size() - 1) {
                combinedIndices += " ";
            }
        }
        
        // Process the pending operations
        processOperationForSelectedIsoFiles(combinedIndices, isMount, isUnmount, write, isFiltered, 
                 filteredFiles, isoDirs, operationFiles, 
                 operationFails, uniqueErrorMessages, skippedMessages,
                 needsClrScrn, operation, isAtISOList, umountMvRmBreak, 
                 filterHistory, newISOFound);

        return true;
    }
    
    return false;
}


// Function to automatically update ISO list if auto-update is on
void refreshListAfterAutoUpdate(int timeoutSeconds, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateHasRun, bool& umountMvRmBreak, std::vector<std::string>& filteredFiles, bool& isFiltered, std::string& listSubtype, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& newISOFound) {
    // Continuously checks for conditions at intervals specified by timeoutSeconds
    
    while (true) {
        // Sleep for the given timeout (2s) before checking the conditions
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));

        // Only proceed if the import process is not running
        if (!isImportRunning.load()) {

            // Check if the list is a non-filtered ISO list
            if (isAtISOList.load() && !isFiltered) {
                // If conditions are met, clear and reload the ISO list with the updated data
                clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, isImportRunning);
                std::cout << "\n";
                
                rl_on_new_line(); // necessary to avoid the graphical glitch when transitioning from filtered -> non-filtered list
                rl_redisplay();    // Refresh the readline interface to display updated content
            }

            // Reset flags indicating update status and new ISO discovery
            updateHasRun.store(false);  // Reset update status flag
            newISOFound.store(false);   // Reset the flag for newly found ISO
            
            break;
        }
    }
}


// Main function to select and operate on ISOs by number for umount mount cp mv and rm
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    // Bind readline keys
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    std::unordered_set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
    std::vector<std::string> filteredFiles;
    
    // Static vector renamed to isoDirs: used exclusively for umount
    static std::vector<std::string> isoDirs; 
    
    // Vector to store delayed execution indices
    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;
    
    globalIsoFileList.reserve(100);
    filteredFiles.reserve(100);
    isoDirs.reserve(100);
    
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool umountMvRmBreak = false;
    bool filterHistory = false;

    // Reset page when entering this menu
    size_t currentPage = 0;
    // Variable to store current page
    size_t originalPage = currentPage;

    // Determine operation color and specific flags
    std::string operationColor = operation == "rm" ? "\033[1;91m" :
                                 operation == "cp" ? "\033[1;92m" : 
                                 operation == "mv" ? "\033[1;93m" :
                                 operation == "mount" ? "\033[1;92m" : 
                                 operation == "write" ? "\033[1;93m" :
                                 operation == "umount" ? "\033[1;93m" : "\033[1;95m";
                                 
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
        
        // Store currentPage for unfiltered list
        if (!isFiltered) originalPage = currentPage;
        
        // Handle crashes when not enough permissions to access database
        if (!isUnmount) {
			removeNonExistentPathsFromDatabase();
            isAtISOList.store(true);
        }
        
        // Load files based on operation type
        if (needsClrScrn) {
            if (!isUnmount) {
                if (!clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, isImportRunning))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, isImportRunning))
                    break;
            }
			
            std::cout << "\n\n";
            // Flag for initiating screen clearing on destructive list actions e.g. Umount/Mv/Rm
            umountMvRmBreak = false;
        }
        if (updateHasRun.load() && !isUnmount && !globalIsoFileList.empty()) {
            std::thread(refreshListAfterAutoUpdate, 2, std::ref(isAtISOList), 
                        std::ref(isImportRunning), std::ref(updateHasRun), std::ref(umountMvRmBreak),
                        std::ref(filteredFiles), std::ref(isFiltered), std::ref(listSubtype), std::ref(pendingIndices), 
                        std::ref(hasPendingProcess), std::ref(currentPage), std::ref(newISOFound)).detach();
        }
           
        std::cout << "\033[1A\033[K";
        
        // Generate prompt - updated to remove "↵" after "<"
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001" : "\001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001")
                           + operationColor + "\002" + operation 
                           + "\001\033[1;94m\002, ? ↵ for help, < ↵ to return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        
        if (!input.get()) break;
            
        std::string inputString(input.get());
        
        // Check specifically for "<" to return/exit
        if (inputString == "<") {
            if (isFiltered) {
                isFiltered = false;
                // Clear the filtering stack when returning to unfiltered mode
                filteringStack.clear();
                currentPage = originalPage;
                needsClrScrn = true;
                continue;
            } else {
				currentPage = 0;
                return; // Exit the function
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
        
        // check if first char is ; and skip
        if (input && input[0] == ';') {
			needsClrScrn = false;
            continue;
        }

        const std::vector<std::string>& currentList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
        
        size_t totalPages = (ITEMS_PER_PAGE != 0) ? ((currentList.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE) : 0;
		
		// Dummy variable never used from here
		bool need2Sort = false;
        
        bool validCommand = processPaginationHelpAndDisplay(inputString, totalPages, currentPage, isFiltered, needsClrScrn, isMount, isUnmount, write, isConversion, need2Sort, isAtISOList);

        if (validCommand) continue;
        
        // Handle empty input - just continue the loop
        if (inputString.empty()) {
            needsClrScrn = false; // Optionally keep the current screen
            continue; // Just continue the loop
        }

        // Handle pending execution
        bool pendingExecuted = handlePendingProcess(inputString, pendingIndices, hasPendingProcess, isMount, isUnmount, write, isFiltered, 
                                                    filteredFiles, isoDirs, operationFiles, skippedMessages, operationFails, uniqueErrorMessages,
                                                    needsClrScrn, operation, isAtISOList, umountMvRmBreak, filterHistory, newISOFound);
        if (pendingExecuted) {
            continue;
        }

        // Handle filtering operations
        if (handleFilteringForISO(inputString, filteredFiles, isFiltered, needsClrScrn, 
                                    filterHistory, operation, operationColor, isoDirs, isUnmount, currentPage)) {
                continue;
        }

        // Handle pending induction (delayed execution with ;)
        bool pendingHandled = handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn);
        if (pendingHandled) {
            continue;
        }

        // Process operation for selected ISO files if no special handling needed
        processOperationForSelectedIsoFiles(inputString, isMount, isUnmount, write, isFiltered, 
                                           filteredFiles, isoDirs, operationFiles, 
                                           operationFails, uniqueErrorMessages, skippedMessages,
                                           needsClrScrn, operation, isAtISOList, umountMvRmBreak, 
                                           filterHistory, newISOFound);
    }
}


// IMAGE SELECTION

std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here


// Main function to select and convert image files based on type to ISO
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list, std::atomic<bool>& isImportRunning) {

    // Bind keys for preventing clear screen and enabling tab completion
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    // Containers to track file processing results
    std::unordered_set<std::string> processedErrors, successOuts, skippedOuts, failedOuts;
    
    // New vector to store delayed execution indices
    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;
    
    // Reset page when entering this menu
    size_t currentPage = 0;
    // Variable to store current page
    size_t originalPage = currentPage;
    
    bool isFiltered = false; // Indicates if the file list is currently filtered
    bool needsClrScrn = true;
    bool filterHistory = false;
    bool need2Sort = true;

    std::string fileExtension = (fileType == "bin" || fileType == "img") ? ".bin/.img" 
                               : (fileType == "mdf") ? ".mdf" : ".nrg"; // Determine file extension based on type
    
    std::string fileExtensionWithOutDots;
    for (char c : fileExtension) {
        if (c != '.') {
            fileExtensionWithOutDots += toupper(c);  // Capitalize the character and add it to the result
        }
    }
    
    // Main processing loop
    while (true) {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        bool verbose = false; // Reset verbose mode
        resetVerboseSets(processedErrors, successOuts, skippedOuts, failedOuts);
        
        // Store currentPage for unfiltered list
        if (!isFiltered) originalPage = currentPage;
        
        clear_history();
        if (needsClrScrn) clearAndLoadImageFiles(files, fileType, need2Sort, isFiltered, list, pendingIndices, hasPendingProcess, currentPage, isImportRunning);
        
        std::cout << "\n\n";
        std::cout << "\033[1A\033[K";
        
        // Build the user prompt string dynamically
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;38;5;208m\002" : "\001\033[1;38;5;208m\002")
                         + fileExtensionWithOutDots + "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion, ? ↵ for help, < ↵ to return:\001\033[0;1m\002 ";
        
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        if (!rawInput) break;
        
        std::string inputString(rawInput.get());
        
        // Check specifically for "<" to return/exit
        if (inputString == "<") {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore the original file list
                files = (fileType == "bin" || fileType == "img") ? binImgFilesCache :
                        (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);
                needsClrScrn = true;
                isFiltered = false; // Reset filter status
                // Clear the filtering stack when returning to unfiltered mode
                filteringStack.clear();
                currentPage = originalPage;
                need2Sort = false;
                continue;
            } else {
				currentPage = 0;
                need2Sort = false;
                break; // Exit the loop
            }
        }
        
        if (inputString == "proc" && pendingIndices.empty()) {
			hasPendingProcess = false;
			continue;
		}
        
        // Check for clear pending command
        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }
        
        // check if first char is ; and skip
        if (rawInput && rawInput.get()[0] == ';') {
             std::cout << "\033[2A\033[K"; // Clear the line when entering ;
             needsClrScrn = false;
			 continue;
        }
        
        std::atomic<bool> isAtISOList{false};
        
        size_t totalPages = (ITEMS_PER_PAGE != 0) ? ((files.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE) : 0;
        bool validCommand = processPaginationHelpAndDisplay(inputString, totalPages, currentPage, isFiltered, needsClrScrn, false, false, false, true, need2Sort, isAtISOList);
        
        if (validCommand) continue;
                
        // Handle input for blank Enter - just continue the loop
        if (rawInput.get()[0] == '\0') {
            std::cout << "\033[2A\033[K"; // Clear the line when entering blank enter
            needsClrScrn = false;
            continue; // Just continue the loop
        }
        
        // Check for "proc" command to execute pending operations
        if (inputString == "proc" && hasPendingProcess && !pendingIndices.empty()) {
            // Combine all pending indices into a single string as if they were entered normally
            std::string combinedIndices = "";
            for (size_t i = 0; i < pendingIndices.size(); ++i) {
                combinedIndices += pendingIndices[i];
                if (i < pendingIndices.size() - 1) {
                    combinedIndices += " ";
                }
            }
            
            // Process the pending operations
            processInputForConversions(combinedIndices, files, (fileType == "mdf"), (fileType == "nrg"), 
                        processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3);
            }
            continue;
        }       

        // Handle filter commands
        if (inputString == "/" || (!inputString.empty() && inputString[0] == '/')) {
            handleFilteringConvert2ISO(inputString, files, fileExtensionWithOutDots, isFiltered, needsClrScrn, filterHistory, need2Sort, currentPage);
            continue;
        }// Check if input contains a semicolon for delayed execution
        else if (inputString.find(';') != std::string::npos) {
            if (handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn)) {
                continue; // Continue the loop if new indices were processed
            }
        }
        // Process other input commands for file processing
        else {
            processInputForConversions(inputString, files, (fileType == "mdf"), (fileType == "nrg"), 
                         processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3); // Print detailed logs if verbose mode is enabled
                needsClrScrn = true;
            }
        }
    }
}
