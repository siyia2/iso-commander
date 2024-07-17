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
    bool needsClrScrn = true;

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
			clearScrollBuffer();
			printIsoFileList(isFiltered ? filteredFiles : globalIsoFileList);
			std::cout << "\n\n\n";
		}
		
		// Move the cursor up 3 lines and clear them
        std::cout << "\033[1A\033[K";
        
		std::string prompt;
		if (isFiltered) {
			prompt = "\001\033[1;96m\002Filtered \001\033[1;92m\002ISO"
            "\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation 
            + "\001\033[1;94m\002 (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
        } else {
			prompt = "\001\033[1;92m\002\002ISO"
            "\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation 
            + "\001\033[1;94m\002 (e.g., 1-3,1 5), / ↵ filter, ↵ return:\001\033[0;1m\002 ";
		}

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());

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
                operationIsos.clear();
                operationErrors.clear();
                uniqueErrorMessages.clear();
                
                clear_history();
                historyPattern = true;
                loadHistory();
                // Move the cursor up 3 lines and clear them
                std::cout << "\033[1A\033[K";
                std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + " \001\033[1;94m\002list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";

                std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);
                
                if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                    historyPattern = false;
                    needsClrScrn = false;
                    clear_history();
                    break;
                }
                std::string inputSearch(searchQuery.get());
                std::cout << "\033[1m\n";
                
                if (strcmp(searchQuery.get(), "/") != 0) {
                    add_history(searchQuery.get());
                    saveHistory();
                }
                
                historyPattern = false;
                clear_history();
                
                auto newFilteredFiles = filterFiles(globalIsoFileList, inputSearch);
                
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
            processOperationInput(inputString, currentFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
            
            if (verbose) {
                verbose_cp_mv_rm(operationIsos, operationErrors, uniqueErrorMessages);
                needsClrScrn = true;
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
    // Tokenize the input string
    while (iss >> token) {
        
        // Check if the token starts wit zero and treat it as a non-existent index
        if (startsWithZero(token)) {
			uniqueErrorMessages.emplace("\033[1;91mInvalid index: '0'.\033[0;1m");
			continue;  
        }

        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
            continue;
        }

        // Process ranges specified with hyphens
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
                
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
                continue;
            }
            
            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[0;1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i); // Mark as processed
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    uniqueErrorMessages.emplace("\033[1;91mInvalid index '" + std::to_string(i) + "'.\033[0;1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num); // Mark index as processed
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid index: '" + std::to_string(num) + "'.\033[0;1m");
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

		unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
		std::vector<std::vector<int>> indexChunks;
		const size_t maxFilesPerChunk = 10;

		// Distribute files evenly among threads, but not exceeding maxFilesPerChunk
		size_t totalFiles = processedIndices.size();
		size_t filesPerThread = (totalFiles + numThreads - 1) / numThreads;
		size_t chunkSize = std::min(maxFilesPerChunk, filesPerThread);

		for (size_t i = 0; i < totalFiles; i += chunkSize) {
			auto chunkEnd = std::min(processedIndices.begin() + i + chunkSize, processedIndices.end());
			indexChunks.emplace_back(processedIndices.begin() + i, chunkEnd);
		}

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
            clearScrollBuffer();
            displaySelectedIsos();
            clear_history();
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
    std::cout << "\033[1m\n";
    
    std::atomic<size_t> totalTasks(static_cast<int>(processedIndices.size()));
    std::atomic<size_t> completedTasks(0);
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
    namespace fs = std::filesystem;
    
    // Get the real user ID and group ID (of the user who invoked sudo)
    uid_t real_uid;
    gid_t real_gid;
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");
    
    if (sudo_uid && sudo_gid) {
        real_uid = std::stoul(sudo_uid);
        real_gid = std::stoul(sudo_gid);
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name
    struct passwd *pw = getpwuid(real_uid);
    if (pw == nullptr) {
        std::cerr << "\nError getting user information: " << strerror(errno) << "\033[0;1m";
        return;
    }
    std::string real_username(pw->pw_name);

    // Vector to store ISO files to operate on
    std::vector<std::string> isoFilesToOperate;

    // Lambda function to change ownership
    auto changeOwnership = [&](const fs::path& path) {
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) == 0) {
            // Only change ownership if it's different from the current user
            if (file_stat.st_uid != real_uid || file_stat.st_gid != real_gid) {
                if (chown(path.c_str(), real_uid, real_gid) != 0) {
                    throw std::runtime_error("Failed to change ownership of '" + path.string() + "': " + std::string(strerror(errno)));
                }
            }
        } else {
            throw std::runtime_error("Failed to get file information for '" + path.string() + "': " + std::string(strerror(errno)));
        }
    };

    // Lambda function to execute the operation
    auto executeOperation = [&](const std::vector<std::string>& files) {
    for (const auto& operateIso : files) {
        fs::path srcPath(operateIso);
        fs::path destPath = fs::path(userDestDir) / srcPath.filename();

        std::error_code ec;
        try {
            if (isMove || isCopy) {
                // Create destination directory if it doesn't exist
                if (!fs::exists(userDestDir)) {
                    fs::create_directories(userDestDir, ec);
                    if (ec) {
                        throw std::runtime_error("Failed to create destination directory: " + ec.message());
                    }

                    // Change ownership of the created directory
                    changeOwnership(fs::path(userDestDir));
                }

                // Copy or move the file
                if (isCopy) {
                    fs::copy(srcPath, destPath, fs::copy_options::overwrite_existing, ec);
                } else if (isMove) {
                    fs::rename(srcPath, destPath, ec);
                }
            } else if (isDelete) {
                fs::remove(srcPath, ec);
            }

            if (ec) {
                throw std::runtime_error("Operation failed: " + ec.message());
            }

            // Change ownership of the copied/moved file
            if (!isDelete) {
                changeOwnership(destPath);
            }

                // Store operation success info
                std::string operationInfo = "\033[1m" + std::string(isDelete ? "Deleted" : (isCopy ? "Copied" : "Moved")) +
                    ": \033[1;92m'" + srcPath.string() + "'" + std::string(isDelete ? "\033[1m\033[0;1m." : "\033[1m\033[0;1m") +
                    (isDelete ? "" : " to \033[1;94m'" + destPath.string() + "'\033[0;1m.");
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationIsos.emplace(operationInfo);
                }
            } catch (const std::exception& e) {
                // Store operation error info
                std::string errorMessageInfo = "\033[1;91mError " + 
                    std::string(isDelete ? "deleting" : (isCopy ? "copying" : "moving")) +
                    ": \033[1;93m'" + srcPath.string() + "'\033[1;91m" +
                    (isDelete ? "" : " to '" + userDestDir + "'") +
                    ": " + e.what() + "\033[1;91m.\033[0;1m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationErrors.emplace(errorMessageInfo);
                }
            }
        }
    };
    // Iterate over each ISO file
    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        
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
                    isoPath.string() + "'\033[1;35m.\033[0;1m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    operationErrors.emplace(errorMessageInfo);
                }
            }
        } else {
            // Print message if file not found in cache
            std::string errorMessageInfo = "\033[1;93mFile not found in cache: \033[0;1m'" + 
                isoPath.string() + "'\033[1;93m.\033[0;1m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                operationErrors.emplace(errorMessageInfo);
            }
        }
    }

    // Execute the operation for all files
    executeOperation(isoFilesToOperate);
}
