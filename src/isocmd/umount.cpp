#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF

// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    const char* isoPath = "/mnt";
    std::vector<std::string> isoDirs;
    isoDirs.reserve(100);  // Pre-allocate space for 100 entries

        DIR* dir = opendir(isoPath);
        if (dir == nullptr) {
            std::cerr << "\033[1;91mError opening the /mnt directory.\033[0;1m\n";
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && strncmp(entry->d_name, "iso_", 4) == 0) {
                isoDirs.emplace_back(entry->d_name + 4);
            }
        }
        closedir(dir);

    if (isoDirs.empty()) {
        return;
    }

    sortFilesCaseInsensitive(isoDirs);

    std::ostringstream output;
    // Reserve estimated space for the string buffer
    std::string buffer;
    buffer.reserve(isoDirs.size() * 100);
    output.str(std::move(buffer));

    output << "\033[92;1m// CHANGES ARE REFLECTED AUTOMATICALLY //\033[0;1m\n\n";

    size_t numDigits = std::to_string(isoDirs.size()).length();

    for (size_t i = 0; i < isoDirs.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? "\033[31;1m" : "\033[32;1m";
        output << sequenceColor << std::setw(numDigits) << (i + 1) << ". "
               << "\033[94;1m/mnt/iso_\033[95;1m" << isoDirs[i] << "\033[0;1m\n";
    }

    std::cout << output.str();
}


// Function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;  // Unable to open directory
    }

    errno = 0;
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (++count > 2) {
            closedir(dir);
            return false;  // Directory not empty
        }
    }

    closedir(dir);
    return errno == 0 && count <= 2;  // Empty if only "." and ".." entries and no errors
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    // Check for root privileges
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "\033[1;93m'\033[1;91m. Root privileges are required.\033[0m";
            {
				std::lock_guard<std::mutex> lowLock(Mutex4Low);
				unmountedErrors.emplace(errorMessage.str());
			}
        }
        return;
    }

    // Construct the unmount command
    std::string unmountCommand = "umount -l";
    for (const auto& isoDir : isoDirs) {
        unmountCommand += " " + shell_escape(isoDir) + " 2>/dev/null";
    }

    // Execute the unmount command
    int unmountResult = system(unmountCommand.c_str());
    if (unmountResult != 0) {
        // Some error occurred during unmounting
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            if (!isDirectoryEmpty(isoDir)) {
                errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "\033[1;93m'\033[1;91m. Probably not an ISO mountpoint.\033[0m";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.emplace(errorMessage.str());
					}
                }
            }
        }
    }

    // Remove empty directories
    std::vector<const char*> directoriesToRemove;
    for (const auto& isoDir : isoDirs) {
        if (isDirectoryEmpty(isoDir)) {
            directoriesToRemove.push_back(isoDir.c_str());
        }
    }

    if (!directoriesToRemove.empty()) {
        int removeDirResult = 0;
        for (const char* dir : directoriesToRemove) {
            removeDirResult = rmdir(dir);
            if (removeDirResult != 0) {
                break;
            }
        }

        if (removeDirResult == 0) {
            for (const auto& dir : directoriesToRemove) {	
                auto [directory, filename] = extractDirectoryAndFilename(dir);
                std::string removedDirInfo = "\033[0;1mUnmounted: \033[1;92m'" + directory + "/" + filename + "\033[1;92m'\033[0m.";
                {
					std::lock_guard<std::mutex> lowLock(Mutex4Low);
					unmountedFiles.emplace(removedDirInfo);
				}
            }
        } else {
            for (const auto& isoDir : directoriesToRemove) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDir << "'\033[1;91m.\033[0m";
                if (unmountedErrors.find(errorMessage.str()) == unmountedErrors.end()) {
					{
						std::lock_guard<std::mutex> lowLock(Mutex4Low);
						unmountedErrors.emplace(errorMessage.str());
					}
                }
            }
        }
    }
}


// Function to print unmounted ISOs and errors
void printUnmountedAndErrors(bool invalidInput, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::set<std::string>& errorMessages) {
	clearScrollBuffer();
	
    // Print unmounted files
    for (const auto& unmountedFile : unmountedFiles) {
        std::cout << "\n" << unmountedFile;
    }
    
    if (!unmountedErrors.empty() && !unmountedFiles.empty()) {
				std::cout << "\n";
			}
			
	for (const auto& unmountedError : unmountedErrors) {
        std::cout << "\n" << unmountedError;
    }
    
    if (invalidInput) {
				std::cout << "\n";
			}
	unmountedFiles.clear();
	unmountedErrors.clear();
    // Print unique error messages
    for (const auto& errorMessage : errorMessages) {
            std::cerr << "\n\033[1;91m" << errorMessage << "\033[0m\033[1m";
        }
    errorMessages.clear();
}


// Main function for unmounting ISOs
void unmountISOs() {
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

    std::vector<std::string> isoDirs;
    std::set<std::string> errorMessages, unmountedFiles, unmountedErrors;
    bool isFiltered = false;
    std::string filterInput;
    std::vector<bool> selectedDirs;
    const std::string isoPath = "/mnt";

    while (true) {
        clear();
        isoDirs.clear();
        errorMessages.clear();
        unmountedFiles.clear();
        unmountedErrors.clear();
        
        // Populate isoDirs with directories that match the "iso_" prefix
        for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
            if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                isoDirs.push_back(entry.path().string());
            }
        }

        sortFilesCaseInsensitive(isoDirs);

        if (isoDirs.empty()) {
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(0, 0, "No path(s) matching the '/mnt/iso_*' pattern found.");
            mvprintw(2, 0, "Press any key to continue...");
            attroff(COLOR_PAIR(4) | A_BOLD);
            getch();
            break;
        }

        std::vector<std::string> filteredIsoDirs = isoDirs;
        selectedDirs.resize(filteredIsoDirs.size(), false);
        
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
            mvprintw(0, 0, "ISO File Unmount (Page %d/%ld)", currentPage + 1, (long)(filteredIsoDirs.size() + pageSize - 1) / pageSize);
            mvprintw(1, 0, "Use UP/DOWN to navigate, SPACE to select, ENTER to unmount, F to toggle filter mode, Q to quit");
            mvprintw(2, 0, "Type a number to jump to that entry");
            mvprintw(3, 0, "Filter: %s", filterInput.c_str());
            attroff(COLOR_PAIR(4) | A_BOLD);

            size_t maxIndex = filteredIsoDirs.size();
            size_t numDigits = std::to_string(maxIndex).length();

            size_t start = currentPage * pageSize;
            size_t end = std::min(start + pageSize, filteredIsoDirs.size());

            for (size_t i = start; i < end; ++i) {
                int color_pair = (i % 2 == 0) ? 1 : 2;
                attron(COLOR_PAIR(color_pair) | A_BOLD);
                mvprintw(startY + static_cast<int>(i - start), 0, "%c %*ld. ", 
                         selectedDirs[i] ? '*' : ' ', static_cast<int>(numDigits), i + 1);
                attroff(COLOR_PAIR(color_pair) | A_BOLD);

                auto [directory, filename] = extractDirectoryAndFilename(filteredIsoDirs[i]);

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
                    } else if (end < filteredIsoDirs.size()) {
                        currentPage++;
                        currentSelection = 0;
                    }
                    break;
                case KEY_NPAGE:
                    if (static_cast<size_t>(currentPage) < (filteredIsoDirs.size() + pageSize - 1) / pageSize - 1) {
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
                        selectedDirs[selectedIndex] = !selectedDirs[selectedIndex];
                    }
                    break;
                case 10: // Enter key
                    if (!numberInput.empty()) {
                        int jumpTo = std::stoi(numberInput) - 1; // Convert to 0-based index
                        if (jumpTo >= 0 && jumpTo < static_cast<int>(filteredIsoDirs.size())) {
                            currentPage = jumpTo / pageSize;
                            currentSelection = jumpTo % pageSize;
                        }
                        numberInput.clear();
                    } else {
                        std::vector<std::string> dirsToUnmount;
                        for (size_t i = 0; i < filteredIsoDirs.size(); ++i) {
                            if (selectedDirs[i]) {
                                dirsToUnmount.push_back(filteredIsoDirs[i]);
                            }
                        }
                        if (!dirsToUnmount.empty()) {
                            unmountISO(dirsToUnmount, unmountedFiles, unmountedErrors);
                            clear();
                            attron(COLOR_PAIR(4) | A_BOLD);
                            mvprintw(0, 0, "Unmounted %ld ISO(s)", dirsToUnmount.size());
                            mvprintw(2, 0, "Press any key to continue...");
                            attroff(COLOR_PAIR(4) | A_BOLD);
                            getch();
                            goto exit_loop; // Exit after unmounting
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
                        filteredIsoDirs = filterFiles(isoDirs, filterInput);
                        selectedDirs.resize(filteredIsoDirs.size(), false);
                        currentPage = 0;
                        currentSelection = 0;
                    }
                    break;
                case 'f':
                case 'F':
                    // Toggle filter mode
                    filterInput.clear();
                    filteredIsoDirs = isoDirs;
                    selectedDirs.resize(filteredIsoDirs.size(), false);
                    currentPage = 0;
                    currentSelection = 0;
                    break;
                case 'q':
                case 'Q':
                    goto exit_loop;
                default:
                    if (isprint(ch)) {
                        filterInput += static_cast<char>(ch);
                        filteredIsoDirs = filterFiles(isoDirs, filterInput);
                        selectedDirs.resize(filteredIsoDirs.size(), false);
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

