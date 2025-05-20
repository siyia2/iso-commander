// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


// Function to generate entries for selected ISO files
std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
    std::vector<std::string> entries;

    // Generate entries for selected ISO files
    for (const auto& chunk : indexChunks) {
        // Iterate through each index in the chunk
        for (int index : chunk) {
            // Extract the directory and filename from the ISO file
            auto [shortDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");
            
            // Format the string to highlight the directory and filename
            std::ostringstream oss;
            oss << "\033[1m-> " << shortDir << "/\033[95m" << filename << "\033[0m\n";
            entries.push_back(oss.str()); // Add formatted entry to the list
        }
    }

    return entries; // Return the list of formatted ISO file entries
}


// Function to handle rm including pagination
bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::vector<std::vector<int>>& indexChunks, bool& umountMvRmBreak,
bool& abortDel) {
    bool isPageTurn = false;
    
    // Set up environment for clearing screen using readline
    auto setupEnv = [&]() {
        rl_bind_key('\f', clear_screen_and_buffer); // Bind clear screen function to page break key
    };

    // Generate the list of ISO entries for the deletion operation
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries); // Sort files case-insensitively

    // Define the prompt that will be displayed for the user
    std::string promptPrefix = "\n";
    std::string promptSuffix = std::string("\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be ") +
    "\001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (Y/N):\001\033[0;1m\002 ";

    while (true) {
        // Display entries with pagination and handle user input
        std::string userInput = handlePaginatedDisplay(
            entries,
            uniqueErrorMessages,
            promptPrefix,
            promptSuffix,
            setupEnv,
            isPageTurn
        );

        // Bind the key to prevent accidental clear screen binding during the loop
        rl_bind_key('\f', prevent_readline_keybindings);
        
        // Continue the loop if the user presses an accidental blank enter
        if (userInput == "") {
            continue;
        }
        
        // If CTRL+D was pressed, set a special signal for EOF
        if (userInput == "EOF_SIGNAL") {
            umountMvRmBreak = false; // Prevent further unmount
            abortDel = true; // Mark delete operation as aborted
            return false; // Return false, as operation is aborted
        }

        if (!isPageTurn) {
            // If no page turn occurred, check the user's response
            if (userInput == "Y") {
                umountMvRmBreak = true; // Confirm the deletion operation
                return true;
            } else {
                umountMvRmBreak = false; // Abort the deletion operation
                abortDel = true;
                std::cout << "\n\033[1;93mrm operation aborted by user.\033[0;1m\n"; // Inform user about abortion
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m"; // Wait for user to continue
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // Ignore input to continue
                return false;
            }
        }
    }
}


// Function to print the error message for Cp/Mv in userDestDirRm
std::string getPathErrorMessage(const std::string& path) {
    // Ensure the path is absolute (starts with '/')
    if (path.empty() || path[0] != '/')
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 must be absolute (start with '/').\001\033[0m\002";
    
    // Check for invalid characters in the path
    const std::string invalidChars = "|><&*?`$()[]{}\"'\\";
    for (char c : invalidChars)
        if (path.find(c) != std::string::npos)
            return "\001\033[1;91m\002Error: Invalid characters in path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002.\001\033[0m\002";
    
    // Check if the path exists
    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0)
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 does not exist.\001\033[0m\002";
    
    // Check if the path is a directory
    if (!S_ISDIR(pathStat.st_mode))
        return "\001\033[1;91m\002Error: \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 is not a directory.\001\033[0m\002";
    
    return ""; // No error found
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


// This function manages the user input for setting the destination directory and operation type
// for copying or deleting ISO files, while supporting features like history management, validation, and pagination.
std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting) {
    
    // Generate the list of ISO entries and sort them case-insensitively
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);
    clearScrollBuffer(); // Clear the scroll buffer to prepare for the next operation

    bool shouldContinue = true;
    std::string userInput;

    while (shouldContinue) {
        if (!isDelete) { // If not in delete mode, handle copying or moving operation
            bool isPageTurn = false;

            // Setup environment, bind keys, and handle operation cancellation
            auto setupEnv = [&]() {
                enable_ctrl_d();  // Enable CTRL+D for EOF
                setupSignalHandlerCancellations();  // Setup signal handler to cancel operations
                g_operationCancelled.store(false);  // Reset operation cancellation flag
                rl_bind_key('\f', clear_screen_and_buffer);  // Bind CTRL+L to clear screen and buffer
                rl_bind_key('\t', rl_complete);  // Bind TAB key for autocomplete
                if (!isCopy) {
                    umountMvRmBreak = true;  // Set break flag for unmount/move/remove operations
                }
                if (!isPageTurn) {
                    clear_history();  // Clear history before starting the operation
                    filterHistory = false;
                    loadHistory(filterHistory);  // Load history without filtering
                }
            };

            // Prompt strings for user input, highlighting the operation (copy or move)
            std::string promptPrefix = "\n";
            std::string promptSuffix = "\n\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " +
                operationColor + operationDescription +
                "\001\033[1;94m\002 into, ? ↵ for help, < ↵ to return:\n\001\033[0;1m\002";

            // Handle the paginated display and process user input
            userInput = handlePaginatedDisplay(
                entries,
                uniqueErrorMessages,
                promptPrefix,
                promptSuffix,
                setupEnv,
                isPageTurn
            );

            // Disable specific readline keybindings after input handling
            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);

            // Check if user wants to exit (CTRL+D)
            if (userInput == "EOF_SIGNAL") {
                shouldContinue = false;
                break;
            }

            // If user requested help, display help content
            if (userInput == "?") {
                bool import2ISO = false;
                bool isCpMv = true;
                helpSearches(isCpMv, import2ISO);  // Display help searches for the operation
                userDestDir = "";  // Reset the destination directory for the user
                continue;
            }

            // If user wants to go back to the previous step
            if (userInput == "<") {
                umountMvRmBreak = false;
                userDestDir = "";  // Reset the destination directory
                clear_history();  // Clear history and break the loop
                shouldContinue = false;
                continue;
            }

            // If the user presses enter without input, continue the loop
            if (userInput.empty()) {
                continue;
            }

            // Validate the provided destination path
            userDestDir = userInput;

            // Check for overwrite flag and remove it for validation
            bool hasOverwriteFlag = false;
            if (userDestDir.size() >= 3 && userDestDir.substr(userDestDir.size() - 3) == " -o") {
                hasOverwriteFlag = true;
                userDestDir = userDestDir.substr(0, userDestDir.size() - 3);  // Remove "-o" if present
            }

            bool pathsValid = true;
            std::string invalidPath;
            std::vector<std::string> paths;
            size_t startPos = 0;
            size_t delimPos;

            // Split the user input into individual paths, separated by semicolons
            while ((delimPos = userDestDir.find(';', startPos)) != std::string::npos) {
                paths.push_back(userDestDir.substr(startPos, delimPos - startPos));
                startPos = delimPos + 1;
            }
            paths.push_back(userDestDir.substr(startPos));

            // Validate each path
            for (const auto& path : paths) {
                std::string trimmedPath = path;
                trimmedPath.erase(0, trimmedPath.find_first_not_of(" \t"));
                trimmedPath.erase(trimmedPath.find_last_not_of(" \t") + 1);
                if (!isValidLinuxPath(trimmedPath)) {  // Check if path is valid
                    pathsValid = false;
                    invalidPath = path;
                    break;
                }
            }

            // If any path is invalid, add an error message and continue the loop
            if (!pathsValid) {
                uniqueErrorMessages.insert(getPathErrorMessage(invalidPath));
                userDestDir = "";
                continue;
            }

            // Handle the overwrite flag
            if (hasOverwriteFlag) {
                overwriteExisting = true;
                userDestDir = userInput.substr(0, userInput.size() - 3);  // Remove "-o" for final destination
            } else {
                overwriteExisting = false;
                userDestDir = userInput;
            }

            // Add user input to history for future reference
            std::string historyInput = userInput;
            if (historyInput.size() >= 3 && historyInput.substr(historyInput.size() - 3) == " -o") {
                historyInput = historyInput.substr(0, historyInput.size() - 3);
            }
            add_history(historyInput.c_str());  // Add valid history input
            saveHistory(filterHistory);

            shouldContinue = false;  // End the loop after valid input
        } else {
            // Handle the delete operation flow
            bool proceedWithDelete = handleDeleteOperation(isoFiles, uniqueErrorMessages, indexChunks, umountMvRmBreak, abortDel);
            if (!proceedWithDelete) {
                userDestDir = "";  // Reset destination directory on failure
                shouldContinue = false;
                continue;
            }
            shouldContinue = false;  // End the loop after delete operation
        }
    }

    return userDestDir;  // Return the final destination directory
}


// Function to copy a file with progress reporting, using a buffered copy approach
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
    // Define buffer size (8MB buffer for copying chunks)
    const size_t bufferSize = 8 * 1024 * 1024; // 8MB buffer
    std::vector<char> buffer(bufferSize); // Create a buffer to hold data while copying
    
    // Check if the operation was cancelled before starting
    if (g_operationCancelled.load()) return false;
    
    // Open the input file in binary mode for reading
    std::ifstream input(src, std::ios::binary);
    if (!input) { // Check if the input file was successfully opened
        ec = std::make_error_code(std::errc::no_such_file_or_directory); // Set error code if the file doesn't exist
        return false;
    }
    
    // Open the output file in binary mode for writing
    std::ofstream output(dst, std::ios::binary);
    if (!output) { // Check if the output file was successfully opened
        ec = std::make_error_code(std::errc::permission_denied); // Set error code if there is a permission issue
        return false;
    }
    
    // Loop for reading from input and writing to output, checking for cancellation at each step
    while (!g_operationCancelled.load()) { // Check cancellation flag at each iteration
        // Read data into the buffer from the input file
        input.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = input.gcount(); // Get the number of bytes read
        
        // If no more data is read, exit the loop
        if (bytesRead == 0) {
            break;
        }
        
        // Write the data to the output file
        output.write(buffer.data(), bytesRead);
        if (!output) { // Check for any errors while writing to the output file
            ec = std::make_error_code(std::errc::io_error); // Set error code for IO error
            return false;
        }
        
        // Update the completed bytes counter atomically
        completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
    }

    // Check if the operation was cancelled after exiting the loop
    if (g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled); // Set error code for cancellation
        output.close(); // Close the output stream before attempting to delete the partial file
        fs::remove(dst, ec); // Delete the partial output file if the operation was cancelled
        return false;
    }
    
    // If the copy operation was successful and not cancelled, return true
    return true;
}


// Function to perform Delete operation
void performDeleteOperation(const fs::path& srcPath, const std::string& srcDir, const std::string& srcFile, size_t fileSize,
                             std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
                             std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, 
                             std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {
    std::error_code ec;
    // Set errorDetail based on whether the operation was cancelled
    std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();

    if (!g_operationCancelled.load()) {
        // Try removing the file if the operation wasn't cancelled
        if (fs::remove(srcPath, ec)) {
            completedBytes->fetch_add(fileSize); // Add file size to completed bytes count
            verboseIsos.push_back("\033[0;1mDeleted: \033[1;92m'" + srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[0;1m.");
            completedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment completed tasks count
        }
    } else {
        // If operation was cancelled, add error message
        verboseErrors.push_back("\033[1;91mError deleting: \033[1;93m'" + srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[1;91m: " +
                                 errorDetail + ".\033[0;1m");
        failedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment failed tasks count
        operationSuccessful.store(false); // Set operation as unsuccessful
    }
    // Insert messages if batch size limit is reached
    batchInsertMessages();
}


// Function to perform Move operation
bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = false;
    
    if (!g_operationCancelled.load()) {
        // Attempt to rename the file (this is faster if the source and destination are on the same filesystem)
        fs::rename(srcPath, destPath, ec);
        
        if (ec) {
            // If rename fails, fall back to copy-then-delete
            ec.clear();
            success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
            if (success) {
                // Try to remove the source file after successful copy
                std::error_code deleteEc;
                if (!fs::remove(srcPath, deleteEc)) {
                    // If deletion fails, log error message
                    verboseErrors.push_back("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                                            srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[1;91m - " +
                                            deleteEc.message() + "\033[0m");
                    completedTasks->fetch_add(1, std::memory_order_acq_rel);
                } else {
                    completedTasks->fetch_add(1, std::memory_order_acq_rel);
                }
            }
        } else {
            // Rename succeeded
            completedBytes->fetch_add(fileSize); // Add file size to completed bytes count
            success = true;
            completedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment completed tasks count
        }
    }
    
    if (!success || ec) {
        // If operation failed or an error occurred, log error
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError moving: \033[1;93m'" + 
                                      srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment failed tasks count
        operationSuccessful.store(false); // Set operation as unsuccessful
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mMoved: \033[1;92m'" + srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile +
                              "'\033[1m to \033[1;94m'" + destDirProcessed + (!displayConfig::toggleNamesOnly ? "/" : "") + destFile + "'\033[0;1m.");
    }
    
    batchInsertMessages(); // Insert messages if batch size limit is reached
    return success; // Return success status
}


// Helper function for multi-destination Move operation
bool performMultiDestMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    
    if (!success || ec) {
        // If copying fails, log error
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError moving: \033[1;93m'" + 
                                      srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment failed tasks count
        operationSuccessful.store(false); // Set operation as unsuccessful
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mMoved: \033[1;92m'" + srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile +
                             "'\033[1m to \033[1;94m'" + destDirProcessed + (!displayConfig::toggleNamesOnly ? "/" : "") + destFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment completed tasks count
    }
    
    batchInsertMessages(); // Insert messages if batch size limit is reached
    return success; // Return success status
}


// Function to perform Copy operation
bool performCopyOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {
    
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    
    if (!success || ec) {
        // If copying fails, log error
        std::string errorDetail = g_operationCancelled.load() ? "Cancelled" : ec.message();
        std::string errorMessageInfo = "\033[1;91mError copying: \033[1;93m'" + 
                                      srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile + "'\033[1;91m" +
                                      " to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m";
        verboseErrors.push_back(errorMessageInfo);
        failedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment failed tasks count
        operationSuccessful.store(false); // Set operation as unsuccessful
    } else {
        // Attempt to change ownership, ignoring any errors
        changeOwnership(destPath);
        verboseIsos.push_back("\033[0;1mCopied: \033[1;92m'" + srcDir + (!displayConfig::toggleNamesOnly ? "/" : "") + srcFile +
                             "'\033[1m to \033[1;94m'" + destDirProcessed + (!displayConfig::toggleNamesOnly ? "/" : "") + destFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel); // Increment completed tasks count
    }
    
    batchInsertMessages(); // Insert messages if batch size limit is reached
    return success; // Return success status
}


// Function to handle CpMvRm
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting) {
    // Flag to track if the operation is successful
    std::atomic<bool> operationSuccessful(true);
    
    // Variables for real user and group ID and name retrieval
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    // Local containers to accumulate verbose messages for logging
    std::vector<std::string> verboseIsos;
    std::vector<std::string> verboseErrors;
    
    // Set a batch size to reduce the number of insertions into the global sets
    const size_t BATCH_SIZE = 1000;

    // Function to batch insert error and ISO messages into the global sets
    auto batchInsertMessages = [&]() {
        if (verboseIsos.size() >= BATCH_SIZE || verboseErrors.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
            operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
            verboseIsos.clear();
            verboseErrors.clear();
        }
    };

    // Parse destination directories from the userDestDir string (semicolon separated)
    std::vector<std::string> destDirs;
    std::istringstream iss(userDestDir);
    std::string destDir;
    while (std::getline(iss, destDir, ';')) {
        destDirs.push_back(fs::path(destDir).string());
    }

    // Helper function to change the ownership of a file
    auto changeOwnership = [&](const fs::path& path) {
        [[maybe_unused]] int ret = chown(path.c_str(), real_uid, real_gid); // Attempt to change ownership, ignore result
    };

    // Function to execute the file operation (move, copy, delete) on the provided files
    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string(), "cp_mv_rm");

            struct stat st;
            size_t fileSize = 0;
            if (stat(srcPath.c_str(), &st) == 0) {
                fileSize = st.st_size;  // Get the file size
            }

            if (isDelete) {
                // Perform deletion of the file
                performDeleteOperation(srcPath, srcDir, srcFile, fileSize,
                                       completedBytes, completedTasks, failedTasks,
                                       verboseIsos, verboseErrors, operationSuccessful,
                                       batchInsertMessages);
            } else {
                std::atomic<bool> atLeastOneCopySucceeded(false);
                std::atomic<int> validDestinations(0);
                std::atomic<int> successfulOperations(0);
                
                // Process each destination directory for the operation (move/copy)
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& destDir = destDirs[i];
                    fs::path destPath = fs::path(destDir) / srcPath.filename();
                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string(), "cp_mv_rm");

                    // Check if source and destination paths are the same
                    fs::path absSrcPath = fs::absolute(srcPath);
                    fs::path absDestPath = fs::absolute(destPath);
                    if (absSrcPath == absDestPath) {
                        std::string operation = isMove ? "move" : "copy";
                        reportErrorCpMvRm("same_file", srcDir, srcFile, "", "", operation, 
                                  verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;  // Skip if source and destination are the same
                    }

                    // Check if the destination directory is valid
                    std::error_code ec;
                    if (!fs::exists(destDir, ec) || !fs::is_directory(destDir, ec)) {
                        std::string operation = isCopy ? "copying" : "moving";
                        reportErrorCpMvRm("invalid_dest", srcDir, srcFile, destDir, "Invalid destination", 
                                  operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;  // Skip if destination directory is invalid
                    }
                    
                    validDestinations.fetch_add(1, std::memory_order_acq_rel);
                    
                    if (!fs::exists(srcPath)) {
                        reportErrorCpMvRm("source_missing", srcDir, srcFile, "", "", "",
                                  verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;  // Skip if source file is missing
                    }

                    // Check if the destination file already exists
                    if (fs::exists(destPath)) {
                        if (overwriteExisting) {
                            // Attempt to overwrite the file if allowed
                            if (!fs::remove(destPath, ec)) {
                                reportErrorCpMvRm("overwrite_failed", "", "", destDirProcessed, ec.message(), "",
                                          verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                                continue;
                            }
                        } else {
                            std::string operation = isCopy ? "copying" : "moving";
                            reportErrorCpMvRm("file_exists", srcDir, srcFile, destDirProcessed, "", 
                                      operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                            continue;  // Skip if file exists and overwriting is not allowed
                        }
                    }

                    std::atomic<bool> success(false);

                    // Handle the multi-destination move operation
                    if (isMove && destDirs.size() > 1) {
                        success.store(performMultiDestMoveOperation(
                            srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                            completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                            operationSuccessful, batchInsertMessages, changeOwnership));
                        
                        if (success.load()) {
                            atLeastOneCopySucceeded.store(true);
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isMove) {
                        // Handle the single-destination move operation
                        success.store(performMoveOperation(
                            srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                            fileSize, completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                            operationSuccessful, batchInsertMessages, changeOwnership));
                        
                        if (success.load()) {
                            successfulOperations.fetch_add(1, std::memory_order_acq_rel);
                        }
                    } else if (isCopy) {
                        // Handle the copy operation
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

    // Prepare the list of ISO files to operate on
    std::vector<std::string> isoFilesToOperate;
    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string(), "cp_mv_rm");

        // Check if the ISO file exists, and if it does, add it to the list to operate on
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

    // Execute the operation on the valid ISO files
    executeOperation(isoFilesToOperate);

    // Insert any remaining verbose messages at the end
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
        operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
    }
}

