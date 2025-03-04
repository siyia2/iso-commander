// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Function to process selected indices for cpMvDel accordingly
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& umountMvRmBreak, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound) {
    setupSignalHandlerCancellations();
    
    bool overwriteExisting = false;
    std::string userDestDir;
    std::set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor       = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    // Parse the input to fill processedIndices.
    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }
    
    // --- Create chunks based on the operation type ---
    std::vector<std::vector<int>> indexChunks;
    if (!isDelete) {
        // Grouping logic for "cp" and "mv": group indices by their base filename.
        std::unordered_map<std::string, std::vector<int>> groups;
        for (int idx : processedIndices) {
            // Safe index access with bounds checking and explicit type conversion
            if (idx > 0 && static_cast<std::vector<std::string>::size_type>(idx - 1) < isoFiles.size()) {
                std::string baseName = std::filesystem::path(isoFiles[static_cast<std::vector<std::string>::size_type>(idx - 1)]).filename().string();
                groups[baseName].push_back(idx);
            }
        }
        
        // Convert the map into a vector of groups and sort groups by the first index to preserve order.
        std::vector<std::vector<int>> groupList;
        for (auto& kv : groups) {
            groupList.push_back(kv.second);
        }
        std::sort(groupList.begin(), groupList.end(), [](const std::vector<int>& a, const std::vector<int>& b) {
            return a.front() < b.front();
        });

        // Merge groups into chunks so that each chunk has at most maxFilesPerChunk files (if possible)
        const size_t maxFilesPerChunk = 5;
        std::vector<int> currentChunk;
        for (const auto& group : groupList) {
            if (!currentChunk.empty() && (currentChunk.size() + group.size() > maxFilesPerChunk)) {
                indexChunks.push_back(currentChunk);
                currentChunk.clear();
            }
            currentChunk.insert(currentChunk.end(), group.begin(), group.end());
        }
        if (!currentChunk.empty()) {
            indexChunks.push_back(currentChunk);
        }
    } else {
        // For "rm", process each file individually (no grouping)
        for (int idx : processedIndices) {
            indexChunks.push_back({ idx });
        }
    }

    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirRm(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
                                                     operationColor, operationDescription, umountMvRmBreak, 
                                                     historyPattern, isDelete, isCopy, abortDel, overwriteExisting);
        
    g_operationCancelled.store(false);
    
    if ((processedUserDestDir.empty() && (isCopy || isMove)) || abortDel) {
        uniqueErrorMessages.clear();
        return;
    }
    uniqueErrorMessages.clear();
    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing " + operationColor + process +
                 "\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";

    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        // Safe index access with bounds checking and explicit type conversion
        if (index > 0 && static_cast<std::vector<std::string>::size_type>(index - 1) < isoFiles.size()) {
            filesToProcess.push_back(isoFiles[static_cast<std::vector<std::string>::size_type>(index - 1)]);
        }
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    size_t totalBytes = getTotalFileSize(filesToProcess);
    size_t totalTasks = filesToProcess.size();
    
    // Adjust totals for copy/move operations with multiple destinations
    if (isCopy || isMove) {
        // Use safe counting method to avoid potential signed/unsigned conversion
        size_t destCount = 1;
        if (!processedUserDestDir.empty()) {
            destCount = static_cast<size_t>(std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1);
        }
        totalBytes *= destCount;
        totalTasks *= destCount;
    }
    
    std::atomic<bool> isProcessingComplete(false);

    // Start progress tracking in a separate thread.
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
                                 totalBytes, &completedTasks, &failedTasks, 
                                 totalTasks, &isProcessingComplete, &verbose);

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    // For each chunk, create a vector of file names and enqueue the operation.
    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(isoFilesInChunk),
            [&isoFiles](int index) { 
                // Safe index access with bounds checking and explicit type conversion
                return (index > 0 && static_cast<std::vector<std::string>::size_type>(index - 1) < isoFiles.size()) 
                    ? isoFiles[static_cast<std::vector<std::string>::size_type>(index - 1)] 
                    : ""; 
            });

        // Remove any empty strings that might have been added due to invalid indices
        isoFilesInChunk.erase(
            std::remove_if(isoFilesInChunk.begin(), isoFilesInChunk.end(), 
                           [](const std::string& s) { return s.empty(); }),
            isoFilesInChunk.end()
        );

        if (!isoFilesInChunk.empty()) {
            futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), 
                                                 &isoFiles, &operationIsos, &operationErrors, &userDestDir, 
                                                 isMove, isCopy, isDelete, &completedBytes, &completedTasks, 
                                                 &failedTasks, &overwriteExisting]() {
                handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, 
                                       userDestDir, isMove, isCopy, isDelete, 
                                       &completedBytes, &completedTasks, &failedTasks, overwriteExisting);
            }));
        }
    }

    for (auto& future : futures) {
        future.wait();
        if (g_operationCancelled.load())
            break;
    }

    isProcessingComplete.store(true);
    progressThread.join();

    promptFlag = false;
    maxDepth = 0;
    
    if (!isDelete) {
        manualRefreshCache(userDestDir, promptFlag, maxDepth, historyPattern, newISOFound);
    }
    
    if (!isDelete && !operationIsos.empty()) {
        saveHistory(historyPattern);
        clear_history();
    }

    clear_history();
    promptFlag = true;
    maxDepth = -1;
}


// Function to prompt for userDestDir and Delete confirmation
std::string userDestDirRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting) {
    
    // Generate entries for selected ISO files - used by both branches
    auto generateSelectedIsosEntries = [&]() {
        std::vector<std::string> entries;
        for (const auto& chunk : indexChunks) {
            for (int index : chunk) {
                // Safely convert index to size_t
                auto safeIndex = static_cast<size_t>(std::max(1, index)) - 1;
                auto [shortDir, filename] = extractDirectoryAndFilename(isoFiles[safeIndex], "cp_mv_rm");
                std::ostringstream oss;
                oss << "\033[1m-> " << shortDir << "/\033[95m" << filename << "\033[0m\n";
                entries.push_back(oss.str());
            }
        }
        return entries;
    };
    
    // Common pagination logic for both flows
    auto setupPagination = [](size_t totalEntries) {
        size_t entriesPerPage;

        if (totalEntries <= 25) {
            entriesPerPage = totalEntries;  // Single page
        } else {
            entriesPerPage = std::max(static_cast<size_t>(25), (totalEntries + 4) / 5);
            // Cap entriesPerPage at 100 if it exceeds, allowing more pages
            if (entriesPerPage > 100) {
                entriesPerPage = 100;
            }
        }

        size_t totalPages = (totalEntries + entriesPerPage - 1) / entriesPerPage;
        return std::make_tuple(entriesPerPage, totalPages);
    };

    // Display error messages if any
    auto displayErrors = [&]() {
        if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
            for (const auto& err : uniqueErrorMessages) {
                std::cout << err << "\n";
            }
        }
    };
    
    // Create page content for current pagination state
    auto getPageContent = [](const std::vector<std::string>& entries, size_t currentPage, size_t entriesPerPage, size_t totalPages) {
        size_t totalEntries = entries.size();
        size_t start = currentPage * entriesPerPage;
        size_t end = std::min(start + entriesPerPage, totalEntries);
        
        std::ostringstream oss;
        for (size_t i = start; i < end; ++i) {
            oss << entries[i];
        }
        
        std::string content = oss.str();
        if (totalPages > 1) {
            content += "\n\033[1mPage " + std::to_string(currentPage + 1) + 
                       "/" + std::to_string(totalPages) + " \033[1;94m(+/-) ↵\n\033[0m";
        }
        return content;
    };
    
    // Handle pagination navigation
    auto handlePageNavigation = [&](const std::string& input, size_t& currentPage, size_t totalPages) -> bool {
        // Make sure this is just a navigation command, not a path with +/- in it
        bool isJustNavigation = true;
        for (char c : input) {
            if (c != '+' && c != '-') {
                isJustNavigation = false;
                break;
            }
        }
        
        if (isJustNavigation && (input.find('+') != std::string::npos || input.find('-') != std::string::npos)) {
            int pageShift = 0;
            if (input.find('+') != std::string::npos) {
                pageShift = static_cast<int>(std::count(input.begin(), input.end(), '+'));
            } else if (input.find('-') != std::string::npos) {
                pageShift = -static_cast<int>(std::count(input.begin(), input.end(), '-'));
            }

            // Safely handle page navigation with signed arithmetic but unsigned storage
            currentPage = (currentPage + static_cast<size_t>(pageShift) + totalPages) % totalPages; 
            return true;
        }
        return false;
    };
    
    // Generate entries once for efficiency
    auto entries = generateSelectedIsosEntries();
    size_t totalEntries = entries.size();
    auto [entriesPerPage, totalPages] = setupPagination(totalEntries);
    size_t currentPage = 0;
    
    // Clear screen initially (common for both paths)
    clearScrollBuffer();
    displayErrors(); // Show errors on first display

    if (!isDelete) {
        // Copy/Move operation flow
        bool isPageTurn = false; // Flag to track if we're coming from a page turn
        
        while (true) {
            // Setup environment
            enable_ctrl_d();
            setupSignalHandlerCancellations();
            g_operationCancelled.store(false);
            rl_bind_key('\f', clear_screen_and_buffer);
            rl_bind_key('\t', rl_complete);
            
            if (!isCopy) {
                umountMvRmBreak = true;
            }
            
            // Clear the screen before displaying the new page
            clearScrollBuffer(); 
            
            // Only reset history and display errors on non-page-turn iterations
            if (!isPageTurn) {
                clear_history();
                historyPattern = false;
                loadHistory(historyPattern);
                displayErrors();
            }
            
            userDestDir.clear();
            bool isCpMv = true;
            std::string selectedIsosPrompt = getPageContent(entries, currentPage, entriesPerPage, totalPages);
            
            std::string prompt = "\n" + selectedIsosPrompt + 
                "\n\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " + 
                operationColor + operationDescription + 
                "\001\033[1;94m\002 into, ? ↵ for help, ↵ to return:\n\001\033[0;1m\002";
            
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            if (!input.get()) {
                break;
            }
            
            // Trim leading and trailing whitespaces but keep spaces inside
            std::string mainInputString = trimWhitespace(input.get());
        
            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);
            
            // Check for page navigation inputs
            if (handlePageNavigation(mainInputString, currentPage, totalPages)) {
                isPageTurn = true;
                continue;
            }
            
            // Then check for help command
            if (mainInputString == "?") {
                bool import2ISO = false;
                helpSearches(isCpMv, import2ISO);
                isPageTurn = false; // Not a page turn
                continue;
            } else {
                isPageTurn = false; // Reset flag for non-page-turn actions
            }
            
            // Handle empty input (return)
            if (mainInputString.empty()) {
                umountMvRmBreak = false;
                userDestDir = "";
                clear_history();
                return userDestDir;
            } 
            
            // Process destination directory including possible -o flag
            userDestDir = mainInputString;
            
            // Check for overwrite flag
            if (userDestDir.size() >= 3 && userDestDir.substr(userDestDir.size() - 3) == " -o") {
                overwriteExisting = true;
                userDestDir = userDestDir.substr(0, userDestDir.size() - 3);
            } else {
                overwriteExisting = false;
            }
            
            // Add to history without the overwrite flag
            std::string historyInput = mainInputString;
            if (historyInput.size() >= 3 && historyInput.substr(historyInput.size() - 3) == " -o") {
                historyInput = historyInput.substr(0, historyInput.size() - 3);
            }
            add_history(historyInput.c_str());
            break;
        }
    } else {
        // Delete operation flow
        rl_bind_key('\f', clear_screen_and_buffer);
        
        while (true) {
            std::string selectedIsosPrompt = getPageContent(entries, currentPage, entriesPerPage, totalPages);
            
            std::string prompt = "\n" + selectedIsosPrompt +
                "\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be " +
                "\001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (y/n):\001\033[0;1m\002 ";
            
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            rl_bind_key('\f', prevent_readline_keybindings);
            
            if (!input.get()) {
                userDestDir = "";
                abortDel = true;
                return userDestDir;
            }
            
            std::string mainInputString(input.get());
            
            // Handle pagination
            if (handlePageNavigation(mainInputString, currentPage, totalPages)) {
                clearScrollBuffer();
                continue;
            }
            
            // Process yes/no
            if (mainInputString == "y" || mainInputString == "Y") {
                umountMvRmBreak = true;
                break;
            } else {
                umountMvRmBreak = false;
                abortDel = true;
                userDestDir = "";
                std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return userDestDir;
            }
        }
    }
    
    return userDestDir;
}


namespace fs = std::filesystem;


// Function to buffer file copying
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
    const size_t bufferSize = 8 * 1024 * 1024; // 8MB buffer
    std::vector<char> buffer(bufferSize);
    
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
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize bytesRead = input.gcount();
        
        if (bytesRead == 0) {
            break;
        }
        
        output.write(buffer.data(), bytesRead);
        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        
        completedBytes->fetch_add(static_cast<size_t>(bytesRead), std::memory_order_relaxed);
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

void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting) {

    bool operationSuccessful = true;
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

    // Function to batch insert messages into sets
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
    std::string parsedDestDir;
    while (std::getline(iss, parsedDestDir, ';')) {
        destDirs.push_back(fs::path(parsedDestDir).string());
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
                fileSize = static_cast<size_t>(st.st_size);
            }

            if (isDelete) {
                if (g_operationCancelled.load()) break;
                std::error_code ec;
                if (fs::remove(srcPath, ec)) {
                    completedBytes->fetch_add(fileSize);
                    verboseIsos.push_back("\033[0;1mDeleted: \033[1;92m'" +
                                            srcDir + "/" + srcFile + "'\033[0;1m.");
                    completedTasks->fetch_add(1, std::memory_order_acq_rel);
                } else {
                    verboseErrors.push_back("\033[1;91mError deleting: \033[1;93m'" +
                                              srcDir + "/" + srcFile + "'\033[1;91m: " +
                                              ec.message() + ".\033[0;1m");
                    failedTasks->fetch_add(1, std::memory_order_acq_rel);
                    operationSuccessful = false;
                }
                
                // Check if we need to batch insert
                batchInsertMessages();
            } else {
                bool atLeastOneCopySucceeded = false;
                std::atomic<int> validDestinations(0);
                std::atomic<int> successfulOperations(0);
                
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& currentDestDir = destDirs[i];
                    fs::path destPath = fs::path(currentDestDir) / srcPath.filename();
                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string(), "cp_mv_rm");

                    // Check if source and destination are the same
                    fs::path absSrcPath = fs::absolute(srcPath);
                    fs::path absDestPath = fs::absolute(destPath);

                    if (absSrcPath == absDestPath) {
                        verboseErrors.push_back("\033[1;91mCannot " +
                                                  std::string(isCopy ? "copy" : "move") +
                                                  " file to itself: \033[1;93m'" +
                                                  srcDir + "/" + srcFile + "'\033[1;91m.\033[0m");
                        failedTasks->fetch_add(1, std::memory_order_acq_rel);
                        operationSuccessful = false;
                        
                        // Check if we need to batch insert
                        batchInsertMessages();
                        continue;
                    }

                    // Handle invalid directory as an error code
                    std::error_code ec;
                    if (!fs::exists(currentDestDir, ec) || !fs::is_directory(currentDestDir, ec)) {
                        ec = std::make_error_code(std::errc::no_such_file_or_directory);
                        std::string errorDetail = "Invalid destination";
                        verboseErrors.push_back("\033[1;91mError " +
                                                  std::string(isCopy ? "copying" : "moving") +
                                                  ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                                                  " to '" + currentDestDir + "': " + errorDetail + "\033[1;91m.\033[0;1m");
                        failedTasks->fetch_add(1, std::memory_order_acq_rel);
                        operationSuccessful = false;
                        
                        // Check if we need to batch insert
                        batchInsertMessages();
                        continue;
                    }
                    
                    // Count valid destinations for reporting
                    validDestinations.fetch_add(1, std::memory_order_acq_rel);
                    
                    // Inside the lock, check again if source exists (might have been moved by another operation)
                    if (!fs::exists(srcPath)) {
                        verboseErrors.push_back("\033[1;91mSource file no longer exists: \033[1;93m'" +
                                                 srcDir + "/" + srcFile + "'\033[1;91m.\033[0;1m");
                        failedTasks->fetch_add(1, std::memory_order_acq_rel);
                        operationSuccessful = false;
                        
                        // Check if we need to batch insert
                        batchInsertMessages();
                        continue;
                    }

                    // Check if destination exists
                    if (fs::exists(destPath)) {
                        if (overwriteExisting) {
                            if (!fs::remove(destPath, ec)) {
                                verboseErrors.push_back("\033[1;91mFailed to overwrite: \033[1;93m'" +
                                                          destDirProcessed + "/" + destFile +
                                                          "'\033[1;91m - " + ec.message() + ".\033[0;1m");
                                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                                operationSuccessful = false;
                                
                                // Check if we need to batch insert
                                batchInsertMessages();
                                continue;
                            }
                        } else {
                            ec = std::make_error_code(std::errc::file_exists);
                            verboseErrors.push_back("\033[1;91mError " +
                                                     std::string(isCopy ? "copying" : "moving") +
                                                     ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                                                     " to '" + destDirProcessed + "/': File exists (enable overwrites)\033[1;91m.\033[0;1m");
                            failedTasks->fetch_add(1, std::memory_order_acq_rel);
                            operationSuccessful = false;
                            
                            // Check if we need to batch insert
                            batchInsertMessages();
                            continue;
                        }
                    }

                    bool success = false;

                    if (isMove && destDirs.size() > 1) {
                        // For multiple destinations during move, always use copy approach
                        success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                        if (success) {
                            atLeastOneCopySucceeded = true;
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isMove) {
                        // For single destination move, try rename first
                        fs::rename(srcPath, destPath, ec);
                        if (ec) {
                            ec.clear();
                            success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                            if (success) {
                                std::error_code deleteEc;
                                if (!fs::remove(srcPath, deleteEc)) {
                                    verboseErrors.push_back("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                                                              srcDir + "/" + srcFile + "'\033[1;91m - " +
                                                              deleteEc.message() + "\033[0m");
                                    successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                                } else {
                                    successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                                }
                            }
                        } else {
                            completedBytes->fetch_add(fileSize);
                            success = true;
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isCopy) {
                        success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                        if (success) {
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    }

                    if (!success || ec) {
                        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
                        std::string errorMessageInfo = "\033[1;91mError " +
                        std::string(isCopy ? "copying" : "moving") +
                        ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                        " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
                        verboseErrors.push_back(errorMessageInfo);
                        failedTasks->fetch_add(1, std::memory_order_acq_rel);
                        operationSuccessful = false;
                    } else {
                        // Attempt to change ownership, but ignore success/failure
                        changeOwnership(destPath);

                        verboseIsos.push_back("\033[0;1m" +
                                                std::string(isCopy ? "Copied" : "Moved") +
                                                ": \033[1;92m'" + srcDir + "/" + srcFile +
                                                "'\033[1m to \033[1;94m'" + destDirProcessed +
                                                "/" + destFile + "'\033[0;1m.");
                        completedTasks->fetch_add(1, std::memory_order_acq_rel);
                    }
                    
                    // Check if we need to batch insert
                    batchInsertMessages();
                }
                
                // For multi-destination move: remove source file after copies succeed
                if (isMove && destDirs.size() > 1 && validDestinations > 0 && atLeastOneCopySucceeded) {
                    
                    std::error_code deleteEc;
                    if (!fs::remove(srcPath, deleteEc)) {
                        verboseErrors.push_back("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                                                  srcDir + "/" + srcFile + "'\033[1;91m - " +
                                                  deleteEc.message() + "\033[0m");
                        
                        // Check if we need to batch insert
                        batchInsertMessages();
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
                verboseErrors.push_back("\033[1;35mMissing: \033[1;93m'" +
                                          isoDir + "/" + isoFile + "'\033[1;35m.\033[0;1m");
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                
                // Check if we need to batch insert
                batchInsertMessages();
            }
        }
    }

    executeOperation(isoFilesToOperate);

    // At the end, insert any remaining collected verbose messages
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
        operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
    }
}
