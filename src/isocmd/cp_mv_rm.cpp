// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// For breaking mv&rm gracefully

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
            if (c == '\0' || c == '\n' || c == '\r' || c == '\t') {
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
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Main function to select and operate on files by number
void select_and_operate_files_by_number(const std::string& operation, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
    std::set<std::string> operationIsos, operationErrors, uniqueErrorMessages;
    std::vector<std::string> isoFiles, filteredFiles;
    isoFiles.reserve(100);
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool mvDelBreak = false;

    std::string operationColor = (operation == "rm") ? "\033[1;91m" :
                                 (operation == "cp") ? "\033[1;92m" : "\033[1;93m";

    std::string process = operation;

    while (true) {
		// Verbose output is to be disabled unless specified by progressbar function downstream
        verbose = false;
        removeNonExistentPathsFromCache();
        loadCache(isoFiles);

        if (needsClrScrn) clearScrollBuffer();

        if (isoFiles.empty()) {
            clearScrollBuffer();
            std::cout << "\n\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            break;
        }

        // Check if the loaded cache differs from the global list
        if (globalIsoFileList.size() != isoFiles.size() ||
            !std::equal(globalIsoFileList.begin(), globalIsoFileList.end(), isoFiles.begin())) {
            sortFilesCaseInsensitive(isoFiles);
            globalIsoFileList = isoFiles;
        } else {
            isoFiles = globalIsoFileList;
        }

        operationIsos.clear();
        operationErrors.clear();
        uniqueErrorMessages.clear();


		if (needsClrScrn) {
			printIsoFileList(isFiltered ? filteredFiles : globalIsoFileList);
			std::cout << "\n\n\n";
		}

		// Move the cursor up 3 lines and clear them
        std::cout << "\033[1A\033[K";

		std::string prompt;
		if (isFiltered) {
			prompt = "\001\033[1;96m\002Filtered \001\033[1;92m\002ISO"
            "\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation
            + "\001\033[1;94m\002 (e.g., 1-3,1 5), ~ ↵ (un)fold, / ↵ filter, ↵ return:\001\033[0;1m\002 ";
        } else {
			prompt = "\001\033[1;92m\002\002ISO"
            "\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation
            + "\001\033[1;94m\002 (e.g., 1-3,1 5), ~ ↵ (un)fold, / ↵ filter, ↵ return:\001\033[0;1m\002 ";
		}

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());
        
        if (inputString == "~") {
			if (toggleFullList) {
				toggleFullList = false;
			} else {
				toggleFullList = true;
			}
			
			needsClrScrn = true;
			continue;
		}

        if (!inputString.empty() && inputString != "/") {
            std::cout << "\033[1m\n";
        }

        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                continue;  // Return to the original list
            } else {
                return;  // Exit the function only if we're already on the original list
            }
        } else if (inputString == "/") {
            while (true) {
                // Verbose output is to be disabled unless specified by progressbar function downstream
				verbose = false;
                operationIsos.clear();
                operationErrors.clear();
                uniqueErrorMessages.clear();
                

                clear_history();
                historyPattern = true;
                loadHistory(historyPattern);
                // Move the cursor up 3 lines and clear them
                std::cout << "\033[1A\033[K";
                std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + " \001\033[1;94m\002list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";

                std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);

                if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                    historyPattern = false;
                    if (isFiltered) {
                        needsClrScrn = true;
                    } else {
                        needsClrScrn = false;
                    }
                    clear_history();
                    break;
                }
                std::string inputSearch(searchQuery.get());
                std::cout << "\033[1m\n";

                if (strcmp(searchQuery.get(), "/") != 0) {
                    add_history(searchQuery.get());
                    saveHistory(historyPattern);
                }

                historyPattern = false;
                clear_history();

                // Filter based on current active list instead of always using global list
                const std::vector<std::string>& sourceList = isFiltered ? filteredFiles : globalIsoFileList;
                auto newFilteredFiles = filterFiles(sourceList, inputSearch);
                sortFilesCaseInsensitive(newFilteredFiles);

                if (newFilteredFiles.size() == globalIsoFileList.size()) {
					isFiltered = false;
					break;
				}

                if (!newFilteredFiles.empty()) {
                    filteredFiles = std::move(newFilteredFiles);
                    needsClrScrn = true;
                    isFiltered = true;
                    break;
                }
                std::cout << "\033[1A\033[K";  // Clear the previous input line
            }
        } else {
            std::vector<std::string>& currentFiles = isFiltered ? filteredFiles : globalIsoFileList;
            needsClrScrn =true;
            processOperationInput(inputString, currentFiles, process, operationIsos, operationErrors, uniqueErrorMessages, promptFlag, maxDepth, mvDelBreak, historyPattern, verbose);

            if (verbose) {
				needsClrScrn = true;
                verbose_cp_mv_rm(operationIsos, operationErrors, uniqueErrorMessages);
            }

            if (process != "cp" && isFiltered && mvDelBreak) {
                historyPattern = false;
                clear_history();
                isFiltered = false;
                needsClrScrn =true;
            }

            if (currentFiles.empty()) {
                clearScrollBuffer();
                needsClrScrn = true;
                std::cout << "\n\033[1;93mNo ISO available for " << operation << ".\033[0m\n\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return;
            }
        }
    }
}


// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& mvDelBreak, bool& historyPattern, bool& verbose) {
    std::string userDestDir;
    std::vector<int> processedIndices;
    processedIndices.reserve(maxThreads);

    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");

    std::string operationColor = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    // Tokenize the input string
    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);
    
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


		unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
		std::vector<std::vector<int>> indexChunks;
		const size_t maxFilesPerChunk = 5;

		// Distribute files evenly among threads, but not exceeding maxFilesPerChunk
		size_t totalFiles = processedIndices.size();
		size_t filesPerThread = (totalFiles + numThreads - 1) / numThreads;
		size_t chunkSize = std::min(maxFilesPerChunk, filesPerThread);

		for (size_t i = 0; i < totalFiles; i += chunkSize) {
			auto chunkEnd = std::min(processedIndices.begin() + i + chunkSize, processedIndices.end());
			indexChunks.emplace_back(processedIndices.begin() + i, chunkEnd);
		}

    bool abortDel = false;
	std::string processedUserDestDir = promptCpMvRm(isoFiles, indexChunks, userDestDir, operationColor, operationDescription, mvDelBreak, historyPattern, isDelete, isCopy, abortDel);
	
	// Early exit if Deletion is aborted or userDestDir is empty for mv or cp
	if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
		return;
	}
	
    clearScrollBuffer();
    std::cout << "\033[1m\n";

    std::atomic<size_t> totalTasks(static_cast<int>(processedIndices.size()));
    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    int totalTasksValue = totalTasks.load();
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete), std::ref(verbose));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());
    std::mutex Mutex4Low; // Mutex for low-level processing

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(
            chunk.begin(),
            chunk.end(),
            std::back_inserter(isoFilesInChunk),
            [&isoFiles](size_t index) { return isoFiles[index - 1]; }
        );

        futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), &isoFiles, &operationIsos, &operationErrors, &userDestDir, isMove, isCopy, isDelete, &completedTasks, &Mutex4Low]() {
			handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, userDestDir, isMove, isCopy, isDelete, Mutex4Low);
			completedTasks.fetch_add(isoFilesInChunk.size(), std::memory_order_relaxed); // No need for static_cast to int
		}));
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    progressThread.join();

       promptFlag = false;
       maxDepth = 0;
       // Refresh cache for all destination directories if not a delete operation
       if (!isDelete) {
               manualRefreshCache(userDestDir, promptFlag, maxDepth, historyPattern);
       }

    clear_history();
    promptFlag = true;
    maxDepth = -1;
}


// Function to promtp for userDestDir and Delete confirmation
std::string promptCpMvRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& mvDelBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel) {
	
	    auto displaySelectedIsos = [&]() {
        std::cout << "\n";
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1m-> " << isoDirectory << "/\033[1;95m" << isoFilename << "\033[0;1m\n";
            }
        }
    };
	
	if (!isDelete) {
        while (true) {
			if (!isCopy) {
				mvDelBreak = true;
			}
            clearScrollBuffer();
            displaySelectedIsos();
            clear_history();
            historyPattern = false;
            loadHistory(historyPattern);
            userDestDir.clear();

            std::string prompt = "\n\001\033[1;92m\002DestinationDirs\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " + operationColor + operationDescription + "\001\033[1;94m\002 into (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), ↵ return:\n\001\033[0;1m\002";
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            std::string mainInputString(input.get());

            if (mainInputString.empty()) {
                mvDelBreak = false;
                userDestDir = "";
                clear_history();
                return userDestDir;
            }

            if (isValidLinuxPathFormat(mainInputString)) {
                bool slashBeforeSemicolon = mainInputString.find(";") == std::string::npos
                                            || mainInputString.find(";/") != std::string::npos
                                            || mainInputString.find("/;") != std::string::npos;
                bool semicolonSurroundedBySlash = true;

                // Check if semicolon is surrounded by slashes
                for (size_t i = 0; i < mainInputString.size(); ++i) {
                    if (mainInputString[i] == ';') {
                        if (i == 0 || i == mainInputString.size() - 1 || mainInputString[i - 1] != '/' || mainInputString[i + 1] != '/') {
                            semicolonSurroundedBySlash = false;
                            break;
                        }
                    }
                }

                if (mainInputString.back() == '/' && mainInputString.front() == '/' && slashBeforeSemicolon && semicolonSurroundedBySlash) {
                    userDestDir = mainInputString;
                    add_history(input.get());
                    saveHistory(historyPattern);
                    clear_history();
                    break;
                } else {
                    std::cout << "\n\033[1;91mThe paths must start and end with \033[0;1m'/'\033[1;91m.\033[0;1m\n";
                }
            } else {
                std::cout << "\n\033[1;91mInvalid paths are not allowed for \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0;1m\n";
            }

            std::cout << "\n\033[1;32m↵ to try again...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    } else {
        clearScrollBuffer();
        displaySelectedIsos();

        std::string confirmation;
        std::cout << "\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be \001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (y/n):\001\033[0;1m\002 ";
        std::getline(std::cin, confirmation);

        if (!(confirmation == "y" || confirmation == "Y")) {
            mvDelBreak = false;
            abortDel = true;
            userDestDir = "";
            std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return userDestDir;
        }
        mvDelBreak = true;
    }
    return userDestDir;
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
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::mutex& Mutex4Low) {
    namespace fs = std::filesystem;

    // Error flag to track overall operation success
    bool operationSuccessful = true;

    // Get the real user ID and group ID (of the user who invoked sudo)
    uid_t real_uid = 0;
    gid_t real_gid = 0;
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        try {
            real_uid = std::stoul(sudo_uid);
            real_gid = std::stoul(sudo_gid);
        } catch (const std::exception& e) {
            std::string errorMessage = "\033[1;91mError parsing SUDO_UID/SUDO_GID: " + std::string(e.what()) + "\033[0;1m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                operationErrors.emplace(errorMessage);
            }
            operationSuccessful = false;
            return;
        }
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name
    struct passwd *pw = getpwuid(real_uid);
    if (pw == nullptr) {
        std::string errorMessage = "\033[1;91mError getting user information: " + std::string(strerror(errno)) + "\033[0;1m";
        {
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            operationErrors.emplace(errorMessage);
        }
        operationSuccessful = false;
        return;
    }
    std::string real_username(pw->pw_name);

    // Vector to store ISO files to operate on
    std::vector<std::string> isoFilesToOperate;

    // Split userDestDir into multiple paths
    std::vector<std::string> destDirs;
    std::istringstream iss(userDestDir);
    std::string destDir;
    while (std::getline(iss, destDir, ';')) {
        destDirs.push_back(fs::path(destDir).string());
    }

    // Lambda function to change ownership
    auto changeOwnership = [&](const fs::path& path) -> bool {
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) == 0) {
            // Only change ownership if it's different from the current user
            if (file_stat.st_uid != real_uid || file_stat.st_gid != real_gid) {
                if (chown(path.c_str(), real_uid, real_gid) != 0) {
                    std::string errorMessage = "\033[1;91mFailed to change ownership of '" + path.string() + "': " + 
                                               std::string(strerror(errno)) + "\033[0;1m";
                    {
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        operationErrors.emplace(errorMessage);
                    }
                    return false;
                }
            }
        } else {
            std::string errorMessage = "\033[1;91mFailed to get file information for '" + path.string() + "': " + 
                                       std::string(strerror(errno)) + "\033[0;1m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                operationErrors.emplace(errorMessage);
            }
            return false;
        }
        return true;
    };

    // Lambda function to execute the operation
    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string());

            if (isDelete) {
                // Handle delete operation
                std::error_code ec;
                if (fs::remove(srcPath, ec)) {
                    std::string operationInfo = "\033[1mDeleted: \033[1;92m'" + srcDir + "/" + srcFile + "'\033[1m\033[0;1m.";
                    {
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        operationIsos.emplace(operationInfo);
                    }
                } else {
                    std::string errorMessageInfo = "\033[1;91mError deleting: \033[1;93m'" + srcDir + "/" + srcFile +
                        "'\033[1;91m: " + ec.message() + "\033[1;91m.\033[0;1m";
                    {
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        operationErrors.emplace(errorMessageInfo);
                    }
                    operationSuccessful = false;
                }
            } else {
                // Handle copy and move operations
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& destDir = destDirs[i];
                    fs::path destPath = fs::path(destDir) / srcPath.filename();
                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string());

                    std::error_code ec;
                    // Create destination directory if it doesn't exist
                    if (!fs::exists(destDir)) {
                        fs::create_directories(destDir, ec);
                        if (ec) {
                            std::string errorMessage = "\033[1;91mFailed to create destination directory: " + 
                                                       destDir + " : " + ec.message() + "\033[0;1m";
                            {
                                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                                operationErrors.emplace(errorMessage);
                            }
                            operationSuccessful = false;
                            continue;
                        }
                        
                        if (!changeOwnership(fs::path(destDir))) {
                            operationSuccessful = false;
                            continue;
                        }
                    }

                    if (isCopy) {
                        // Copy operation
                        fs::copy(srcPath, destPath, fs::copy_options::overwrite_existing, ec);
                    } else if (isMove) {
                        if (i < destDirs.size() - 1) {
                            // For all but the last destination, copy the file
                            fs::copy(srcPath, destPath, fs::copy_options::overwrite_existing, ec);
                        } else {
                            // For the last destination, move the file
                            fs::rename(srcPath, destPath, ec);
                        }
                    }

                    if (ec) {
                        std::string errorMessageInfo = "\033[1;91mError " +
                            std::string(isCopy ? "copying" : (i < destDirs.size() - 1 ? "moving" : "moving")) +
                            ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                            " to '" + destDirProcessed + "/': " + ec.message() + "\033[1;91m.\033[0;1m";
                        {
                            std::lock_guard<std::mutex> lowLock(Mutex4Low);
                            operationErrors.emplace(errorMessageInfo);
                        }
                        operationSuccessful = false;
                        continue;
                    }

                    if (!changeOwnership(destPath)) {
                        operationSuccessful = false;
                        continue;
                    }

                    // Store operation success info
                    std::string operationInfo = "\033[1m" +
                        std::string(isCopy ? "Copied" : (i < destDirs.size() - 1 ? "Moved" : "Moved")) +
                        ": \033[1;92m'" + srcDir + "/" + srcFile + "'\033[1m\033[0;1m" +
                        " to \033[1;94m'" + destDirProcessed + "/" + destFile + "'\033[0;1m.";
                    {
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        operationIsos.emplace(operationInfo);
                    }
                }
            }
        }
    };

    // Iterate over each ISO file
    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string());

        // Check if ISO file is present in the copy list
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
        if (it != isoFilesCopy.end()) {
            // Check if the file exists
            if (fs::exists(isoPath)) {
                // Add ISO file to the list of files to operate on
                isoFilesToOperate.push_back(iso);
            } else {
                // Print message if file not found
                std::string errorMessageInfo = "\033[1;35mFile not found: \033[0;1m'" +
                    isoDir + "/" + isoFile + "'\033[1;35m.\033[0;1m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationErrors.emplace(errorMessageInfo);
                }
                operationSuccessful = false;
            }
        } else {
            // Print message if file not found in cache
            std::string errorMessageInfo = "\033[1;93mFile not found in cache: \033[0;1m'" +
                isoDir + "/" + isoFile + "'\033[1;93m.\033[0;1m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                operationErrors.emplace(errorMessageInfo);
            }
            operationSuccessful = false;
        }
    }

    // Execute the operation for all files
    executeOperation(isoFilesToOperate);
}
