#include "headers.h"


// Vector to store deleted ISOs
std::vector<std::string> deletedIsos;
// Vector to store errors for deleted ISOs
std::vector<std::string> deletedErrors;

// Vector to store operation ISOs
std::vector<std::string> operationIsos;
// Vector to store errors for operation ISOs
std::vector<std::string> operationErrors;


// General

// Function to check if a linux path is valid
bool isValidLinuxPathFormat(const std::string& path) {
    // Check if the path is empty or does not start with '/'
    if (path.empty() || path[0] != '/') {
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
    // Remove nonexistent paths from cache
    removeNonExistentPathsFromCache();
    
    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // If no ISO files are available, display a message and return
    if (isoFiles.empty()) {
        clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93mNo ISO(s) available for " << operation << ".\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
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
        std::cout << "\033[94;1m         	CHANGES TO CACHED ISOS ARE REFLECTED AUTOMATICALLY\n\033[0m\033[1m" << std::endl;

        // Reload ISO files (in case cache was updated)
        removeNonExistentPathsFromCache();
        isoFiles = loadCache();
        isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
            return !ends_with_iso(iso);
        }), isoFiles.end());

        // Print the list of ISO files
        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Get user input for ISO file selection
        char* input = readline(("\033[1;94mISO(s) ↵ for " + operationColor + operation + "\033[1;94m (e.g., '1-3', '1 5'), or press ↵ to return:\033[0m\033[1m ").c_str());
        std::system("clear");

        // Check if input is empty or whitespace (to return to main menu)
        if (std::isspace(input[0]) || input[0] == '\0') {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        } else if (operation == "rm") {
            // Process delete operation
            clearScrollBuffer();
            std::system("clear");
            std::unordered_set<std::string> deletedSet;
            processDeleteInput(input, isoFiles, deletedSet);
        } else if (operation == "mv") {
            // Process move operation
            clearScrollBuffer();
            std::system("clear");
            process = "mv";
            processOperationInput(input, isoFiles, operationSet, process);
        } else if (operation == "cp") {
            // Process copy operation
            clearScrollBuffer();
            std::system("clear");
            process = "cp";
            processOperationInput(input, isoFiles, operationSet, process);
        }

        // If ISO files become empty after operation, display a message and return
        if (isoFiles.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mNo ISO(s) available for " << operation << ".\033[0m\033[1m" << std::endl;
            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue..." << std::endl;
            std::cin.get();
            break;
        }

        // Additional message for delete operation to continue
        if (operation == "rm") {
            std::cout << " " << std::endl;
            std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
            std::cin.get();
        }
    }
}


// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& operationSet, const std::string& process) {
	// variable for user specified destination
	std::string userDestDir;

	// Vector to store selected ISOs for display
	std::vector<std::string> selectedIsos;

	// Clear the userDestDir variable
	userDestDir.clear();

	// Load history from file
	loadHistory();
	
	bool isCopy;
     
    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    std::vector<int> validIndices;     // Vector to keep track of valid indices

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

    // Display additional information if there are invalid inputs and some valid indices
    if (invalidInput && !validIndices.empty()) {
        std::cout << " " << std::endl;
    }
	// Detect and use the minimum of available threads and validIndices to ensure efficient parallelism
	unsigned int numThreads = std::min(static_cast<int>(validIndices.size()), static_cast<int>(maxThreads));

    // Batch the valid indices into chunks based on numThreads
    std::vector<std::vector<int>> indexChunks;
    const size_t chunkSize = (validIndices.size() + numThreads - 1) / numThreads;
    for (size_t i = 0; i < validIndices.size(); i += chunkSize) {
        indexChunks.emplace_back(validIndices.begin() + i, std::min(validIndices.begin() + i + chunkSize, validIndices.end()));
    }
	
        bool isMove = (process == "mv");
        std::string operationDescription = (isMove) ? "MOVED" : "COPIED";

        while (true) {
            std::system("clear");
            
            for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
            std::cout << uniqueErrorMessage << std::endl;
			}
			if (!uniqueErrorMessages.empty()) {
            std::cout << " " << std::endl;
			}
			if (validIndices.empty()) {
			std::cout << "\033[1;91mNo valid selections to be " << operationDescription << ".\033[1;91m" << std::endl;
			std::cout << "\n\033[1;32mPress Enter to continue...\033[0m\033[1m";
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
            std::string inputLine = readInputLine("\n\033[1;94mDestination directory ↵ for selected ISO file(s), or press ↵ to cancel:\n\033[0m\033[1m");

            // Check if the user canceled
            if (inputLine.empty()) {
				clear_history();
                return;
            }

            // Check if the entered path is valid
            if (isValidLinuxPathFormat(inputLine)) {
                userDestDir = inputLine;
                if (!inputLine.empty()) {
                    saveHistory();
                }
                break;
            } else {
                std::cout << "\n\033[1;91mInvalid paths and/or multiple paths are excluded from \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0m\033[1m" << std::endl;
                std::cout << "\n\033[1;32mPress Enter to try again...\033[0m\033[1m";
                std::cin.get();
            }
        }

        // Start the timer after user confirmation
        auto start_time = std::chrono::high_resolution_clock::now();

        std::system("clear");
        std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
        // Create a thread pool with a optimal number of threads
        ThreadPool pool(numThreads);
        // Use std::async to launch asynchronous tasks
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);

        // Lock to ensure thread safety in a multi-threaded environment
        std::lock_guard<std::mutex> highLock(Mutex4High);

        for (const auto& chunk : indexChunks) {
            std::vector<std::string> isoFilesInChunk;
            for (const auto& index : chunk) {
                isoFilesInChunk.push_back(isoFiles[index - 1]);
            }
            if (isMove) {
				isCopy = false;
                futures.emplace_back(pool.enqueue(handleIsoFileOperation, isoFilesInChunk, std::ref(isoFiles), userDestDir, isCopy));
            } else {
				isCopy = true;
                futures.emplace_back(pool.enqueue(handleIsoFileOperation, isoFilesInChunk, std::ref(isoFiles), userDestDir, isCopy));
            }
        }

        // Wait for all asynchronous tasks to complete
        for (auto& future : futures) {
            future.wait();
        }


		// Print all operated files and errors
		clearScrollBuffer();
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
        
		promptFlag = false;        
        manualRefreshCache(userDestDir);
        
        clear_history();

        // Stop the timer after completing all deletion tasks
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;
        
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
        
}

// Function to handle the deletion of ISO files in batches
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, const std::string& userDestDir, bool isCopy) {
    // Lock the global mutex for synchronization
    std::lock_guard<std::mutex> lowLock(Mutex4Low);
    
    // Determine batch size based on the number of isoDirs
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

    // Declare the isoFilesToOperate vector
    std::vector<std::string> isoFilesToOperate;
    
    // Process each ISO file
    for (const auto& iso : isoFiles) {
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Check if the ISO file is in the cache
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);

        if (it != isoFilesCopy.end()) {
            // Check if the file exists before attempting the operation
            if (fileExists(iso)) {
                // Add the ISO file to the operation batch
                isoFilesToOperate.push_back(iso);

                // If the operation batch reaches the batch size, or no more ISO files to process
                if (isoFilesToOperate.size() == batchSize || &iso == &isoFiles.back()) {
                    // Construct the operation command for the entire batch
                    std::string operationCommand = "mkdir -p " + shell_escape(userDestDir) + " && ";
                    operationCommand += (isCopy ? "cp -f " : "mv ");
                    for (const auto& operateIso : isoFilesToOperate) {
                        operationCommand += shell_escape(operateIso) + " " + shell_escape(userDestDir) + " ";
                    }
                    operationCommand += "> /dev/null 2>&1";
                    
                    // Execute the operation command
                    int result = std::system(operationCommand.c_str());
                    
                    std::ostringstream oss;
					std::string errorMessageInfo;

                    // Process operation results
                    if (result == 0) {
                        for (const auto& iso : isoFilesToOperate) {
							auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);
							oss.str("");
							oss << "\033[1m" << (isCopy ? "Copied" : "Moved") << ": \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m to \033[1;94m'" << userDestDir << "'\033[0m\033[1m";
							std::string operationInfo = oss.str();
							operationIsos.push_back(operationInfo);
						}
                    } else {
                        for (const auto& iso : isoFilesToOperate) {
							auto [isoDir, isoFilename] = extractDirectoryAndFilename(iso);
							oss.str("");
							oss << "\033[1;91mError " << (isCopy ? "copying" : "moving") << ": \033[1;93m'" << isoDir << "/" << isoFilename << "'\033[1;91m to '" << userDestDir << "'\033[0m\033[1m";
							errorMessageInfo = oss.str();
							operationErrors.push_back(errorMessageInfo);
						}
                    }

                    // Clear the operation batch for the next set
                    isoFilesToOperate.clear();
                }
            } else {
                std::cout << "\033[1;35mFile not found: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[0m\033[1m" << std::endl;
            }
        } else {
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


// Function to handle the deletion of ISO files in batches
void handleDeleteIsoFile(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& deletedSet) {
    // Lock the global mutex for synchronization
    std::lock_guard<std::mutex> lowLock(Mutex4Low);
    
    // Determine batch size based on the number of isoDirs
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
    
    // Track ISO files to delete in the current batch
    std::vector<std::string> isoFilesToDelete;

    // Process each ISO file
    for (const auto& iso : isoFiles) {
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Check if the ISO file is in the cache
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);

        if (it != isoFilesCopy.end()) {
            // Check if the file exists before attempting to delete
            if (fileExists(iso)) {
                // Add the ISO file to the deletion batch
                isoFilesToDelete.push_back(iso);

                // If the deletion batch reaches the batch size, or no more ISO files to process
                if (isoFilesToDelete.size() == batchSize || &iso == &isoFiles.back()) {
                    // Construct the delete command for the entire batch
                    std::string deleteCommand = "sudo rm -f ";
                    for (const auto& deleteIso : isoFilesToDelete) {
                        deleteCommand += shell_escape(deleteIso) + " ";
                    }
                    deleteCommand += "> /dev/null 2>&1";

                    // Execute the delete command
                    int result = std::system(deleteCommand.c_str());

                    // Process deletion results
                    if (result == 0) {
                        for (const auto& deletedIso : isoFilesToDelete) {
                            deletedSet.insert(deletedIso);
                            std::string deletedIsoInfo = "\033[1;92mDeleted: \033[1;91m'" + isoDirectory + "/" + isoFilename + "'\033[1;92m.\033[0m\033[1m";
                            deletedIsos.push_back(deletedIsoInfo);
                        }
                    } else {
                        for (const auto& deletedIso : isoFilesToDelete) {
                            auto [isoDir, isoFile] = extractDirectoryAndFilename(deletedIso);
                            std::string errorMessageInfo = "\033[1;91mError deleting: \033[0m\033[1m'" + isoDir + "/" + isoFile + "'\033[1;95m.\033[0m\033[1m";
							deletedErrors.push_back(errorMessageInfo);
                        }
                    }

                    // Clear the deletion batch for the next set
                    isoFilesToDelete.clear();
                }
            } else {
                std::cout << "\033[1;35mFile not found: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[0m\033[1m" << std::endl;
            }
        } else {
            std::cout << "\033[1;93mFile not found in cache: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m.\033[0m\033[1m" << std::endl;
        }
    }
}


// Function to process user input for selecting and deleting specific ISO files
void processDeleteInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
        
    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    std::vector<int> validIndices;     // Vector to keep track of valid indices

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

    // Display additional information if there are invalid inputs and some valid indices
    if (invalidInput && !validIndices.empty()) {
        std::cout << " " << std::endl;
    }
	// Detect and use the minimum of available threads and validIndices to ensure efficient parallelism
	unsigned int numThreads = std::min(static_cast<int>(validIndices.size()), static_cast<int>(maxThreads));

    // Batch the valid indices into chunks based on numThreads
    std::vector<std::vector<int>> indexChunks;
    const size_t chunkSize = (validIndices.size() + numThreads - 1) / numThreads;
    for (size_t i = 0; i < validIndices.size(); i += chunkSize) {
        indexChunks.emplace_back(validIndices.begin() + i, std::min(validIndices.begin() + i + chunkSize, validIndices.end()));
    }

     // Display selected deletions
    if (!indexChunks.empty()) {
        std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*PERMANENTLY DELETED*\033[1;94m:\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m" << std::endl;
            }
        }
    }

     // Display a message if there are no valid selections for deletion
    if (!uniqueErrorMessages.empty() && indexChunks.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mNo valid selection(s) for deletion.\033[0m\033[1m" << std::endl;
    } else {
        // Prompt for confirmation before proceeding
        std::string confirmation;
        std::cout << " " << std::endl;
        std::cout << "\033[1;94mDo you want to proceed with the \033[1;91mdeletion\033[1;94m of the above? (y/n):\033[0m\033[1m ";
        std::getline(std::cin, confirmation);

        // Check if the entered character is not 'Y' or 'y'
    if (!(confirmation == "y" || confirmation == "Y")) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;93mDeletion aborted by user.\033[0m\033[1m" << std::endl;
        return;
    } else {
        // Start the timer after user confirmation
        auto start_time = std::chrono::high_resolution_clock::now();

        std::system("clear");
        std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
        // Detect and use the minimum of available threads and indexChunks to ensure efficient parallelism
		unsigned int numThreads = std::min(static_cast<int>(indexChunks.size()), static_cast<int>(maxThreads));
        // Create a thread pool with a optimal number of threads
        ThreadPool pool(numThreads);
        // Use std::async to launch asynchronous tasks
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);
        
        // Lock to ensure thread safety in a multi-threaded environment
        std::lock_guard<std::mutex> highLock(Mutex4High);
        
        // Launch deletion tasks for each chunk of selected indices
        for (const auto& chunk : indexChunks) {
            std::vector<std::string> isoFilesInChunk;
            for (const auto& index : chunk) {
                isoFilesInChunk.push_back(isoFiles[index - 1]);
            }
            futures.emplace_back(pool.enqueue(handleDeleteIsoFile, isoFilesInChunk, std::ref(isoFiles), std::ref(deletedSet)));
        }

        // Wait for all asynchronous tasks to complete
        for (auto& future : futures) {
            future.wait();
        }
        
        clearScrollBuffer();
        std::system("clear");
        
        if (!deletedIsos.empty()) {
            std::cout << " " << std::endl;
        }
    
        // Print all deleted files
        for (const auto& deletedIso : deletedIsos) {
            std::cout << deletedIso << std::endl;
        }
        
        if (!deletedErrors.empty()) {
            std::cout << " " << std::endl;
        }
        
        for (const auto& deletedError : deletedErrors) {
            std::cout << deletedError << std::endl;
        }
        
        // Clear the vector after each iteration
        deletedIsos.clear();

        // Stop the timer after completing all deletion tasks
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;
		}
	}
}
