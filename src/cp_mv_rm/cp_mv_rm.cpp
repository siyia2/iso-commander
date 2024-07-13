#include "../headers.h"
#include "../threadpool.h"


// For breaking mv&rm gracefully
bool mvDelBreak=false;

// General

// Function to check if a linux path is valid
bool isValidLinuxPathFormat(const std::string& path) {
    // Check if the path is empty or does not start with '/'
    if (path[0] != '/') {
        return false; // Linux paths must start with '/'
    }

    bool previousWasSlash = false;

    // Iterate through each character in the path
    for (char c : path) {
        if (c == '/') {
            if (previousWasSlash) {
                return false; // Consecutive slashes are not allowed
            }
            previousWasSlash = true;
        } else {
            previousWasSlash = false;

            // Check for invalid characters: '\0', '\n', '\r', '\t'
            if (c == '\0' || c == '\n' || c == '\r' || c == '\t' || c == ';') {
                return false; // Invalid characters in Linux path
            }
        }
    }

    return true; // Path format is valid
}


// Print verbose output for cp_mv_rm
void verbose_cp_mv_rm(std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages) {
    clearScrollBuffer();
    
    if (!operationIsos.empty()) {
        std::cout << "\n";
    }
    for (const auto& operationIso : operationIsos) {
        std::cout << operationIso << "\n\033[0;1m";
    }
    
    if (!operationErrors.empty()) {
        std::cout << "\n";
    }
    
    for (const auto& operationError : operationErrors) {
        std::cout << operationError << "\n\033[0;1m";
    }
    
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
    }
    for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
        std::cout << uniqueErrorMessage << "\033[0;1m\n";
    }
    
    // Clear the vector after each iteration
    operationIsos.clear();
    operationErrors.clear();
    uniqueErrorMessages.clear();
	verbose = false;
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Main function to select and operate on files by number
void select_and_operate_files_by_number(const std::string& operation) {
    std::set<std::string> operationIsos, operationErrors, uniqueErrorMessages;
    std::vector<std::string> isoFiles, filteredFiles;
    isoFiles.reserve(100);
    bool isFiltered = false;

    std::string operationColor = (operation == "rm") ? "\033[1;91m" : 
                                 (operation == "cp") ? "\033[1;92m" : "\033[1;93m";

    std::string process = operation;

    while (true) {
        removeNonExistentPathsFromCache();
        loadCache(isoFiles);

        if (isoFiles.empty()) {
            clearScrollBuffer();
            std::cout << "\n\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            break;
        }

        sortFilesCaseInsensitive(isoFiles);
        operationIsos.clear();
        operationErrors.clear();
        uniqueErrorMessages.clear();
        clearScrollBuffer();

        printIsoFileList(isFiltered ? filteredFiles : isoFiles);

        std::string prompt = std::string(isFiltered ? "\n\n\001\033[1;92m\002Filtered ISO" : "\n\n\001\033[1;92m\002ISO")
            + "\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation 
            + "\001\033[1;94m\002 (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());

        clearScrollBuffer();
        if (!inputString.empty() && inputString != "/") {
            std::cout << "\033[1mPlease wait...\033[1m\n";
        }

        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                continue;  // Return to the original list
            } else {
                return;  // Exit the function only if we're already on the original list
            }
        } else if (inputString == "/") {
            historyPattern = true;
            loadHistory();

            while (true) {
                clearScrollBuffer();
                std::string filterPrompt = "\n\001\033[1;92m\002Term(s)\001\033[1;94m\002 ↵ to filter \001" + operationColor + "\002" + operation 
                    + " \001\033[1;94m\002list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";
                std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);
                
                if (!searchQuery || searchQuery.get()[0] == '\0') {
                    historyPattern = false;
                    isFiltered = false;
                    break;
                }

                std::string inputSearch(searchQuery.get());
                clearScrollBuffer();
                std::cout << "\033[1mPlease wait...\033[1m\n";
                
                if (strcmp(searchQuery.get(), "/") != 0) {
                    add_history(searchQuery.get());
                    saveHistory();
                }
                
                filteredFiles = filterFiles(isoFiles, inputSearch);

                if (filteredFiles.empty()) {
                    clearScrollBuffer();
                    std::cout << "\n\033[1;91mNo matches found.\033[0;1m\n";
                    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                } else {
                    isFiltered = true;
                    break;
                }
            }
            clear_history();
        } else {
            std::vector<std::string>& currentFiles = isFiltered ? filteredFiles : isoFiles;
            
            processOperationInput(inputString, currentFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
            
            if (verbose) {
                verbose_cp_mv_rm(operationIsos, operationErrors, uniqueErrorMessages);
            }
            
            if (process !="cp" && isFiltered && mvDelBreak) {
				historyPattern = false;
				isFiltered =false;
			}

            if (currentFiles.empty()) {
                std::cout << "\n\033[1;93mNo ISO(s) available for " << operation << ".\033[0m\n\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return;
            }
        }
    }
}


// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages) {
    std::string userDestDir;
    std::istringstream iss(input);
    std::vector<int> processedIndices;
    processedIndices.reserve(maxThreads);
    
    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    
    std::string operationColor = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    std::string token;
    while (iss >> token) {
        if (startsWithZero(token) || std::count(token.begin(), token.end(), '-') > 1) {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
            continue;
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;
            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::exception& e) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            }
            
            if (start < 1 || end < 1 || static_cast<size_t>(std::max(start, end)) > isoFiles.size()) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((step > 0 && i <= end) || (step < 0 && i >= end)); i += step) {
                if (std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i);
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num);
            } else {
                uniqueErrorMessages.emplace("\033[1;91mInvalid index: '" + token + "'.\033[0;1m");
            }
        } else {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }
    
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0;1m\n";
        }
        if (!processedIndices.empty()) std::cout << "\n";
    }

    if (processedIndices.empty()) {
        clearScrollBuffer();
        mvDelBreak = false;
        std::cout << "\n\033[1;91mNo valid indices to be " << operationDescription << ".\033[1;91m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        clear_history();
        return;
    }

    unsigned int numThreads = std::min(static_cast<int>(processedIndices.size()), static_cast<int>(maxThreads));
    std::vector<std::vector<int>> indexChunks;
    const size_t chunkSize = (processedIndices.size() + numThreads - 1) / numThreads;
    for (size_t i = 0; i < processedIndices.size(); i += chunkSize) {
        indexChunks.emplace_back(processedIndices.begin() + i, std::min(processedIndices.begin() + i + chunkSize, processedIndices.end()));
    }

    auto displaySelectedIsos = [&]() {
        std::cout << "\n";
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1m -> " << isoDirectory << "/\033[1;95m" << isoFilename << "\033[0;1m\n";
            }
        }
    };

    if (!isDelete) {
        while (true) {
            clearScrollBuffer();
            displaySelectedIsos();
            historyPattern = false;
            loadHistory();
            userDestDir.clear();

            std::string prompt = "\n\001\033[1;92m\002Destination directory\001\033[1;94m\002 ↵ for selected ISO to be " + operationColor + operationDescription + "\001\033[1;94m\002 into, ↵ return:\n\001\033[0;1m\002";
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            std::string mainInputString(input.get());

            if (mainInputString.empty()) {
                mvDelBreak = false;
                clear_history();
                return;
            }

            if (isValidLinuxPathFormat(mainInputString)) {
                if (mainInputString.back() == '/') {
                    userDestDir = mainInputString;
                    add_history(input.get());
                    saveHistory();
                    clear_history();
                    break;
                } else {
                    std::cout << "\n\033[1;91mThe path must end with \033[0;1m'/'\033[1;91m.\033[0;1m\n";
                }
            } else {
                std::cout << "\n\033[1;91mInvalid paths and/or multiple paths are excluded from \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0;1m\n";
            }

            std::cout << "\n\033[1;32m↵ to try again...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    } else {
        clearScrollBuffer();
        displaySelectedIsos();

        std::string confirmation;
        std::cout << "\n\033[1;94mThe selected ISO will be \033[1;91m*PERMANENTLY DELETED FROM DISK*\033[1;94m. Proceed? (y/n):\033[0;1m ";
        std::getline(std::cin, confirmation);

        if (!(confirmation == "y" || confirmation == "Y")) {
            mvDelBreak = false;
            std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
        mvDelBreak = true;
    }

    clearScrollBuffer();
    std::cout << "\033[1mPlease wait...\033[1m\n";
    
    std::atomic<int> totalTasks(static_cast<int>(processedIndices.size()));
    std::atomic<int> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    int totalTasksValue = totalTasks.load();
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        for (const auto& index : chunk) {
            isoFilesInChunk.push_back(isoFiles[index - 1]);
        }

        {
            std::lock_guard<std::mutex> highLock(Mutex4High);
            futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), &isoFiles, &operationIsos, &operationErrors, &userDestDir, isMove, isCopy, isDelete, &completedTasks]() {
                handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, userDestDir, isMove, isCopy, isDelete);
                completedTasks.fetch_add(static_cast<int>(isoFilesInChunk.size()), std::memory_order_relaxed);
            }));
        }
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    progressThread.join();
    
    if (!isDelete) {
        promptFlag = false;
        maxDepth = 0;   
        manualRefreshCache(userDestDir);
    }
        
    clear_history();
    userDestDir.clear();
    maxDepth = -1;
}


// Function to check if directory exists
bool directoryExists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return info.st_mode & S_IFDIR; // Check if it's a directory
}


// Function to handle the deletion of ISO files in batches
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete) {
    // Get current user and group
    char* current_user = getlogin();
    if (current_user == nullptr) {
        std::cerr << "\nError getting current user: " << strerror(errno) << "\033[0;1m";
        return;
    }
    gid_t current_group = getegid();
    if (current_group == static_cast<unsigned int>(-1)) {
        std::cerr << "\n\033[1;91mError getting current group:\033[0;1m " << strerror(errno) << "\033[0;1m";
        return;
    }
    std::string user_str(current_user);
    std::string group_str = std::to_string(static_cast<unsigned int>(current_group));

    // Vector to store ISO files to operate on
    std::vector<std::string> isoFilesToOperate;

    // Lambda function to execute the operation command
    auto executeOperation = [&](const std::vector<std::string>& files) {
        std::string operationCommand;
        std::ostringstream oss;
        std::string errorMessageInfo;

        // Construct operation command based on operation type
        if (isMove || isCopy) {
            bool dirExists = directoryExists(userDestDir);
            if (!dirExists) {
                operationCommand = "mkdir -p " + shell_escape(userDestDir) + " && ";
                operationCommand += "chown " + user_str + ":" + group_str + " " + shell_escape(userDestDir) + " && ";
            }
        }

        if (isMove) {
            operationCommand += "mv -f ";
        } else if (isCopy) {
            operationCommand += "cp --reflink=auto -f ";
        } else if (isDelete) {
            operationCommand = "rm -f ";
        } else {
            std::cerr << "Invalid operation specified.\n";
            return;
        }

        // Append ISO files to the operation command
        for (const auto& operateIso : files) {
            operationCommand += shell_escape(operateIso) + " ";
        }

        // If not deleting, append destination directory
        if (!isDelete) {
            operationCommand += shell_escape(userDestDir) + " 2>/dev/null";
        }

        // Execute the operation command
        int result = system(operationCommand.c_str());

        // Handle operation result
        if (result == 0) {
            // Store operation success info
            for (const auto& iso : files) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);
                std::string destPath = userDestDir + isoFilename;
                oss.str("");
                if (!isDelete) {
                    oss << "\033[1m" << (isCopy ? "Copied" : "Moved") << ": \033[1;92m'"
                        << isoDirectory << "/" << isoFilename << "'\033[0;1m to \033[1;94m'" << destPath << "'\033[0;1m";
                } else {
                    oss << "\033[1m" << "Deleted" << ": \033[1;92m'"
                        << isoDirectory << "/" << isoFilename << "'\033[0;1m";
                }
                std::string operationInfo = oss.str();
                {
					std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationIsos.emplace(operationInfo);
				}

                // Change ownership of the copied/moved file
                if (!isDelete) {
                    std::string chownCommand = "chown " + user_str + ":" + group_str + " " + shell_escape(destPath);
                    system(chownCommand.c_str());
                }
            }
        } else {
            // Store operation error info
            for (const auto& iso : files) {
                auto [isoDir, isoFilename] = extractDirectoryAndFilename(iso);
                oss.str("");
                if (!isDelete) {
                    oss << "\033[1;91mError " << (isCopy ? "copying" : "moving") << ": \033[1;93m'"
                        << isoDir << "/" << isoFilename << "'\033[1;91m to '" << userDestDir << "'\033[0;1m";
                } else {
                    oss << "\033[1;91mError " << "deleting" << ": \033[1;93m'"
                        << isoDir << "/" << isoFilename << "'\033[0;1m";
                }
                errorMessageInfo = oss.str();
                {	std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationErrors.emplace(errorMessageInfo);
                }
            }
        }
    };
	std::string errorMessageInfo;
    // Iterate over each ISO file
    for (const auto& iso : isoFiles) {
        // Extract directory and filename from the ISO file path
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Check if ISO file is present in the copy list
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
        if (it != isoFilesCopy.end()) {
            // Check if the file exists
            if (fileExists(iso)) {
                // Add ISO file to the list of files to operate on
                isoFilesToOperate.push_back(iso);
            } else {
				// Print message if file not found
				errorMessageInfo = "\033[1;35mFile not found: \033[0;1m'" + isoDirectory + "/" + isoFilename + "'\033[1;95m.\033[0;1m";
				{	std::lock_guard<std::mutex> lowLock(Mutex4Low);
					operationErrors.emplace(errorMessageInfo);
				}
			}
        } else {
			// Print message if file not found in cache
			errorMessageInfo = "\033[1;93mFile not found in cache: \033[0;1m'" + isoDirectory + "/" + isoFilename + "'\033[1;93m.\033[0;1m";
			{	std::lock_guard<std::mutex> lowLock(Mutex4Low);
				operationErrors.emplace(errorMessageInfo);
			}
		}
    }

    // Execute the operation for all files in one go
    executeOperation(isoFilesToOperate);
}
