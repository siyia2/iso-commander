#include "../headers.h"
#include "../threadpool.h"
#include <ncurses.h>

//	MOUNT STUFF

// Function to mount all ISOs indiscriminately
void select_and_mount_files_by_number() {
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

    std::vector<std::string> originalIsoFiles;
    std::vector<std::string> filteredIsoFiles;
    std::set<std::string> mountedFiles, skippedMessages, mountedFails;
    std::vector<bool> selectedFiles;
    std::string filterInput;

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
            mvprintw(1, 0, "Use UP/DOWN to navigate, SPACE to select, ENTER to mount, F to toggle filter mode, Q to quit");
            mvprintw(2, 0, "Type a number to jump to that entry");
            mvprintw(3, 0, "Filter: %s", filterInput.c_str());
            attroff(COLOR_PAIR(4) | A_BOLD);

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
                        std::vector<std::string> filesToMount;
                        for (size_t i = 0; i < filteredIsoFiles.size(); ++i) {
                            if (selectedFiles[i]) {
                                filesToMount.push_back(filteredIsoFiles[i]);
                            }
                        }
                        if (!filesToMount.empty()) {
                            mountIsoFile(filesToMount, mountedFiles, skippedMessages, mountedFails);
                            clear();
                            attron(COLOR_PAIR(4) | A_BOLD);
                            mvprintw(0, 0, "Mounted %ld ISO(s)", filesToMount.size());
                            mvprintw(2, 0, "Press any key to continue...");
                            attroff(COLOR_PAIR(4) | A_BOLD);
                            getch();
                            goto exit_loop; // Exit after mounting
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
                    filterInput.clear();
                    filteredIsoFiles = originalIsoFiles;
                    selectedFiles.resize(filteredIsoFiles.size(), false);
                    currentPage = 0;
                    currentSelection = 0;
                    break;
                case 'q':
                case 'Q':
                    goto exit_loop;
                default:
                    if (isprint(ch)) {
                        filterInput += static_cast<char>(ch);
                        filteredIsoFiles = filterFiles(originalIsoFiles, filterInput);
                        selectedFiles.resize(filteredIsoFiles.size(), false);
                        currentPage = 0;
                        currentSelection = 0;
                    }
                    break;
            }
            numberInput.clear(); // Clear number input after each non-digit key press
        }
exit_loop:
        break;
    }

    endwin();
}

// Function to print mount verbose messages
void printMountedAndErrors( std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages) {
		
    // Print all mounted files
    for (const auto& mountedFile : mountedFiles) {
        std::cout << "\n" << mountedFile << "\033[0;1m";
    }
    
    if (!mountedFiles.empty()) {
        std::cout << "\n";
    }

    // Print all the stored skipped messages
    for (const auto& skippedMessage : skippedMessages) {
        std::cerr << "\n" << skippedMessage << "\033[0;1m";
    }
    
    if (!skippedMessages.empty()) {
        std::cout << "\n";
    }

    // Print all the stored error messages
    for (const auto& mountedFail : mountedFails) {
        std::cerr << "\n" << mountedFail << "\033[0;1m";
    }
    
    if (!mountedFails.empty()) {
        std::cout << "\n";
    }
	
    // Print all the stored error messages
    for (const auto& errorMessage : uniqueErrorMessages) {
        std::cerr << "\n" << errorMessage << "\033[0;1m";
    }
    
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
    }

    // Clear the vectors after each iteration
    mountedFiles.clear();
    skippedMessages.clear();
    mountedFails.clear();
    uniqueErrorMessages.clear();
    
	std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to check if a mountpoint isAlreadyMounted
bool isAlreadyMounted(const std::string& mountPoint) {
    struct statvfs vfs;
    if (statvfs(mountPoint.c_str(), &vfs) != 0) {
        return false; // Error or doesn't exist
    }

    // Check if it's a mount point
    return (vfs.f_flag & ST_NODEV) == 0;
}


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    namespace fs = std::filesystem;
    
    // Declare fsTypes inside the function
    const std::vector<std::string> fsTypes = {
        "iso9660", "udf", "hfsplus", "rockridge", "joliet", "isofs", "auto"
    };

    for (const auto& isoFile : isoFilesToMount) {
        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();

        // Create a hash of the full path
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);

        // Convert hash to a base36 string (using digits 0-9 and letters a-z)
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {  // Use 5 characters for the short hash
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        std::string uniqueId = isoFileName + "\033[38;5;245m~" + shortHash + "\033[1;94m";
        std::string mountPoint = "/mnt/iso_" + uniqueId;

        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);

        if (isAlreadyMounted(mountPoint)) {
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename
                           << "'\033[1;93m already mnt@: \033[1;94m'" << mountisoDirectory
                           << "/" << mountisoFilename << "'\033[1;93m.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                skippedMessages.insert(skippedMessage.str());
            }
            continue;
        }

        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[0m\033[1;91m. Root privileges are required.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
            continue;
        }

        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to create mount point: \033[1;93m'" << mountPoint
                             << "'\033[0m\033[1;91m. Error: " << e.what() << "\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    mountedFails.insert(errorMessage.str());
                }
                continue;
            }
        }

        bool mountSuccess = false;

	// Attempt to mount using sudo mount command
	for (const auto& fsType : fsTypes) {
		std::string mountOptions = "loop,ro";
		if (fsType == "auto") {
			// For "auto", we don't specify a file system type
			mountOptions += ",auto";
			std::string mountCommand = "mount -o " + mountOptions + " " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
			int ret = std::system(mountCommand.c_str());
        
			if (ret == 0) {
					// Mount successful
					std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
												+ "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "\033[1;94m/" + mountisoFilename
												+ "'\033[0;1m.\033[0m";
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						mountedFiles.insert(mountedFileInfo);
					}
					mountSuccess = true;
					break;
			}
		
			} else {
				// For specific file systems, we include the type in the mount command
				std::string mountCommand = "mount -t " + fsType + " -o " + mountOptions + " " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
				int ret = std::system(mountCommand.c_str());
        
				if (ret == 0) {
					// Mount successful
					std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
												+ "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
												+ "'\033[0;1m. {" + fsType + "}\033[0m";
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						mountedFiles.insert(mountedFileInfo);
					}
					mountSuccess = true;
					break;
				}
			}
		}
        

        if (!mountSuccess) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[1;91m.\033[0;1m {badFS}";
            fs::remove(mountPoint);
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
        }
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages) {
    std::istringstream iss(input);
    
    std::atomic<bool> invalidInput(false);
    std::mutex indicesMutex;
    std::set<int> processedIndices;
    std::mutex validIndicesMutex;
    std::set<int> validIndices;
    std::mutex processedRangesMutex;
    std::set<std::pair<int, int>> processedRanges;
    
    // Step 1: Tokenize the input to determine the number of threads to use
    std::istringstream issCount(input);
    std::set<std::string> tokens;
    std::string tokenCount;
    
    while (issCount >> tokenCount && tokens.size() < maxThreads) {
    if (tokenCount[0] == '-') continue;
    
    // Count the number of hyphens
    size_t hyphenCount = std::count(tokenCount.begin(), tokenCount.end(), '-');
    
    // Skip if there's more than one hyphen
    if (hyphenCount > 1) continue;
    
    size_t dashPos = tokenCount.find('-');
    if (dashPos != std::string::npos) {
        std::string start = tokenCount.substr(0, dashPos);
        std::string end = tokenCount.substr(dashPos + 1);
        if (std::all_of(start.begin(), start.end(), ::isdigit) && 
            std::all_of(end.begin(), end.end(), ::isdigit)) {
            int startNum = std::stoi(start);
            int endNum = std::stoi(end);
            if (static_cast<std::vector<std::string>::size_type>(startNum) <= isoFiles.size() && 
                static_cast<std::vector<std::string>::size_type>(endNum) <= isoFiles.size()) {
                int step = (startNum <= endNum) ? 1 : -1;
                for (int i = startNum; step > 0 ? i <= endNum : i >= endNum; i += step) {
                    if (i != 0) {
                        tokens.emplace(std::to_string(i));
                    }
                    if (tokens.size() >= maxThreads) {
                        break;
                    }
                }
            }
        }
    } else if (std::all_of(tokenCount.begin(), tokenCount.end(), ::isdigit)) {
			int num = std::stoi(tokenCount);
			if (num > 0 && static_cast<std::vector<std::string>::size_type>(num) <= isoFiles.size()) {
				tokens.emplace(tokenCount);
				if (tokens.size() >= maxThreads) {
					break;
				}
			}
		}
	}
    unsigned int numThreads = std::min(static_cast<int>(tokens.size()), static_cast<int>(maxThreads));
    std::vector<int> indicesToAdd;
	indicesToAdd.reserve(numThreads);
	ThreadPool pool(numThreads);
    
    std::atomic<int> totalTasks(0);
    std::atomic<int> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    std::atomic<int> activeTaskCount(0);

    std::condition_variable taskCompletionCV;
    std::mutex taskCompletionMutex;

    std::mutex errorMutex;

    auto processTask = [&](int index) {
    bool shouldProcess = false;
    {
        std::lock_guard<std::mutex> lock(validIndicesMutex);
        shouldProcess = validIndices.emplace(index).second;
    }

    if (shouldProcess) {
        std::vector<std::string> isoFilesToMount = {isoFiles[index - 1]};
        mountIsoFile(isoFilesToMount, mountedFiles, skippedMessages, mountedFails);
    }

	completedTasks.fetch_add(1, std::memory_order_relaxed);
	if (activeTaskCount.fetch_sub(1, std::memory_order_release) == 1) {
			taskCompletionCV.notify_all();
		}
	};

	auto addError = [&](const std::string& error) {
		std::lock_guard<std::mutex> lock(errorMutex);
		uniqueErrorMessages.emplace(error);
		invalidInput.store(true, std::memory_order_release);
	};


	std::string token;
	while (iss >> token) {
		if (token == "/") break;

		if (isAllZeros(token)) {
			addError("\033[1;91mInvalid index: '0'.\033[0;1m");
			continue;
		}

		size_t dashPos = token.find('-');
		if (dashPos != std::string::npos) {
			if (dashPos == 0 || dashPos == token.size() - 1 || 
				!std::all_of(token.begin(), token.begin() + dashPos, ::isdigit) || 
				!std::all_of(token.begin() + dashPos + 1, token.end(), ::isdigit)) {
				addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
				continue;
			}

			int start, end;
			try {
				start = std::stoi(token.substr(0, dashPos));
				end = std::stoi(token.substr(dashPos + 1));
			} catch (const std::exception&) {
				addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
				continue;
			}

			if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || 
				end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
				addError("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
				continue;
			}

			std::pair<int, int> range(start, end);
			bool shouldProcessRange = false;
			{
				std::lock_guard<std::mutex> lock(processedRangesMutex);
				shouldProcessRange = processedRanges.emplace(range).second;
			}

			if (shouldProcessRange) {
				int step = (start <= end) ? 1 : -1;
				for (int i = start; (step > 0) ? (i <= end) : (i >= end); i += step) {
					{
						std::lock_guard<std::mutex> lock(indicesMutex);
						indicesToAdd.push_back(i);
					}
				}
			}
		} else if (isNumeric(token)) {
			int num = std::stoi(token);
			if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
				{
					std::lock_guard<std::mutex> lock(indicesMutex);
					indicesToAdd.push_back(num);
				}
			} else {
				addError("\033[1;91mInvalid index: '" + token + "'.\033[0;1m");
			}
		} else {
			addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
		}
	}

	// Batch insert into processedIndices
	{
		std::lock_guard<std::mutex> lock(indicesMutex);
		for (int num : indicesToAdd) {
			bool shouldProcess = processedIndices.emplace(num).second;
			if (shouldProcess) {
				totalTasks.fetch_add(1, std::memory_order_relaxed);
				activeTaskCount.fetch_add(1, std::memory_order_relaxed);
				pool.enqueue([&, num]() { processTask(num); });
			}
		}
	}

	if (!processedIndices.empty()) {
		int totalTasksValue = totalTasks.load();
		std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete));

		{
			std::unique_lock<std::mutex> lock(taskCompletionMutex);
			taskCompletionCV.wait(lock, [&]() { return activeTaskCount.load() == 0; });
		}

		isProcessingComplete.store(true, std::memory_order_release);
		progressThread.join();
	}
}
