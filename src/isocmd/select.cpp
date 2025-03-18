// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// ISO SELECTION


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
    currentPage = 0;

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
        
        // Handle crashes when not enough permissions to access database
        try {
            if (!isUnmount) {
                removeNonExistentPathsFromDatabase();
                isAtISOList.store(true);
            }
        } catch (const std::exception& e) {
            std::cerr << "\n\033[1;91mUnable to access ISO database: " << e.what() << std::endl;
            // Handle the error gracefully, maybe set a flag or perform other necessary cleanup
            std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
        
        // Load files based on operation type
        if (needsClrScrn) {
            if (!isUnmount) {
                if (!clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess))
                    break;
            }
			
            std::cout << "\n\n";
            // Flag for initiating screen clearing on destructive list actions e.g. Umount/Mv/Rm
            umountMvRmBreak = false;
        }
        
        if (updateHasRun.load() && !isUnmount && !globalIsoFileList.empty()) {
            std::thread(refreshListAfterAutoUpdate, 1, std::ref(isAtISOList), 
                        std::ref(isImportRunning), std::ref(updateHasRun), std::ref(umountMvRmBreak),
                        std::ref(filteredFiles), std::ref(isFiltered), std::ref(listSubtype), std::ref(pendingIndices), std::ref(hasPendingProcess), std::ref(newISOFound)).detach();
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
                pendingIndices.clear();
                hasPendingProcess = false;
                isFiltered = false;
                needsClrScrn = true;
                currentPage = 0; // Reset page when clearing filter
                continue;
            } else {
                return; // Exit the function
            }
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
        
        bool validCommand = processPaginationHelpAndDisplay(inputString, totalPages, currentPage, needsClrScrn, isMount, isUnmount, write, isConversion, isAtISOList);

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
                                    filterHistory, pendingIndices, hasPendingProcess, 
                                    currentPage, operation, operationColor, isoDirs, isUnmount)) {
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
        processIsoOperations(inputString, activeList, operationFiles, skippedMessages, 
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
        processOperationInput(inputString, activeList, operation, operationFiles, operationFails, 
                             uniqueErrorMessages, umountMvRmBreak, filterHistory, verbose, newISOFound);
    }
    
    handleSelectIsoFilesResults(uniqueErrorMessages, operationFiles, operationFails, skippedMessages, 
                               operation, verbose, isMount, isFiltered, umountMvRmBreak, isUnmount, needsClrScrn);
}



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


// IMAGE SELECTION

std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here
std::vector<std::string> nrgFilesCache; // Memory cached nrgImgFiles here


// Function to check and list stored ram cache
void ramCacheList(std::vector<std::string>& files, bool& list, const std::string& fileExtension, const std::vector<std::string>& binImgFilesCache, const std::vector<std::string>& mdfMdsFilesCache, const std::vector<std::string>& nrgFilesCache, bool modeMdf, bool modeNrg) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    if (((binImgFilesCache.empty() && !modeMdf && !modeNrg) || 
         (mdfMdsFilesCache.empty() && modeMdf) || 
         (nrgFilesCache.empty() && modeNrg)) && list) {
        std::cout << "\n\033[1;93mNo " << fileExtension << " entries stored in RAM.\033[1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        files.clear();
        clearScrollBuffer();
        return;
    } else if (list) {
        if (!modeMdf && !modeNrg) {
            files = binImgFilesCache;
        } else if (modeMdf) {
            files = mdfMdsFilesCache;
        } else if (modeNrg) {
            files = nrgFilesCache;
        }
    }
}


// Function to clear Ram Cache and memory transformations for bin/img mdf nrg files
void clearRamCache(bool& modeMdf, bool& modeNrg) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    std::vector<std::string> extensions;
    std::string cacheType;
    bool cacheIsEmpty = false;

    if (!modeMdf && !modeNrg) {
        extensions = {".bin", ".img"};
        cacheType = "BIN/IMG";
        cacheIsEmpty = binImgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(binImgFilesCache);
    } else if (modeMdf) {
        extensions = {".mdf"};
        cacheType = "MDF";
        cacheIsEmpty = mdfMdsFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(mdfMdsFilesCache);
    } else if (modeNrg) {
        extensions = {".nrg"};
        cacheType = "NRG";
        cacheIsEmpty = nrgFilesCache.empty();
        if (!cacheIsEmpty) std::vector<std::string>().swap(nrgFilesCache);
    }

    // Manually remove items with matching extensions from transformationCache
    bool transformationCacheWasCleared = false;
    bool originalCacheWasCleared = false;
    
    for (auto it = transformationCache.begin(); it != transformationCache.end();) {
		const std::string& key = it->first;
		std::string keyLower = key; // Create a lowercase copy of the key
		toLowerInPlace(keyLower);

		bool shouldErase = std::any_of(extensions.begin(), extensions.end(),
			[&keyLower](std::string ext) { // Pass by value to modify locally
				toLowerInPlace(ext); // Convert extension to lowercase
				return keyLower.size() >= ext.size() &&
					keyLower.compare(keyLower.size() - ext.size(), ext.size(), ext) == 0;
			});

		if (shouldErase) {
			it = transformationCache.erase(it);
			transformationCacheWasCleared = true;
		} else {
			++it;
		}
	}

	// Manually remove items with matching extensions from original cache
	for (auto it = originalPathsCache.begin(); it != originalPathsCache.end();) {
		const std::string& key = it->first;
		std::string keyLower = key; // Create a lowercase copy of the key
		toLowerInPlace(keyLower);

		bool shouldErase = std::any_of(extensions.begin(), extensions.end(),
			[&keyLower](std::string ext) { // Pass by value to modify locally
				toLowerInPlace(ext); // Convert extension to lowercase
				return keyLower.size() >= ext.size() &&
					keyLower.compare(keyLower.size() - ext.size(), ext.size(), ext) == 0;
			});

		if (shouldErase) {
			it = originalPathsCache.erase(it);
			originalCacheWasCleared = true;
		} else {
			++it;
		}
	}


    // Display appropriate messages
    if (cacheIsEmpty && (!transformationCacheWasCleared || !originalCacheWasCleared)) {
        std::cout << "\n\033[1;93m" << cacheType << " buffer is empty. Nothing to clear.\033[0;1m\n";
    } else {
        std::cout << "\n\033[1;92m" << cacheType << " buffer cleared.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    clearScrollBuffer();
}


// Main function to select and convert image files based on type to ISO
void select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list) {

    // Bind keys for preventing clear screen and enabling tab completion
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    // Containers to track file processing results
    std::unordered_set<std::string> processedErrors, successOuts, skippedOuts, failedOuts;
    
    // New vector to store delayed execution indices
    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;
    
    // Reset page when entering this menu
    currentPage = 0;
    
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
        
        clear_history();
        if (needsClrScrn) clearAndLoadImageFiles(files, fileType, need2Sort, isFiltered, list, pendingIndices, hasPendingProcess);
        
        std::cout << "\n\n";
        std::cout << "\033[1A\033[K";
        
        // Build the user prompt string dynamically
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;38;5;208m\002" : "\001\033[1;38;5;208m\002")
                         + fileExtensionWithOutDots + "\001\033[1;94m\002 ↵ for \001\033[1;92m\002ISO\001\033[1;94m\002 conversion, ? ↵ for help, < ↵ to return:\001\033[0;1m\002 ";
        
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        if (!rawInput) break;
        
        std::string mainInputString(rawInput.get());
        
        // Check specifically for "<" to return/exit
        if (mainInputString == "<") {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore the original file list
                files = (fileType == "bin" || fileType == "img") ? binImgFilesCache :
                        (fileType == "mdf" ? mdfMdsFilesCache : nrgFilesCache);
                pendingIndices.clear();
                hasPendingProcess = false;
                needsClrScrn = true;
                isFiltered = false; // Reset filter status
                need2Sort = false;
                // Reset page when exiting filtered list
                currentPage = 0;
                continue;
            } else {
                need2Sort = false;
                break; // Exit the loop
            }
        }
        
        // Check for clear pending command
        if (mainInputString == "clr") {
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
        bool validCommand = processPaginationHelpAndDisplay(mainInputString, totalPages, currentPage, needsClrScrn, false, false, false, true, isAtISOList);
        
        if (validCommand) continue;
                
        // Handle input for blank Enter - just continue the loop
        if (rawInput.get()[0] == '\0') {
            std::cout << "\033[2A\033[K"; // Clear the line when entering blank enter
            needsClrScrn = false;
            continue; // Just continue the loop
        }
        
        // Check for "proc" command to execute pending operations
        if (mainInputString == "proc" && hasPendingProcess && !pendingIndices.empty()) {
            // Combine all pending indices into a single string as if they were entered normally
            std::string combinedIndices = "";
            for (size_t i = 0; i < pendingIndices.size(); ++i) {
                combinedIndices += pendingIndices[i];
                if (i < pendingIndices.size() - 1) {
                    combinedIndices += " ";
                }
            }
            
            // Process the pending operations
            processInput(combinedIndices, files, (fileType == "mdf"), (fileType == "nrg"), 
                        processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3);
            }
            continue;
        }       

        // Handle filter commands
        if (mainInputString == "/" || (!mainInputString.empty() && mainInputString[0] == '/')) {
            handleFilteringConvert2ISO(mainInputString, files, fileExtensionWithOutDots, pendingIndices, hasPendingProcess, isFiltered, needsClrScrn, filterHistory, need2Sort);
            continue;
        }// Check if input contains a semicolon for delayed execution
        else if (mainInputString.find(';') != std::string::npos) {
            if (handlePendingInduction(mainInputString, pendingIndices, hasPendingProcess, needsClrScrn)) {
                continue; // Continue the loop if new indices were processed
            }
        }
        // Process other input commands for file processing
        else {
            processInput(mainInputString, files, (fileType == "mdf"), (fileType == "nrg"), 
                         processedErrors, successOuts, skippedOuts, failedOuts, verbose, needsClrScrn, newISOFound);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(processedErrors, successOuts, skippedOuts, failedOuts, 3); // Print detailed logs if verbose mode is enabled
                needsClrScrn = true;
            }
        }
    }
}
