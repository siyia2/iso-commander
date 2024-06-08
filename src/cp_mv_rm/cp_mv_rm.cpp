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

// Main function to select and operate on files by number
void select_and_operate_files_by_number(const std::string& operation) {
	
	// Vector to store operation ISOs
	std::set<std::string> operationIsos;
	// Vector to store errors for operation ISOs
	std::set<std::string> operationErrors;
	// Vector to store ISO unique input errors
	std::set<std::string> uniqueErrorMessages;
	
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from the cache
    std::vector<std::string> isoFiles = loadCache();

    // If no ISO files are available, display a message and return
    if (isoFiles.empty()) {
        clearScrollBuffer();
        std::cout << "\033[1;93mISO Cache is empty. Please refresh it from the main Menu Options.\033[0;1m\n";
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Color code based on the operation
    std::string operationColor;
    if (operation == "rm") {
        operationColor = "\033[1;91m"; // Red for 'rm'
    } else if (operation == "cp") {
        operationColor = "\033[1;92m"; // Green for 'cp'
    } else {
        operationColor = "\033[1;93m"; // Yellow for other operations
    }

    std::string process;

    // Main loop for interacting with ISO files
    while (true) {
        clearScrollBuffer();

        // Display header message
        std::cout << "\033[1;93m! IF EXPECTED ISO FILE(S) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\033[0;1m\n";
        std::cout << "\033[94;1m 		CHANGES TO CACHED ISOS ARE REFLECTED AUTOMATICALLY\033[0;1m\n";

        // Reload ISO files (in case the cache was updated)
        removeNonExistentPathsFromCache();
        isoFiles = loadCache();

        std::string searchQuery;
        std::vector<std::string> filteredFiles = isoFiles;
        printIsoFileList(isoFiles);

        // Prompt user for input or filter
        char* input = readline(("\n\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + "\001\033[1;94m\002 (e.g., '1-3', '1 5'), / ↵ to filter, or ↵ to return:\001\033[0;1m\002 ").c_str());
        clearScrollBuffer();
        
        if (strcmp(input, "/") != 0 || (!(std::isspace(input[0]) || input[0] == '\0'))) {
			std::cout << "\033[1mPlease wait...\033[1m\n";
		}

        // Check if the user wants to return
        if (std::isspace(input[0]) || input[0] == '\0') {
			free(input);
            break;
        }
		mvDelBreak=false;
        if (strcmp(input, "/") == 0) {
			free(input);
			while (!mvDelBreak) {
            clearScrollBuffer();
			
			historyPattern = true;
			loadHistory();
			
			std::string prompt;
			
            // User pressed '/', start the filtering process
            prompt = "\n\001\033[1;92m\002SearchQuery\001\033[1;94m\002 ↵ to filter \001" + operationColor + "\002" + operation + "\001" + "\002\001\033[1;94m\002 list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0;1m\002";
            
            char* searchQuery = readline(prompt.c_str());
            clearScrollBuffer();
            
            if (searchQuery && searchQuery[0] != '\0') {
				std::cout << "\033[1mPlease wait...\033[1m\n";
				add_history(searchQuery); // Add the search query to the history
				saveHistory();
			}
            clear_history();
            

            // Store the original isoFiles vector
            std::vector<std::string> originalIsoFiles = isoFiles;
            
            if (!(std::isspace(searchQuery[0]) || searchQuery[0] == '\0')) {

            if (searchQuery != nullptr) {
                std::vector<std::string> filteredFiles = filterFiles(isoFiles, searchQuery);
                free(searchQuery);

                if (filteredFiles.empty()) {
					clearScrollBuffer();
                    std::cout << "\033[1;91mNo ISO(s) match the search query.\033[0;1m\n";
					std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
					std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                } else {
					while (!mvDelBreak) {
						clearScrollBuffer();
						sortFilesCaseInsensitive(filteredFiles);
						std::cout << "\033[1mFiltered results:\033[0;1m\n";
						printIsoFileList(filteredFiles); // Print the filtered list of ISO files

						// Prompt user for input again with the filtered list
						char* input = readline(("\n\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for " + operationColor + operation + "\001\033[1;94m\002 (e.g., '1-3', '1 5'), or ↵ to return:\001\033[0;1m\002 ").c_str());
                    
						// Check if the user wants to return
						if (std::isspace(input[0]) || input[0] == '\0') {
							free(input);
							historyPattern = false;
							break;
						}

						// Check if the user provided input
						if (input[0] != '\0' && (strcmp(input, "/") != 0)) {
							clearScrollBuffer();
							historyPattern = false;

							// Process the user input with the filtered list
							if (operation == "rm") {
								process = "rm";
								mvDelBreak=true;
								processOperationInput(input, filteredFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
							} else if (operation == "mv") {
								process = "mv";
								mvDelBreak=true;
								processOperationInput(input, filteredFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
							} else if (operation == "cp") {
								process = "cp";
								mvDelBreak=false;
								processOperationInput(input, filteredFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
								}
							}
							free(input);
						}
					}
				}
			} else {
					free(searchQuery);
					isoFiles = originalIsoFiles; // Revert to the original cache list
					historyPattern = false;
					break;
				}
			}
			
        } else {
            // Process the user input with the original list
            if (operation == "rm") {
                process = "rm";
                processOperationInput(input, isoFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
            } else if (operation == "mv") {
                process = "mv";
                processOperationInput(input, isoFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
            } else if (operation == "cp") {
                process = "cp";
                processOperationInput(input, isoFiles, process, operationIsos, operationErrors, uniqueErrorMessages);
            }
            free(input);
        }

        // If ISO files become empty after operation, display a message and return
        if (isoFiles.empty()) {
            std::cout << "\n";
            std::cout << "\033[1;93mNo ISO(s) available for " << operation << ".\033[0;1m\n";
            std::cout << "\n";
            std::cout << "↵ to continue...\n";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            break;
        }
    }
}


// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages) {
	
	// variable for user specified destination
	std::string userDestDir;

    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);
    
    std::mutex MutexForProcessedIndices;
    
    std::mutex MutexForUniqueErrors;

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    
    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    
    // Color code based on the operation
    std::string operationColor;
    if (process == "rm") {
        operationColor = "\033[1;91m"; // Red for 'rm'
    } else if (process == "cp") {
        operationColor = "\033[1;92m"; // Green for 'cp'
    } else {
        operationColor = "\033[1;93m"; // Yellow for other operations
    }

    std::string token;

    // Tokenize the input string
    while (iss >> token) {
        
        // Check if the token consists only of zeros and treat it as a non-existent index
        if (isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0;1m");
            }
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0;1m");
            }
        }
        
        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
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
				// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range& e) {
				// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0;1m");
                continue;
            }
            
            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
				// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0;1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
					// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> highLock(MutexForProcessedIndices);
                    processedIndices.push_back(i); // Mark as processed
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
					// Lock to ensure thread safety in a multi-threaded environment
					std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
                    invalidInput = true;
                    uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(i) + "' does not exist.\033[0;1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
				// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> lock(MutexForProcessedIndices);
                processedIndices.push_back(num); // Mark index as processed
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
				// Lock to ensure thread safety in a multi-threaded environment
                std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[0;1m");
            }
        } else {
			// Lock to ensure thread safety in a multi-threaded environment
            std::lock_guard<std::mutex> lock(MutexForUniqueErrors);
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }
    
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
    }

    // Display unique errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0;1m\n";
        }
    }

    if (invalidInput && !processedIndices.empty()) {
        std::cout << "\n";
    }

    if (processedIndices.empty()) {
		clearScrollBuffer();
		mvDelBreak=false;
        std::cout << "\n\033[1;91mNo valid input to be " << operationDescription << ".\033[1;91m\n";
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

    if (!isDelete) {
        while (true) {
			clearScrollBuffer();
			
			if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
			}
            
            for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
            std::cout << uniqueErrorMessage << "\033[0;1m\n";
			}
			
			if (processedIndices.empty()) {
			clearScrollBuffer();
			mvDelBreak=false;
			std::cout << "\n\033[1;91mNo valid input to be " << operationDescription << ".\033[1;91m\n";
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clear_history();
            return;
			}
				

            // Display selected operations
            std::cout << "\n\033[1;94mThe following ISO(s) will be " << operationColor + operationDescription << " \033[1;94mto ?\033[1;93m" << userDestDir << "\033[1;94m:\n\033[0;1m\n";
            for (const auto& chunk : indexChunks) {
                for (const auto& index : chunk) {
                    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                    std::cout << "\033[1m" << isoDirectory << "/\033[1;95m" << isoFilename << "\033[0;1m\n";
                }
            }
            
            // Load history from file
			loadHistory();

            // Ask for the destination directory
            std::string inputLine = readInputLine("\n\033[1;94mDestination directory ↵ for selected ISO file(s), or ↵ to cancel:\n\033[0;1m");

            // Check if the user canceled
            if (inputLine.empty()) {
				mvDelBreak=false;
				clear_history();
                return;
            }

            // Check if the entered path is valid
            if (isValidLinuxPathFormat(inputLine) && inputLine.back() == '/') {
                userDestDir = inputLine;
                    saveHistory();
                    clear_history();
                break;
            } else {
                std::cout << "\n\033[1;91mInvalid paths and/or multiple paths are excluded from \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to try again...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
    } else {
		clearScrollBuffer();	
		if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
		}
        for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
            std::cout << uniqueErrorMessage << "\033[0;1m\n";
		}
		
        std::cout << "\n\033[1;94mThe following ISO(s) will be "<< operationColor + operationDescription << "\033[1;94m:\n\033[0;1m\n";
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0;1m\n";
            }
        }

        if (!uniqueErrorMessages.empty() && indexChunks.empty()) {
			clearScrollBuffer();
			mvDelBreak=false;
            std::cout << "\n\033[1;91mNo valid input for deletion.\033[0;1m\n";
        } else {
            std::string confirmation;
            std::cout << "\n\033[1;94mDo you want to proceed? (y/n):\033[0;1m ";
            std::getline(std::cin, confirmation);

            if (!(confirmation == "y" || confirmation == "Y")) {
				mvDelBreak=false;
                std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
				std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
				std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return;
            }
        }
    }

   // auto start_time = std::chrono::high_resolution_clock::now();
	clearScrollBuffer();
    std::cout << "\033[1mPlease wait...\033[1m\n";

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        for (const auto& index : chunk) {
            isoFilesInChunk.push_back(isoFiles[index - 1]);
        }
        futures.emplace_back(pool.enqueue(handleIsoFileOperation,isoFilesInChunk,std::ref(isoFiles),std::ref(operationIsos),std::ref(operationErrors),userDestDir,isMove,isCopy,isDelete));
    }

    for (auto& future : futures) {
        future.wait();
    }
    
		if (!isDelete) {
			promptFlag = false;        
			manualRefreshCache(userDestDir);
		}

		clearScrollBuffer();
        
        if (!operationIsos.empty()) {
            std::cout << "\n";
        }
    
        // Print all moved files
        for (const auto& operationIso : operationIsos) {
            std::cout << operationIso << "\n\033[0;1m";
        }
        
        if (!operationErrors.empty()) {
            std::cout << "\n";
        }
        
        for (const auto& operationError : operationErrors) {
            std::cout << operationError << "\n\033[0;1m";
        }
        
        // Clear the vector after each iteration
        operationIsos.clear();
        operationErrors.clear();
        userDestDir.clear();
        clear_history();
        
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
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
            operationCommand += "mv ";
        } else if (isCopy) {
            operationCommand += "cp -f ";
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
            operationCommand += shell_escape(userDestDir);
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
                    std::lock_guard<std::mutex> lock(Mutex4Low);
                    operationIsos.insert(operationInfo);
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
                {
                    std::lock_guard<std::mutex> lock(Mutex4Low);
                    operationErrors.insert(errorMessageInfo);
                }
            }
        }
    };

    // Iterate over each ISO file
    for (const auto& iso : isoFiles) {
        // Extract directory and filename from the ISO file path
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Lock the low-level mutex to ensure thread safety
        std::lock_guard<std::mutex> lowLock(Mutex4Low);

        // Check if ISO file is present in the copy list
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
        if (it != isoFilesCopy.end()) {
            // Check if the file exists
            if (fileExists(iso)) {
                // Add ISO file to the list of files to operate on
                isoFilesToOperate.push_back(iso);
            } else {
                // Print message if file not found
                std::cout << "\033[1;35mFile not found: \033[0;1m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[0;1m\n";
            }
        } else {
            // Print message if file not found in cache
            std::cout << "\033[1;93mFile not found in cache: \033[0;1m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m.\033[0;1m\n";
        }
    }

    // Execute the operation for all files in one go
    executeOperation(isoFilesToOperate);
}


// RM

// Function to check if a file exists
bool fileExists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}
