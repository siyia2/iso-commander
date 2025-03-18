// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


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
bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::vector<std::vector<int>>& indexChunks, bool& umountMvRmBreak,
bool& abortDel) {
    bool isPageTurn = false;
    auto setupEnv = [&]() {
        rl_bind_key('\f', clear_screen_and_buffer);
    };

    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);

    std::string promptPrefix = "\n";
    std::string promptSuffix = std::string("\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be ") +
    "\001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (Y/N):\001\033[0;1m\002 ";

    while (true) {
        std::string userInput = handlePaginatedDisplay(
            entries,
            uniqueErrorMessages,
            promptPrefix,
            promptSuffix,
            setupEnv,
            isPageTurn
        );

        rl_bind_key('\f', prevent_readline_keybindings);
		
		// Continue loop on possible accidental blank enters
		if (userInput == "") {
			continue;
		}
		
        // If CTRL+D was pressed, userInput will be our special signal
        if (userInput == "EOF_SIGNAL") {
            umountMvRmBreak = false;
            abortDel = true;
            return false;
        }

        if (!isPageTurn) {
            if (userInput == "Y") {
                umountMvRmBreak = true;
                return true;
            } else {
                umountMvRmBreak = false;
                abortDel = true;
                std::cout << "\n\033[1;93mrm operation aborted by user.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return false;
            }
        }
    }
}


// Function to print the error message for Cp/Mv in userDestDirRm
std::string getPathErrorMessage(const std::string& path) {
    if (path.empty() || path[0] != '/')
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 must be absolute (start with '/').\001\033[0m\002";
    
    const std::string invalidChars = "|><&*?`$()[]{}\"'\\";
    for (char c : invalidChars)
        if (path.find(c) != std::string::npos)
            return "\001\033[1;91m\002Error: Invalid characters in path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002.\001\033[0m\002";
    
    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0)
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 does not exist.\001\033[0m\002";
    
    if (!S_ISDIR(pathStat.st_mode))
        return "\001\033[1;91m\002Error: \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 is not a directory.\001\033[0m\002";
    
    return ""; // No error
}


// Function to validate Linux file paths for Cp/Mv FolderPath prompt
bool isValidLinuxPath(const std::string& path) {
    // Check if path starts with a forward slash (absolute path)
    if (path.empty() || path[0] != '/') {
        return false;
    }
    
    // Check for invalid characters in path
    const std::string invalidChars = "|><&*?`$()[]{}\"'\\";
    
    for (char c : invalidChars) {
        if (path.find(c) != std::string::npos) {
            return false;
        }
    }
    
    // Check for control characters
    for (char c : path) {
        if (iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    
    // Avoid paths that are just spaces
    bool isOnlySpaces = true;
    for (char c : path) {
        if (c != ' ' && c != '\t') {
            isOnlySpaces = false;
            break;
        }
    }
    
    if (isOnlySpaces && !path.empty()) {
        return false;
    }
    
    // Check if path exists
    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0) {
        return false; // Path doesn't exist
    }
    
    // Ensure it's a directory
    if (!S_ISDIR(pathStat.st_mode)) {
        return false; // Path exists but is not a directory
    }
    
    return true;
}


// Function to prompt for userDestDir or Delete confirmation including pagination
std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel,
bool& overwriteExisting){
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);
    clearScrollBuffer();

    bool shouldContinue = true;
    std::string userInput;

    while (shouldContinue) {
        if (!isDelete) {
            bool isPageTurn = false;
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

            std::string promptPrefix = "\n";
            std::string promptSuffix = "\n\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " +
                operationColor + operationDescription +
                "\001\033[1;94m\002 into, ? ↵ for help, < ↵ to return:\n\001\033[0;1m\002";

            userInput = handlePaginatedDisplay(
                entries,
                uniqueErrorMessages,
                promptPrefix,
                promptSuffix,
                setupEnv,
                isPageTurn
            );

            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);

            // Check if CTRL+D (EOF) was pressed
            if (userInput == "EOF_SIGNAL") {
                shouldContinue = false;
                break;
            }

            if (userInput == "?") {
                bool import2ISO = false;
                bool isCpMv = true;
                helpSearches(isCpMv, import2ISO);
                userDestDir = "";
                continue;
            }

            if (userInput == "<") {
                umountMvRmBreak = false;
                userDestDir = "";
                clear_history();
                shouldContinue = false;
                continue;
            }

            // Blank enter (empty string) simply continues the loop
            if (userInput.empty()) {
                continue;
            }

            // Validate the path before processing
            userDestDir = userInput;

            // Check for overwrite flag and remove it for validation
            bool hasOverwriteFlag = false;
            if (userDestDir.size() >= 3 && userDestDir.substr(userDestDir.size() - 3) == " -o") {
                hasOverwriteFlag = true;
                userDestDir = userDestDir.substr(0, userDestDir.size() - 3);
            }

            bool pathsValid = true;
            std::string invalidPath;
            std::vector<std::string> paths;
            size_t startPos = 0;
            size_t delimPos;

            while ((delimPos = userDestDir.find(';', startPos)) != std::string::npos) {
                paths.push_back(userDestDir.substr(startPos, delimPos - startPos));
                startPos = delimPos + 1;
            }
            paths.push_back(userDestDir.substr(startPos));

            for (const auto& path : paths) {
                std::string trimmedPath = path;
                trimmedPath.erase(0, trimmedPath.find_first_not_of(" \t"));
                trimmedPath.erase(trimmedPath.find_last_not_of(" \t") + 1);
                if (!isValidLinuxPath(trimmedPath)) {
                    pathsValid = false;
                    invalidPath = path;
                    break;
                }
            }
            if (!pathsValid) {
                uniqueErrorMessages.insert(getPathErrorMessage(invalidPath));
                userDestDir = "";
                continue;
            }

            if (hasOverwriteFlag) {
                overwriteExisting = true;
                userDestDir = userInput.substr(0, userInput.size() - 3);
            } else {
                overwriteExisting = false;
                userDestDir = userInput;
            }

            std::string historyInput = userInput;
            if (historyInput.size() >= 3 && historyInput.substr(historyInput.size() - 3) == " -o") {
                historyInput = historyInput.substr(0, historyInput.size() - 3);
            }
            add_history(historyInput.c_str());

            shouldContinue = false;
        } else {
            // Delete operation flow
            bool proceedWithDelete = handleDeleteOperation(isoFiles, uniqueErrorMessages, indexChunks, umountMvRmBreak, abortDel);
            if (!proceedWithDelete) {
                userDestDir = "";
                shouldContinue = false;
                continue;
            }
            shouldContinue = false;
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
        [[maybe_unused]] int ret = chown(path.c_str(), real_uid, real_gid); // Attempt to change ownership, ignore result
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
