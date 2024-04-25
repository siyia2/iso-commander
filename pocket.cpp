#include "headers.h"


// Function to select and delete ISO files by number
void select_and_move_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
		clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93mNo ISO(s) available for move.\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
        return;
    }

    // Filter isoFiles to include only entries with ".iso" or ".ISO" extensions
    isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
        return !ends_with_iso(iso);
    }), isoFiles.end());

    // Set to track deleted ISO files
    std::unordered_set<std::string> movedSet;

    // Main loop for selecting and deleting ISO files
    while (true) {
		clearScrollBuffer();
        std::system("clear");
        //std::cout << "\033[1;93m ! ISO DELETION IS IRREVERSIBLE PROCEED WITH CAUTION !\n\033[0m\033[1m" << std::endl;

        // Remove non-existent paths from the cache
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
        isoFiles = loadCache();

        // Filter isoFiles to include only entries with ".iso" or ".ISO" extensions
        isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
            return !ends_with_iso(iso);
        }), isoFiles.end());

        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Prompt user for input
char* input = readline("\033[1;94mISO(s) ↵ for \033[1;91mmv\033[1;94m (e.g., '1-3', '1 5'), or press ↵ to return:\033[0m\033[1m ");


std::system("clear");

        // Check if the user wants to return
        if (input[0] == '\0') {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        } else {
			clearScrollBuffer();
			std::system("clear");
            // Process user input to select and delete specific ISO files
            processMoveInput(input, isoFiles, movedSet);
        }

        // Check if the ISO file list is empty
        if (isoFiles.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mNo ISO(s) available for deletion.\033[0m\033[1m" << std::endl;
            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue..." << std::endl;
            std::cin.get();
            break;
        }
    }
}


// Function to handle the deletion of ISO files in batches
void handleMoveIsoFile(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, const std::string& userDestDir) {
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
	// Declare the isoFilesToMove vector
    std::vector<std::string> isoFilesToMove;
    
// Process each ISO file
    for (const auto& iso : isoFiles) {
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Check if the ISO file is in the cache
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);

        if (it != isoFilesCopy.end()) {
            // Check if the file exists before attempting to move
            if (fileExists(iso)) {
                // Add the ISO file to the move batch
                isoFilesToMove.push_back(iso);

                // If the move batch reaches the batch size, or no more ISO files to process
                if (isoFilesToMove.size() == batchSize || &iso == &isoFiles.back()) {
                    // Construct the move command for the entire batch
                    std::string moveCommand = "mkdir -p " + userDestDir + " && mv ";
                    for (const auto& moveIso : isoFilesToMove) {
                        moveCommand += shell_escape(moveIso) + " " + userDestDir + " ";
                    }
                    moveCommand += "> /dev/null 2>&1";

                    // Execute the move command
                    int result = std::system(moveCommand.c_str());

                    // Process move results
                    if (result == 0) {
                           for (const auto& iso : isoFilesToMove) {
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);
        std::string movedIsoInfo = "\033[1;92mMoved: \033[1;91m'" + isoDirectory + "/" + isoFilename + "'\033[1;92m to \033[1;91m'" + userDestDir + "'\033[0m\033[1m";
        movedIsos.push_back(movedIsoInfo);
    }
} else {
    for (const auto& iso : isoFilesToMove) {
        auto [isoDir, isoFilename] = extractDirectoryAndFilename(iso);
        std::string errorMessageInfo = "\033[1;91mError moving: \033[0m\033[1m'" + isoDir + "/" + isoFilename + "'\033[1;95m to \033[1;91m'" + userDestDir + "'\033[0m\033[1m";
        movedErrors.push_back(errorMessageInfo);
    }
}

                    // Clear the move batch for the next set
                    isoFilesToMove.clear();
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
void processMoveInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& movededSet) {
	
std::string userDestDir;

// Clear the userDestDir variable
userDestDir.clear();

// Load history from file
loadHistory();

// Ask the user for the destination directory
std::string inputLine = readInputLine("\033[1;94mEnter the destination directory for the selected ISO files or press ↵ to cancel:\n\033[0m\033[1m");

if (!inputLine.empty()) {
    // Save history to file
    saveHistory();
}

// Check if the user canceled the cache refresh
if (inputLine.empty()) {
	// Clear history
    clear_history();
    return;
}

// Store the user input in userDestDir
userDestDir = inputLine; // Here, you had a typo 'inputD' instead of 'inputLine'

// Check if the entered path is valid
while (true) {
    std::filesystem::path destPath(userDestDir);
    if (std::filesystem::exists(destPath)) {
        break; // Valid path, exit the loop
    } else {
        std::cout << "\n\033[1;91mInvalid path. The destination directory does not exist.\033[0m\033[1m" << std::endl;
        std::cout << "\n\033[1;32mPress Enter to try again...\033[0m\033[1m";
        std::cin.get();
        std::system("clear");
        std::string inputLine = readInputLine("\033[1;94mEnter the destination directory for the selected ISO files or press ↵ to cancel:\n\033[0m\033[1m");

        // Check if the user canceled the cache refresh
        if (inputLine.empty()) {
			// Clear history
			clear_history();
            return;
        }

        // Store the new user input in userDestDir
        userDestDir = inputLine;
    }
}
     // Clear history
       clear_history();
           
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

     // Display selected moves
    if (!indexChunks.empty()) {
		std::system("clear");
        std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*MOVED* \033[1;94mto \033[1;93m" << userDestDir << "\033[1;94m:\033[0m\033[1m" << std::endl;
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
        std::cout << "\033[1;91mNo valid selection(s) for move.\033[0m\033[1m" << std::endl;
    } else {
        // Prompt for confirmation before proceeding
        std::string confirmation;
        std::cout << " " << std::endl;
        std::cout << "\033[1;94mDo you want to proceed with the \033[1;91mmove\033[1;94m of the above? (y/n):\033[0m\033[1m ";
        std::getline(std::cin, confirmation);

        // Check if the entered character is not 'Y' or 'y'
    if (!(confirmation == "y" || confirmation == "Y")) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;93mMove aborted by user.\033[0m\033[1m" << std::endl;
        return;
    } else {
        // Start the timer after user confirmation
        auto start_time = std::chrono::high_resolution_clock::now();

        std::system("clear");
        // Detect and use the minimum of available threads and indexChunks to ensure efficient parallelism
		unsigned int numThreads = std::min(static_cast<int>(indexChunks.size()), static_cast<int>(maxThreads));
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
    futures.emplace_back(pool.enqueue(handleMoveIsoFile, isoFilesInChunk, std::ref(isoFiles), userDestDir));
}

        // Wait for all asynchronous tasks to complete
        for (auto& future : futures) {
            future.wait();
        }
        
        clearScrollBuffer();
        std::system("clear");
        
        if (!movedIsos.empty()) {
            std::cout << " " << std::endl;
        }
    
        // Print all deleted files
        for (const auto& movedIso : movedIsos) {
            std::cout << movedIso << std::endl;
        }
        
        if (!movedErrors.empty()) {
            std::cout << " " << std::endl;
        }
        
        for (const auto& deletedError : movedErrors) {
            std::cout << deletedError << std::endl;
        }
        
        // Clear the vector after each iteration
        movedIsos.clear();

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
	}
}
