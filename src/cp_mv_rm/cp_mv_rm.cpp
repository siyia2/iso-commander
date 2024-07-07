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
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    // Initialize color pairs
    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);
    init_pair(5, COLOR_BLUE, COLOR_BLACK);  // Color pair for filter input

    std::vector<std::string> originalIsoFiles;
    std::vector<std::string> filteredIsoFiles;
    std::set<std::string> operationIsos, operationErrors, uniqueErrorMessages;
    std::vector<bool> selectedFiles;
    std::string filterInput;
    bool filterMode = false; // Track the filter mode state

    while (true) {
        clear();
        removeNonExistentPathsFromCache();
        loadCache(originalIsoFiles);
        sortFilesCaseInsensitive(originalIsoFiles);

        if (originalIsoFiles.empty()) {
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(0, 0, "ISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.");
            mvprintw(2, 0, "Press any key to continue...");
            attroff(COLOR_PAIR(4) | A_BOLD);
            getch();
            break;
        }

        filteredIsoFiles = originalIsoFiles;
        selectedFiles.resize(filteredIsoFiles.size(), false);

        int maxY, maxX;
        getmaxyx(stdscr, maxY, maxX);
        int startY = 4;
        int pageSize = maxY - 7;
        int currentPage = 0;
        int currentSelection = 0;
        std::string numberInput;

        while (true) {
            clear();
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(0, 0, "ISO File Selection (Page %d/%ld)", currentPage + 1, (long)(filteredIsoFiles.size() + pageSize - 1) / pageSize);
            mvprintw(1, 0, "Use UP/DOWN to navigate, SPACE to select, ENTER to %s, F to toggle filter mode, Q to quit", operation.c_str());
            mvprintw(2, 0, "Type a number to jump to that entry");
            attroff(COLOR_PAIR(4) | A_BOLD);

            if (filterMode) {
                attron(COLOR_PAIR(5) | A_BOLD);  // Blue color for filter input
                mvprintw(3, 0, "Filter: ");
                
                // Print ':' in white bold
                attroff(COLOR_PAIR(5) | A_BOLD);
                attron(COLOR_PAIR(4) | A_BOLD);  // White color for colon
                attroff(COLOR_PAIR(4) | A_BOLD);

                // Print filterInput in blue
                attron(COLOR_PAIR(5) | A_BOLD);
                addstr(filterInput.c_str());
                attroff(COLOR_PAIR(5) | A_BOLD);
            } else {
                mvprintw(3, 0, "Filter: %s", filterInput.c_str());
            }

            size_t maxIndex = filteredIsoFiles.size();
            size_t numDigits = std::to_string(maxIndex).length();

            size_t start = currentPage * pageSize;
            size_t end = std::min(start + pageSize, filteredIsoFiles.size());

            for (size_t i = start; i < end; ++i) {
                int color_pair = (i % 2 == 0) ? 1 : 2;
                attron(COLOR_PAIR(color_pair) | A_BOLD);
                mvprintw(startY + static_cast<int>(i - start), 0, "%c %*ld. ", 
                         selectedFiles[i] ? '*' : ' ', static_cast<int>(numDigits), i + 1);
                attroff(COLOR_PAIR(color_pair) | A_BOLD);

                auto [directory, filename] = extractDirectoryAndFilename(filteredIsoFiles[i]);
                
                attron(A_BOLD);
                addstr(directory.c_str());
                addch('/');
                attroff(A_BOLD);

                attron(COLOR_PAIR(3) | A_BOLD);
                addstr(filename.c_str());
                attroff(COLOR_PAIR(3) | A_BOLD);
            }

            mvchgat(startY + currentSelection, 0, -1, A_REVERSE, 0, NULL);
            
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(maxY - 1, 0, "Jump to: %s", numberInput.c_str());
            attroff(COLOR_PAIR(4) | A_BOLD);
            
            refresh();

            int ch = getch();
            if (ch >= '0' && ch <= '9') {
                numberInput += static_cast<char>(ch);
                continue;
            }

            switch (ch) {
                case KEY_UP:
                    if (currentSelection > 0) {
                        currentSelection--;
                    } else if (currentPage > 0) {
                        currentPage--;
                        currentSelection = pageSize - 1;
                    }
                    break;
                case KEY_DOWN:
                    if (currentSelection < static_cast<int>(end - start) - 1) {
                        currentSelection++;
                    } else if (end < filteredIsoFiles.size()) {
                        currentPage++;
                        currentSelection = 0;
                    }
                    break;
                case KEY_NPAGE:
                    if (static_cast<size_t>(currentPage) < (filteredIsoFiles.size() + pageSize - 1) / pageSize - 1) {
                        currentPage++;
                        currentSelection = 0;
                    }
                    break;
                case KEY_PPAGE:
                    if (currentPage > 0) {
                        currentPage--;
                        currentSelection = 0;
                    }
                    break;
                case ' ': // Spacebar
                    {
                        size_t selectedIndex = start + currentSelection;
                        selectedFiles[selectedIndex] = !selectedFiles[selectedIndex];
                    }
                    break;
                case 10: // Enter key
                    if (!numberInput.empty()) {
                        int jumpTo = std::stoi(numberInput) - 1; // Convert to 0-based index
                        if (jumpTo >= 0 && jumpTo < static_cast<int>(filteredIsoFiles.size())) {
                            currentPage = jumpTo / pageSize;
                            currentSelection = jumpTo % pageSize;
                        }
                        numberInput.clear();
                    } else {
                        std::vector<std::string> filesToOperate;
                        for (size_t i = 0; i < filteredIsoFiles.size(); ++i) {
                            if (selectedFiles[i]) {
                                filesToOperate.push_back(filteredIsoFiles[i]);
                            }
                        }
                        if (!filesToOperate.empty()) {
                            processOperationInput(numberInput, filteredIsoFiles, operation, operationIsos, operationErrors, uniqueErrorMessages);
                            clear();
                            attron(COLOR_PAIR(4) | A_BOLD);
                            mvprintw(0, 0, "Processed %ld ISO(s) with %s operation", filesToOperate.size(), operation.c_str());
                            mvprintw(2, 0, "Press any key to continue...");
                            attroff(COLOR_PAIR(4) | A_BOLD);
                            getch();
                            goto exit_loop; // Exit after operation
                        } else {
                            attron(COLOR_PAIR(4) | A_BOLD);
                            mvprintw(maxY - 1, 0, "No ISOs selected. Press any key to continue...");
                            attroff(COLOR_PAIR(4) | A_BOLD);
                            getch();
                        }
                    }
                    break;
                case KEY_BACKSPACE:
                case 127: // Delete key
                    if (!numberInput.empty()) {
                        numberInput.pop_back();
                    } else if (!filterInput.empty()) {
                        filterInput.pop_back();
                        filteredIsoFiles = filterFiles(originalIsoFiles, filterInput);
                        selectedFiles.resize(filteredIsoFiles.size(), false);
                        currentPage = 0;
                        currentSelection = 0;
                    }
                    break;
                case 'f':
                case 'F':
                    // Toggle filter mode
                    filterMode = !filterMode; // Toggle filter mode state
                    
                    if (filterMode) {
                        filterInput.clear();
                        filteredIsoFiles = originalIsoFiles;
                        selectedFiles.resize(filteredIsoFiles.size(), false);
                        currentPage = 0;
                        currentSelection = 0;
                    } else {
                        // Revert the filter color to default (white)
                        attron(COLOR_PAIR(4) | A_BOLD);  // White color for default text
                        mvprintw(3, 0, "Filter: %s", filterInput.c_str());
                        attroff(COLOR_PAIR(4) | A_BOLD);
                    }
                    break;
                case 'q':
                case 'Q':
                    goto exit_loop;
                default:
                    if (filterMode && isprint(ch)) {
                        filterInput += static_cast<char>(ch);
                        filteredIsoFiles = filterFiles(originalIsoFiles, filterInput);
                        selectedFiles.resize(filteredIsoFiles.size(), false);
                        currentPage = 0;
                        currentSelection = 0;
                    }
                    break;
            }
        }
    }
exit_loop:
    endwin();
}




// Function to process either mv or cp indices
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages) {
	
	// variable for user specified destination
	std::string userDestDir;

    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);    

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    processedIndices.reserve(maxThreads);
    
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
                uniqueErrorMessages.emplace("\033[1;91mInvalid index '0'.\033[0;1m");
            }
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.emplace("\033[1;91mInvalid index '0'.\033[0;1m");
            }
        }
        
        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInput = true;
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
                invalidInput = true;
                uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
                continue;
            }
            
            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                invalidInput = true;
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[0;1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i); // Mark as processed
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    invalidInput = true;
                    uniqueErrorMessages.emplace("\033[1;91mInvalid index '" + std::to_string(i) + "'.\033[0;1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num); // Mark index as processed
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.emplace("\033[1;91mInvalid index '" + std::to_string(num) + "'.\033[0;1m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
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
			
			if (processedIndices.empty()) {
			clearScrollBuffer();
			mvDelBreak=false;
			verbose = false;
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
			userDestDir.clear();

            // Ask for the destination directory
            std::string prompt = "\n\001\033[1;92m\002Destination directory\001\033[1;94m\002 ↵ for selected ISO(s) to be " + operationColor + operationDescription + "\001\033[1;94m\002 into, or ↵ to abort:\n\001\033[0;1m\002";
            
			// Use std::unique_ptr to manage memory for input
			std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
		
			std::string mainInputString(input.get());

            // Check if the user canceled
            if (input.get()[0] == '\0') {
				mvDelBreak=false;
				clear_history();
                return;
            }

            // Check if the entered path is valid
			if (isValidLinuxPathFormat(mainInputString) && std::string(mainInputString).back() == '/') {
				userDestDir = mainInputString;
				add_history(input.get());
				saveHistory();
				clear_history();
				break;
			} else if (isValidLinuxPathFormat(mainInputString) && std::string(mainInputString).back() != '/') {
				std::cout << "\n\033[1;91mThe path must end with \033[0;1m'/'\033[1;91m.\033[0;1m\n";
			} else {
				std::cout << "\n\033[1;91mInvalid paths and/or multiple paths are excluded from \033[1;92mcp\033[1;91m and \033[1;93mmv\033[1;91m operations.\033[0;1m\n";
			}

			std::cout << "\n\033[1;32m↵ to try again...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		}
    } else {
		clearScrollBuffer();	
		
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
			verbose = false;
            std::cout << "\n\033[1;91mNo valid input for deletion.\033[0;1m\n";
        } else {
            std::string confirmation;
            std::cout << "\n\033[1;94mThe selected ISO(s) will be \033[1;91m*PERMANENTLY DELETED FROM DISK*\033[1;94m. Proceed? (y/n):\033[0;1m ";
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
    
	// Add progress tracking
	std::atomic<int> totalTasks(0);
	std::atomic<int> completedTasks(0);
	std::mutex progressMutex;
	std::atomic<bool> isProcessingComplete(false);

	// Set total number of tasks
	totalTasks.store(static_cast<int>(processedIndices.size()));

	// Create a non-atomic int to hold the total tasks value
	int totalTasksValue = totalTasks.load();

	// Start the progress bar in a separate thread
	std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete));

	ThreadPool pool(numThreads);
	std::vector<std::future<void>> futures;
	futures.reserve(numThreads);

	for (const auto& chunk : indexChunks) {
		std::vector<std::string> isoFilesInChunk;
		for (const auto& index : chunk) {
			isoFilesInChunk.push_back(isoFiles[index - 1]);
		}
		
		{	
			std::lock_guard<std::mutex> highLock(Mutex4High);
			futures.emplace_back(pool.enqueue([&, isoFilesInChunk]() {
				handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, userDestDir, isMove, isCopy, isDelete);
				// Update progress
				completedTasks.fetch_add(static_cast<int>(isoFilesInChunk.size()), std::memory_order_relaxed);
			}));
		}
	}

	for (auto& future : futures) {
		future.wait();
	}

	// Signal that processing is complete and wait for the progress thread to finish
	isProcessingComplete.store(true);
	progressThread.join();
    
	if (!isDelete) {
		promptFlag = false;
		maxDepth = 0;   
		manualRefreshCache(userDestDir);
	}
	if (verbose) {
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
		
		if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
		}
        for (const auto& uniqueErrorMessage : uniqueErrorMessages) {
            std::cout << uniqueErrorMessage << "\033[0;1m\n";
		}
	}
        
    // Clear the vector after each iteration
    operationIsos.clear();
    operationErrors.clear();
    userDestDir.clear();
    uniqueErrorMessages.clear();
    clear_history();
        
    maxDepth = -1;
    if (verbose) {
		std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}  
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
