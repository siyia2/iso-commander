// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Function to group files for CpMvRm, identical filenames are grouped in the same chunk and processed by the same thread
std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete) {
    // Convert unordered_set to vector
    std::vector<int> processedIndicesVector(processedIndices.begin(), processedIndices.end());

    std::vector<std::vector<int>> indexChunks;

    if (!isDelete) {
        // Group indices by their base filename
        std::unordered_map<std::string, std::vector<int>> groups;
        for (int idx : processedIndicesVector) {
            std::string baseName = std::filesystem::path(isoFiles[idx - 1]).filename().string();
            groups[baseName].push_back(idx);
        }

        std::vector<int> uniqueNameFiles;
        // Separate multi-file groups and collect unique files
        for (auto& kv : groups) {
            if (kv.second.size() > 1) {
                indexChunks.push_back(kv.second);
            } else {
                uniqueNameFiles.push_back(kv.second[0]);
            }
        }

        // Calculate max files per chunk based on numThreads
        size_t maxFilesPerChunk = std::max(1UL, numThreads > 0 ? (uniqueNameFiles.size() + numThreads - 1) / numThreads : 5);

        // Split unique files into chunks
        for (size_t i = 0; i < uniqueNameFiles.size(); i += maxFilesPerChunk) {
            auto end = std::min(i + maxFilesPerChunk, uniqueNameFiles.size());
            std::vector<int> chunk(
                uniqueNameFiles.begin() + i,
                uniqueNameFiles.begin() + end
            );
            indexChunks.emplace_back(chunk);
        }
    } else {
        // For "rm", group indices into chunks based on numThreads
        size_t maxFilesPerChunk = std::max(1UL, numThreads > 0 ? (processedIndicesVector.size() + numThreads - 1) / numThreads : 10);

        for (size_t i = 0; i < processedIndicesVector.size(); i += maxFilesPerChunk) {
            std::vector<int> chunk;
            auto end = std::min(i + maxFilesPerChunk, processedIndicesVector.size());
            for (size_t j = i; j < end; ++j) {
                chunk.push_back(processedIndicesVector[j]);
            }
            indexChunks.push_back(chunk);
        }
    }

    return indexChunks;
}


// Function to process selected indices for cpMvDel accordingly
void processOperationInput(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound) {
    setupSignalHandlerCancellations();
    
    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");
    
    std::string coloredProcess = 
    isDelete ? "\033[1;91m" + process + " \033[0;1moperation" :
    isMove   ? "\033[1;93m" + process + " \033[0;1moperation" :
    isCopy   ? "\033[1;92m" + process + " \033[0;1moperation" :
    process;
    
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor       = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    // Parse the input to fill processedIndices.
    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }
    
	unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
	std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

	// Flag for Rm abortion
    bool abortDel = false;
    
    std::string processedUserDestDir = userDestDirRm(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
                                                     operationColor, operationDescription, umountMvRmBreak, 
                                                     filterHistory, isDelete, isCopy, abortDel, overwriteExisting);
        
    g_operationCancelled.store(false);
    
    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
        uniqueErrorMessages.clear();
        return;
    }
    uniqueErrorMessages.clear();
    clearScrollBuffer();

    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(isoFiles[index - 1]);
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    size_t totalBytes = getTotalFileSize(filesToProcess);
    size_t totalTasks = filesToProcess.size();
                 
    // Adjust totals for copy/move operations with multiple destinations
    if (isCopy || isMove) {
		size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;
    }
    
    std::cout << "\n\033[0;1m Processing " << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process <<
             " \033[0;1moperation\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";
    
    std::atomic<bool> isProcessingComplete(false);

    // Start progress tracking in a separate thread.
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
                                 totalBytes, &completedTasks, &failedTasks, 
                                 totalTasks, &isProcessingComplete, &verbose, std::string(coloredProcess));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    // For each chunk, create a vector of file names and enqueue the operation.
    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(isoFilesInChunk),
            [&isoFiles](size_t index) { return isoFiles[index - 1]; });

        futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), 
                                             &isoFiles, &operationIsos, &operationErrors, &userDestDir, 
                                             isMove, isCopy, isDelete, &completedBytes, &completedTasks, 
                                             &failedTasks, &overwriteExisting]() {
            handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, 
                                   userDestDir, isMove, isCopy, isDelete, 
                                   &completedBytes, &completedTasks, &failedTasks, overwriteExisting);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    progressThread.join();
    

    if (!isDelete) {
		bool promptFlag = false;
        int maxDepth = 0;
        manualRefreshForDatabase(userDestDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }
    
    if (!isDelete && !operationIsos.empty()) {
        saveHistory(filterHistory);
        clear_history();
    }

    clear_history();
}


// Function that handles all pagination logic for a list of entries
std::string handlePaginatedDisplay(const std::vector<std::string>& entries, const std::string& promptPrefix, const std::string& promptSuffix, const std::function<void()>& displayErrorsFn, const std::function<void()>& setupEnvironmentFn, bool& isPageTurn) {
    int totalEntries = entries.size();
    
    // Setup pagination parameters
    int entriesPerPage;
    if (totalEntries <= 25) {
        entriesPerPage = totalEntries;  // Single page
    } else {
        entriesPerPage = std::max(25, (totalEntries + 4) / 5);
        // Cap entriesPerPage at 100 if it exceeds, allowing more pages
        if (entriesPerPage > 100) {
            entriesPerPage = 100;
        }
    }
    
    int totalPages = (totalEntries + entriesPerPage - 1) / entriesPerPage;
    int currentPage = 0;
    
    while (true) {
        // Setup environment if function is provided
        if (setupEnvironmentFn) {
            setupEnvironmentFn();
        }
        
        // Clear the screen before displaying the new page
        clearScrollBuffer(); 
        
        // Only display errors on non-page-turn iterations
        if (!isPageTurn && displayErrorsFn) {
            displayErrorsFn();
        }
        
        // Create content for current page
        int start = currentPage * entriesPerPage;
        int end = std::min(start + entriesPerPage, totalEntries);
        
        std::ostringstream pageContent;
        for (int i = start; i < end; ++i) {
            pageContent << entries[i];
        }
        
        if (totalPages > 1) {
            pageContent << "\n\033[1mPage " << (currentPage + 1) 
                      << "/" << totalPages << " \033[1;94m(n/p) or g<num> ↵\n\033[0m";
        }
        
        // Build the full prompt
        std::string prompt = promptPrefix + pageContent.str() + promptSuffix;
        
        // Get user input
        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        if (!input.get()) {
            return "";  // Handle EOF or error
        }
        
        // Process input
        std::string userInput = trimWhitespace(input.get());
        
        // Handle page navigation
        bool isNavigation = false;
        if (!userInput.empty()) {
            // Check if this is a "go to page" command (e.g., "g4")
            if (userInput.size() >= 2 && userInput[0] == 'g' && std::isdigit(userInput[1])) {
                std::string pageNumStr = userInput.substr(1);
                try {
                    int requestedPage = std::stoi(pageNumStr);
                    if (requestedPage >= 1 && requestedPage <= totalPages) {
                        currentPage = requestedPage - 1;  // Convert to 0-based index
                        isPageTurn = true;
                        isNavigation = true;
                        continue;
                    }
                } catch (const std::exception&) {
                    // Invalid page number format, treat as regular input
                }
            }
            
            // Check for n/p navigation - mark as navigation even if page doesn't change
            if (userInput == "n" || userInput == "N") {
                isNavigation = true;  // Mark as navigation regardless of page change
                
                // Next page, but only if not at the last page
                if (currentPage < totalPages - 1) {
                    currentPage++;
                    isPageTurn = true;
                    continue;
                } else {
                    // At last page, still mark as page turn to avoid triggering error display
                    isPageTurn = true;
                    continue;
                }
            } else if (userInput == "p" || userInput == "P") {
                isNavigation = true;  // Mark as navigation regardless of page change
                
                // Previous page, but only if not at the first page
                if (currentPage > 0) {
                    currentPage--;
                    isPageTurn = true;
                    continue;
                } else {
                    // At first page, still mark as page turn to avoid triggering error display
                    isPageTurn = true;
                    continue;
                }
            }
        }
        
        if (!isNavigation) {
            isPageTurn = false;  // Reset flag for non-page-turn actions
            return userInput;     // Return the final non-navigation input
        }
    }
}


// Function to generate entries for selected ISO files
std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
    std::vector<std::string> entries;

    // Generate entries for selected ISO files
    for (const auto& chunk : indexChunks) {
        for (int index : chunk) {
            auto [shortDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");
            std::ostringstream oss;
            oss << "\033[1m-> " << shortDir << "/\033[95m" << filename << "\033[0m\n";
            entries.push_back(oss.str());
        }
    }

    return entries;
}


// Function to handle rm including pagination
bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, const std::function<void()>& displayErrorsFn, bool& umountMvRmBreak, bool& abortDel) {
    
    bool isPageTurn = false;
    
    // Setup environment function
    auto setupEnv = [&]() {
        rl_bind_key('\f', clear_screen_and_buffer);
    };
    
    // Generate entries for selected ISO files
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);

    // Sort the entries using the natural comparison
    sortFilesCaseInsensitive(entries);
    
    // Prefix and suffix for the prompt
    std::string promptPrefix = "\n";
    std::string promptSuffix = "\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be " +
        std::string("\001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (y/n):\001\033[0;1m\002 ");
    
    // Use the consolidated pagination function with custom input handling
    while (true) {
        // Use the consolidated pagination function
        std::string userInput = handlePaginatedDisplay(
            entries,
            promptPrefix,
            promptSuffix,
            displayErrorsFn,
            setupEnv,
            isPageTurn
        );
        
        rl_bind_key('\f', prevent_readline_keybindings);
        
        // Handle EOF
        if (userInput.empty() && !isPageTurn) {
			umountMvRmBreak = false;
            abortDel = true;
            std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return false;
        }
        
        // Process yes/no (only if not page turning)
        if (!isPageTurn) {
            if (userInput == "y" || userInput == "Y") {
                umountMvRmBreak = true;
                return true;
            } else {
                umountMvRmBreak = false;
                abortDel = true;
                std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return false;
            }
        }
    }
}


// Function to prompt for userDestDir or Delete confirmation including pagination
std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting) {

    // Display error messages if any
    auto displayErrors = [&]() {
        if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
            for (const auto& err : uniqueErrorMessages) {
                std::cout << err << "\n";
            }
        }
    };
    
    // Generate entries for selected ISO files - used by both branches
	std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
	
	 // Sort the entries using the natural comparison
    sortFilesCaseInsensitive(entries);
    
    // Clear screen initially
    clearScrollBuffer();
    displayErrors(); // Show errors on first display
    
    if (!isDelete) {
        // Copy/Move operation flow
        bool isPageTurn = false;
        
        // Setup environment function
        auto setupEnv = [&]() {
            enable_ctrl_d();
            setupSignalHandlerCancellations();
            g_operationCancelled.store(false);
            rl_bind_key('\f', clear_screen_and_buffer);
            rl_bind_key('\t', rl_complete);
            
            if (!isCopy) {
                umountMvRmBreak = true;
            }
            
            if (!isPageTurn) {
                clear_history();
                filterHistory = false;
                loadHistory(filterHistory);
            }
        };
        
        // Prefix and suffix for the prompt
        std::string promptPrefix = "\n";
        std::string promptSuffix = "\n\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " + 
            operationColor + operationDescription + 
            "\001\033[1;94m\002 into, ? ↵ for help, ↵ to return:\n\001\033[0;1m\002";
        
        // Use the consolidated pagination function
        std::string userInput = handlePaginatedDisplay(
            entries, 
            promptPrefix, 
            promptSuffix, 
            displayErrors,
            setupEnv, 
            isPageTurn
        );
        
        // After pagination, handle the input
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
        
        // Handle help command
        if (userInput == "?") {
            bool import2ISO = false;
            bool isCpMv = true;
            helpSearches(isCpMv, import2ISO);
            userDestDir = "";
            return userDestDir;
        }
        
        // Handle empty input (return)
        if (userInput.empty()) {
            umountMvRmBreak = false;
            userDestDir = "";
            clear_history();
            return userDestDir;
        }
        
        // Process destination directory including possible -o flag
        userDestDir = userInput;
        
        // Check for overwrite flag
        if (userDestDir.size() >= 3 && userDestDir.substr(userDestDir.size() - 3) == " -o") {
            overwriteExisting = true;
            userDestDir = userDestDir.substr(0, userDestDir.size() - 3);
        } else {
            overwriteExisting = false;
        }
        
        // Add to history without the overwrite flag
        std::string historyInput = userInput;
        if (historyInput.size() >= 3 && historyInput.substr(historyInput.size() - 3) == " -o") {
            historyInput = historyInput.substr(0, historyInput.size() - 3);
        }
        add_history(historyInput.c_str());
    } else {
        // Delete operation flow - call the extracted function
        bool proceedWithDelete = handleDeleteOperation(isoFiles, indexChunks, displayErrors, umountMvRmBreak, abortDel);
        
        if (!proceedWithDelete) {
            userDestDir = "";
            return userDestDir;
        }
    }
    
    return userDestDir;
}


// Function to buffer file copying
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
    const size_t bufferSize = 8 * 1024 * 1024; // 8MB buffer
    std::vector<char> buffer(bufferSize);
    if (g_operationCancelled.load()) return false;
    
    std::ifstream input(src, std::ios::binary);
    if (!input) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    
    std::ofstream output(dst, std::ios::binary);
    if (!output) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    
    while (!g_operationCancelled.load()) { // Check cancellation flag at each iteration
        input.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = input.gcount();
        
        if (bytesRead == 0) {
            break;
        }
        
        output.write(buffer.data(), bytesRead);
        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        
        completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
    }

    // Check if the operation was cancelled
    if (g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        output.close(); // Close the output stream before attempting to delete
        fs::remove(dst, ec); // Delete the partial file, ignore errors here
        return false;
    }
    
    return true;
}


// Function to perform Rm
void performDeleteOperation(const fs::path& srcPath,const std::string& srcDir, const std::string& srcFile, size_t fileSize,std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors ,std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {
    std::error_code ec;
    // Set errorDetail based on whether the operation was cancelled
    std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();

    if (!g_operationCancelled.load()) {
        if (fs::remove(srcPath, ec)) {
            completedBytes->fetch_add(fileSize);
            verboseIsos.push_back("\033[0;1mDeleted: \033[1;92m'" +
                                    srcDir + "/" + srcFile + "'\033[0;1m.");
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
    } else {
        verboseErrors.push_back("\033[1;91mError deleting: \033[1;93m'" +
                                  srcDir + "/" + srcFile + "'\033[1;91m: " +
                                  errorDetail + ".\033[0;1m");
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    }
    // Insert messages if batch size limit is reached
    batchInsertMessages();
}


// Function to perform Mv
bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile,const std::string& destDirProcessed, const std::string& destFile,size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = false;
    
    if (!g_operationCancelled.load()) {
        // Try rename first (faster if on same filesystem)
        fs::rename(srcPath, destPath, ec);
        
        if (ec) {
            // If rename fails, fall back to copy-then-delete
            ec.clear();
            success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
            if (success) {
                std::error_code deleteEc;
                if (!fs::remove(srcPath, deleteEc)) {
                    verboseErrors.push_back("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                                            srcDir + "/" + srcFile + "'\033[1;91m - " +
                                            deleteEc.message() + "\033[0m");
                    completedTasks->fetch_add(1, std::memory_order_acq_rel);
                } else {
                    completedTasks->fetch_add(1, std::memory_order_acq_rel);
                }
            }
        } else {
            // Rename succeeded
            completedBytes->fetch_add(fileSize);
            success = true;
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
    }
    
    if (!success || ec) {
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError moving: \033[1;93m'" + 
                                      srcDir + "/" + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mMoved: \033[1;92m'" + srcDir + "/" + srcFile +
                              "'\033[1m to \033[1;94m'" + destDirProcessed +
                              "/" + destFile + "'\033[0;1m.");
    }
    
    batchInsertMessages();
    return success;
}


// Helper function for multi-destination Mv
bool performMultiDestMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    
    if (!success || ec) {
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError moving: \033[1;93m'" + 
                                      srcDir + "/" + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mMoved: \033[1;92m'" + srcDir + "/" + srcFile +
                             "'\033[1m to \033[1;94m'" + destDirProcessed +
                             "/" + destFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    
    batchInsertMessages();
    return success;
}


// Function to perform Cp
bool performCopyOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    
    if (!success || ec) {
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError copying: \033[1;93m'" + 
                                      srcDir + "/" + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mCopied: \033[1;92m'" + srcDir + "/" + srcFile +
                             "'\033[1m to \033[1;94m'" + destDirProcessed +
                             "/" + destFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    
    batchInsertMessages();
    return success;
}


// Function to handle CpMvRm
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes,std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting) {

    std::atomic<bool> operationSuccessful(true);
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    // Local containers to accumulate verbose messages
    std::vector<std::string> verboseIsos;
    std::vector<std::string> verboseErrors;
    
    // Batch size for inserting entries into sets
    const size_t BATCH_SIZE = 1000;

    // Function to batch insert messages into global sets
    auto batchInsertMessages = [&]() {
        if (verboseIsos.size() >= BATCH_SIZE || verboseErrors.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
            operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
            verboseIsos.clear();
            verboseErrors.clear();
        }
    };

    std::vector<std::string> destDirs;
    std::istringstream iss(userDestDir);
    std::string destDir;
    while (std::getline(iss, destDir, ';')) {
        destDirs.push_back(fs::path(destDir).string());
    }

    // Change ownership function (no return value)
    auto changeOwnership = [&](const fs::path& path) {
        chown(path.c_str(), real_uid, real_gid); // Attempt to change ownership, ignore result
    };

    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string(), "cp_mv_rm");

            struct stat st;
            size_t fileSize = 0;
            if (stat(srcPath.c_str(), &st) == 0) {
                fileSize = st.st_size;
            }

            if (isDelete) {
                // Use the modularized delete function
                performDeleteOperation(srcPath, srcDir, srcFile, fileSize,
                                       completedBytes, completedTasks, failedTasks,
                                       verboseIsos, verboseErrors, operationSuccessful,
                                       batchInsertMessages);
            } else {
                std::atomic<bool> atLeastOneCopySucceeded(false);
                std::atomic<int> validDestinations(0);
                std::atomic<int> successfulOperations(0);
                
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& destDir = destDirs[i];
                    fs::path destPath = fs::path(destDir) / srcPath.filename();
                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string(), "cp_mv_rm");

                    // Check if source and destination are the same
                    fs::path absSrcPath = fs::absolute(srcPath);
                    fs::path absDestPath = fs::absolute(destPath);

                    if (absSrcPath == absDestPath) {
                        std::string operation = isMove ? "move" : "copy";
                        reportErrorCpMvRm("same_file", srcDir, srcFile, "", "", operation, 
                                  verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;
                    }

                    // Handle invalid directory as an error code
                    std::error_code ec;
                    if (!fs::exists(destDir, ec) || !fs::is_directory(destDir, ec)) {
                        std::string operation = isCopy ? "copying" : "moving";
                        reportErrorCpMvRm("invalid_dest", srcDir, srcFile, destDir, "Invalid destination", 
                                  operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;
                    }
                    
                    validDestinations.fetch_add(1, std::memory_order_acq_rel);
                    
                    if (!fs::exists(srcPath)) {
                        reportErrorCpMvRm("source_missing", srcDir, srcFile, "", "", "",
                                  verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;
                    }

                    if (fs::exists(destPath)) {
                        if (overwriteExisting) {
                            if (!fs::remove(destPath, ec)) {
                                reportErrorCpMvRm("overwrite_failed", "", "", destDirProcessed, ec.message(), "",
                                          verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                                continue;
                            }
                        } else {
                            std::string operation = isCopy ? "copying" : "moving";
                            reportErrorCpMvRm("file_exists", srcDir, srcFile, destDirProcessed, "", 
                                      operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                            continue;
                        }
                    }

                    std::atomic<bool> success(false);

                    if (isMove && destDirs.size() > 1) {
                        // For multiple destinations during move, use the modularized multi-destination move function
                        success.store(performMultiDestMoveOperation(
                            srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                            completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                            operationSuccessful, batchInsertMessages, changeOwnership));
                        
                        if (success.load()) {
                            atLeastOneCopySucceeded.store(true);
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isMove) {
                        // For single destination move, use the modularized move function
                        success.store(performMoveOperation(
                            srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                            fileSize, completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                            operationSuccessful, batchInsertMessages, changeOwnership));
                        
                        if (success.load()) {
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isCopy) {
                        // For copy operation, use the modularized copy function
                        success.store(performCopyOperation(
                            srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                            completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                            operationSuccessful, batchInsertMessages, changeOwnership));
                        
                        if (success.load()) {
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    }
                }
                
                // For multi-destination move: remove source file after copies succeed
                if (isMove && destDirs.size() > 1 && validDestinations > 0 && atLeastOneCopySucceeded.load()) {
                    std::error_code deleteEc;
                    if (!fs::remove(srcPath, deleteEc)) {
                        reportErrorCpMvRm("remove_after_move", srcDir, srcFile, "", deleteEc.message(), "",
                                  verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                    }
                }
            }
        }
    };

    std::vector<std::string> isoFilesToOperate;
    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string(), "cp_mv_rm");

        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
        if (it != isoFilesCopy.end()) {
            if (fs::exists(isoPath)) {
                isoFilesToOperate.push_back(iso);
            } else {
                reportErrorCpMvRm("missing_file", isoDir, isoFile, "", "", "",
                          verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
            }
        }
    }

    executeOperation(isoFilesToOperate);

    // Insert any remaining verbose messages at the end
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
        operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
    }
}
