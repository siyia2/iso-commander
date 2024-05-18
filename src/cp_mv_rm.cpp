#include "headers.h"


// Vector to store operation ISOs
std::vector<std::string> operationIsos;
// Vector to store errors for operation ISOs
std::vector<std::string> operationErrors;


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

// Main function to select and operate on files by number
void select_and_operate_files_by_number(const std::string& operation) {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from the cache
    std::vector<std::string> isoFiles = loadCache();

    // If no ISO files are available, display a message and return
    if (isoFiles.empty()) {
        std::system("clear");
        std::cout << "\033[1;93mNo ISO(s) available for " << operation << ".\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32m↵ to continue...\033[0m\033[1m";
        std::cin.get();
        return;
    }

    // Filter ISO files to keep only those that end with ".iso"
    isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
        return !ends_with_iso(iso);
    }), isoFiles.end());

    // Color code based on the operation
    std::string operationColor;
    if (operation == "rm") {
        operationColor = "\033[1;91m"; // Red for 'rm'
    } else if (operation == "cp") {
        operationColor = "\033[1;92m"; // Green for 'cp'
    } else {
        operationColor = "\033[1;93m"; // Yellow for other operations
    }

    std::unordered_set<std::string> operationSet;
    std::string process;

    // Main loop for interacting with ISO files
    while (true) {
        clearScrollBuffer();
        std::system("clear");

        // Display header message
        std::cout << "\033[1;93m! IF EXPECTED ISO FILE(S) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\033[0m\033[1m" << std::endl;
        std::cout << "\033[94;1m 		CHANGES TO CACHED ISOS ARE REFLECTED AUTOMATICALLY\n\033[0m\033[1m" << std::endl;

        // Reload ISO files (in case the cache was updated)
        removeNonExistentPathsFromCache();
        isoFiles = loadCache();
        isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
            return !ends_with_iso(iso);
        }), isoFiles.end());

        std::string searchQuery;
        std::vector<std::string> filteredIsoFiles = isoFiles;
        printIsoFileList(isoFiles);

        // Prompt user for input or filter
        char* input = readline(("\n\033[1;94mISO(s) ↵ for " + operationColor + operation + "\033[1;94m (e.g., '1-3', '1 5'), / ↵ to filter, or ↵ to return:\033[0m\033[1m ").c_str());
        clearScrollBuffer();
        std::system("clear");

        // Check if the user wants to return
        if (std::isspace(input[0]) || input[0] == '\0') {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        }

        if (strcmp(input, "/") == 0) {
            clearScrollBuffer();
            std::system("clear");

            std::cout << " " << std::endl;

            printIsoFileList(isoFiles);

            // User pressed '/', start the filtering process
            std::cout << "\n\033[1;92mSearchQuery\033[1;94m ↵ or ↵ to return (case-insensitive): \033[0m\033[1m";
            std::getline(std::cin, searchQuery);
            clearScrollBuffer();
			std::system("clear");
			std::cout << "\033[1mPlease wait...\033[1m" << std::endl;

            // Store the original isoFiles vector
            std::vector<std::string> originalIsoFiles = isoFiles;

            if (!searchQuery.empty()) {
                filteredIsoFiles = filterIsoFiles(isoFiles, searchQuery);

                if (filteredIsoFiles.empty()) {
					clearScrollBuffer();
					std::system("clear");
                    std::cout << "\033[1;93mNo files match the search query.\033[0m\033[1m\n";
					std::cout << "\n\033[1;32m↵ to continue...\033[0m\033[1m";
					std::cin.get();
                } else {
                    clearScrollBuffer();
                    std::system("clear");
                    std::cout << " " << std::endl;
                    printIsoFileList(filteredIsoFiles); // Print the filtered list of ISO files

                    // Prompt user for input again with the filtered list
                    char* input = readline(("\n\033[1;94mISO(s) ↵ for " + operationColor + operation + "\033[1;94m (e.g., '1-3', '1 5'), or ↵ to return:\033[0m\033[1m ").c_str());

                    // Check if the user provided input
                    if (input[0] != '\0' && (strcmp(input, "/") != 0)) {
                        clearScrollBuffer();
                        std::system("clear");

                        // Process the user input with the filtered list
                        if (operation == "rm") {
                            process = "rm";
                            processOperationInput(input, filteredIsoFiles, operationSet, process);
                        } else if (operation == "mv") {
                            process = "mv";
                            processOperationInput(input, filteredIsoFiles, operationSet, process);
                        } else if (operation == "cp") {
                            process = "cp";
                            processOperationInput(input, filteredIsoFiles, operationSet, process);
                        }
                    }
                }
            } else {
                isoFiles = originalIsoFiles; // Revert to the original cache list
            }
        } else {
            // Process the user input with the original list
            if (operation == "rm") {
                process = "rm";
                processOperationInput(input, isoFiles, operationSet, process);
            } else if (operation == "mv") {
                process = "mv";
                processOperationInput(input, isoFiles, operationSet, process);
            } else if (operation == "cp") {
                process = "cp";
                processOperationInput(input, isoFiles, operationSet, process);
            }
        }

        // If ISO files become empty after operation, display a message and return
        if (isoFiles.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mNo ISO(s) available for " << operation << ".\033[0m\033[1m" << std::endl;
            std::cout << " " << std::endl;
            std::cout << "↵ to continue..." << std::endl;
            std::cin.get();
            break;
        }
    }
}


// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& operationSet, const std::string& process) {
	
	// variable for user specified destination
	std::string userDestDir;

	// Vector to store selected ISOs for display
	std::vector<std::string> selectedIsos;

	// Load history from file
	loadHistory();
	     
    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    std::vector<int> validIndices;     // Vector to keep track of valid indices
    
    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "PERMANENTLY DELETED" : (isMove ? "MOVED" : "COPIED");

    std::string token;

    // Tokenize the input string
    while (iss >> token) {
        
        // Check if the token consists only of zeros and treat it as a non-existent index
        if (isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0m\033[1m");
            }
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0m\033[1m");
            }
        }
        
        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
            continue;
        }

        // Process ranges specified with hyphens
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
                // Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> highLock(Mutex4High);
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }
            
            // Lock to ensure thread safety in a multi-threaded environment
            std::lock_guard<std::mutex> highLock(Mutex4High);

            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i); // Mark as processed
                    validIndices.push_back(i);
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    invalidInput = true;
                    uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(i) + "' does not exist.\033[0m\033[1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num); // Mark index as processed
                validIndices.push_back(num);
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[0m\033[1m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
        }
    }

    // Display unique errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0m\033[1m" << std::endl;
        }
    }

    if (invalidInput && !validIndices.empty()) {
        std::cout << " " << std::endl;
    }

    if (validIndices.empty()) {
        std::cout << "\033[1;91mNo valid selections to be " << operationDescription << ".\033[1;91m" << std::endl;
        std::cout << "\n\033[1;32m↵ to continue...\033[0m\033[1m";
        std::cin.get();
        clear_history();
        return;
    }

    unsigned int numThreads = std::min(static_cast<int>(validIndices.size()), static_cast<int>(maxThreads));
    std::vector<std::vector<int>> indexChunks;
    const size_t chunkSize = (validIndices.size() + numThreads - 1) / numThreads;
    for (size_t i = 0; i < validIndices.size(); i += chunkSize) {
        indexChunks.emplace_back(validIndices.begin() + i, std::min(validIndices.begin() + i + chunkSize, validIndices.end()));
    }

    if (!isDelete) {
        while (true) {
			clearScrollBuffer();
            std::system("clear");
            
            for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
            std::cout << uniqueErrorMessage << std::endl;
			}
			if (!uniqueErrorMessages.empty()) {
            std::cout << " " << std::endl;
			}
			if (validIndices.empty()) {
			std::cout << "\033[1;91mNo valid selections to be " << operationDescription << ".\033[1;91m" << std::endl;
			std::cout << "\n\033[1;32m↵ to continue...\033[0m\033[1m";
            std::cin.get();
            clear_history();
            return;
			}
				

            // Display selected operations
            std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*" << operationDescription << "* \033[1;94mto ?\033[1;93m" << userDestDir << "\033[1;94m:\033[0m\033[1m" << std::endl;
            std::cout << " " << std::endl;
            for (const auto& chunk : indexChunks) {
                for (const auto& index : chunk) {
                    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                    std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m" << std::endl;
                }
            }

            // Ask for the destination directory
            std::string inputLine = readInputLine("\n\033[1;94mDestination directory ↵ for selected ISO file(s), or ↵ to cancel:\n\033[0m\033[1m");

            // Check if the user canceled
            if (inputLine.empty()) {
				clear_history();
                return;
            }

            // Check if the entered path is valid
            if (isValidLinuxPathFormat(inputLine)) {
                userDestDir = inputLine;
                    saveHistory();
                break;
            } else {
                std::cout << "\n\033[1;91mInvalid paths and/or multiple paths are excluded from \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0m\033[1m" << std::endl;
                std::cout << "\n\033[1;32mPress Enter to try again...\033[0m\033[1m";
                std::cin.get();
            }
        }
    } else {
        std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*" << operationDescription << "*\033[1;94m:\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m" << std::endl;
            }
        }

        if (!uniqueErrorMessages.empty() && indexChunks.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;91mNo valid selection(s) for deletion.\033[0m\033[1m" << std::endl;
        } else {
            std::string confirmation;
            std::cout << " " << std::endl;
            std::cout << "\033[1;94mDo you want to proceed? (y/n):\033[0m\033[1m ";
            std::getline(std::cin, confirmation);

            if (!(confirmation == "y" || confirmation == "Y")) {
                std::cout << " " << std::endl;
                std::cout << "\033[1;93mDelete operation aborted by user.\033[0m\033[1m" << std::endl;
				std::cout << "\n\033[1;32m↵ to continue...\033[0m\033[1m";
				std::cin.get();
                return;
            }
        }
    }

    auto start_time = std::chrono::high_resolution_clock::now();
	
	clearScrollBuffer();
    std::system("clear");
    std::cout << "\033[1mPlease wait...\033[1m" << std::endl;

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    std::lock_guard<std::mutex> highLock(Mutex4High);

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        for (const auto& index : chunk) {
            isoFilesInChunk.push_back(isoFiles[index - 1]);
        }
        if (isDelete) {
            futures.emplace_back(pool.enqueue(handleIsoFileOperation, isoFilesInChunk, std::ref(isoFiles), userDestDir, isMove, isCopy, isDelete));
        } else if (isMove) {
             futures.emplace_back(pool.enqueue(handleIsoFileOperation, isoFilesInChunk, std::ref(isoFiles), userDestDir, isMove, isCopy, isDelete));
        } else {
             futures.emplace_back(pool.enqueue(handleIsoFileOperation, isoFilesInChunk, std::ref(isoFiles), userDestDir, isMove, isCopy, isDelete));
        }
    }

    for (auto& future : futures) {
        future.wait();
    }
    
		if (!isDelete) {
			promptFlag = false;        
			manualRefreshCache(userDestDir);
		}

		std::system("clear");
        
        if (!operationIsos.empty()) {
            std::cout << " " << std::endl;
        }
    
        // Print all moved files
        for (const auto& operationIso : operationIsos) {
            std::cout << operationIso << std::endl;
        }
        
        if (!operationErrors.empty()) {
            std::cout << " " << std::endl;
        }
        
        for (const auto& operationError : operationErrors) {
            std::cout << operationError << std::endl;
        }
        
        // Clear the vector after each iteration
        operationIsos.clear();
        operationErrors.clear();
        userDestDir.clear();
        
        clear_history();

        // Stop the timer after completing all deletion tasks
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32m↵ to continue...\033[0m\033[1m";
        std::cin.get();
        
}

// Function to handle the deletion of ISO files in batches
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete) {
    // Lock the low-level mutex to ensure thread safety
    std::lock_guard<std::mutex> lowLock(Mutex4Low);

    // Determine batch size based on the number of ISO files and maxThreads
    size_t batchSize = 1;
    if (isoFiles.size() > 100000 && isoFiles.size() > maxThreads) {
        batchSize = 100;
    } else if (isoFiles.size() > 10000 && isoFiles.size() > maxThreads) {
        batchSize = 50;
    } else if (isoFiles.size() > 1000 && isoFiles.size() > maxThreads) {
        batchSize = 25;
    } else if (isoFiles.size() > 100 && isoFiles.size() > maxThreads) {
        batchSize = 10;
    } else if (isoFiles.size() > 50 && isoFiles.size() > maxThreads) {
        batchSize = 5;
    } else if (isoFiles.size() > maxThreads) {
        batchSize = 2;
    }

    // Vector to store ISO files to operate on
    std::vector<std::string> isoFilesToOperate;

    auto pclose_deleter = [](FILE* fp) { return pclose(fp); };

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

                // Get current user and group
                char* current_user = getlogin();
                if (current_user == nullptr) {
                    std::cerr << "Error getting current user: " << strerror(errno) << std::endl;
                    return;
                }
                gid_t current_group = getegid();
                if (current_group == static_cast<unsigned int>(-1)) {
                    std::cerr << "\033[1;91mError getting current group:\033[0m\033[1m " << strerror(errno) << std::endl;
                    return;
                }
                std::string user_str(current_user);
                std::string group_str = std::to_string(static_cast<unsigned int>(current_group));

                // Execute operations in batches
                if (isoFilesToOperate.size() == batchSize || &iso == &isoFiles.back()) {
                    std::string operationCommand;
                    std::ostringstream oss;
                    std::string errorMessageInfo;

                    // Construct operation command based on operation type
                    if (isMove) {
                        operationCommand = "sudo mkdir -p " + shell_escape(userDestDir) + " && ";
                        operationCommand += "sudo mv ";
                    } else if (isCopy) {
                        operationCommand = "sudo mkdir -p " + shell_escape(userDestDir) + " && ";
                        operationCommand += "sudo cp -f ";
                    } else if (isDelete) {
                        operationCommand = "sudo rm -f ";
                    } else {
                        std::cerr << "Invalid operation specified." << std::endl;
                        return;
                    }

                    // Append ISO files to the operation command
                    for (const auto& operateIso : isoFilesToOperate) {
                        operationCommand += shell_escape(operateIso) + " ";
                    }

                    // If not deleting, change ownership of destination directory
                    if (!isDelete) {
                        operationCommand += shell_escape(userDestDir) + " && sudo chown -R " + user_str + ":" + group_str + " " + shell_escape(userDestDir);
                    }

                    // Execute the operation command asynchronously
                    std::future<int> operationFuture = std::async(std::launch::async, [&operationCommand, &pclose_deleter]() {
                        std::array<char, 128> buffer;
                        std::string result;
                        std::unique_ptr<FILE, decltype(pclose_deleter)> pipe(popen(operationCommand.c_str(), "r"), pclose_deleter);
                        if (!pipe) {
                            throw std::runtime_error("popen() failed!");
                        }
                        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                            result += buffer.data();
                        }
                        return pipe.get() ? 0 : -1;
                    });

                    int result = operationFuture.get();

                    // Handle operation result
                    if (result == 0) {
                        // Store operation success info
                        for (const auto& iso : isoFilesToOperate) {
                            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);
                            oss.str("");
                            if (!isDelete) {
                                oss << "\033[1m" << (isCopy ? "Copied" : (isMove ? "Moved" : "Deleted")) << ": \033[1;92m'"
                                    << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m to \033[1;94m'" << userDestDir << "'\033[0m\033[1m";
                            } else {
                                oss << "\033[1m" << (isCopy ? "Copied" : (isMove ? "Moved" : "Deleted")) << ": \033[1;92m'"
                                    << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m";
                            }
                            std::string operationInfo = oss.str();
                            operationIsos.push_back(operationInfo);
                        }
                    } else {
                        // Store operation error info
                        for (const auto& iso : isoFilesToOperate) {
                            auto [isoDir, isoFilename] = extractDirectoryAndFilename(iso);
                            oss.str("");
                            if (!isDelete) {
                                oss << "\033[1;91mError " << (isCopy ? "copying" : (isMove ? "moving" : "deleting")) << ": \033[1;93m'"
                                    << isoDir << "/" << isoFilename << "'\033[1;91m to '" << userDestDir << "'\033[0m\033[1m";
                            } else {
                                oss << "\033[1;91mError " << (isCopy ? "copying" : (isMove ? "moving" : "deleting")) << ": \033[1;93m'"
                                    << isoDir << "/" << isoFilename << "'\033[0m\033[1m";
                            }
                            errorMessageInfo = oss.str();
                            operationErrors.push_back(errorMessageInfo);
                        }
                    }

                    // Clear the list of files to operate on
                    isoFilesToOperate.clear();
                }
            } else {
                // Print message if file not found
                std::cout << "\033[1;35mFile not found: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[0m\033[1m" << std::endl;
            }
        } else {
            // Print message if file not found in cache
            std::cout << "\033[1;93mFile not found in cache: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m.\033[0m\033[1m" << std::endl;
        }
    }
}


// RM

// Function to check if a file exists
bool fileExists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}
