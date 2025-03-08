// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// For storing isoFiles in RAM
std::vector<std::string> globalIsoFileList;

// Mutex to prevent race conditions when live updating ISO list
std::mutex updateListMutex;


// Function to automatically update ISO list if auto-update is on
void refreshListAfterAutoUpdate(int timeoutSeconds, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateHasRun, bool& umountMvRmBreak, std::vector<std::string>& filteredFiles, bool& isFiltered, std::string& listSubtype, std::atomic<bool>& newISOFound) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));
        
        if (!isImportRunning.load()) {
			if (newISOFound.load() && isAtISOList.load()) {
				
				clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak);
            
				std::cout << "\n";
				rl_on_new_line(); 
				rl_redisplay();
			}
            updateHasRun.store(false);
            newISOFound.store(false);
            
            break;
        }
    }
}


// Main function to select and operate on ISOs by number for umount mount cp mv and rm
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    // Bind readline keys
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    std::unordered_set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
    std::vector<std::string> filteredFiles;
    
    // Static vector renamed to isoDirs: used exclusively for umount
    static std::vector<std::string> isoDirs; 
    
    globalIsoFileList.reserve(100);
    filteredFiles.reserve(100);
    isoDirs.reserve(100);
    
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool umountMvRmBreak = false;
    
    bool promptFlag = false;
    int maxDepth = 0;

    // Determine operation color and specific flags
    std::string operationColor = operation == "rm" ? "\033[1;91m" :
                                 operation == "cp" ? "\033[1;92m" : 
                                 operation == "mv" ? "\033[1;93m" :
                                 operation == "mount" ? "\033[1;92m" : 
                                 operation == "write" ? "\033[1;93m" :
                                 operation == "umount" ? "\033[1;93m" : "\033[1;95m";
                                 
    bool isMount = (operation == "mount");
    bool isUnmount = (operation == "umount");
    bool write = (operation == "write");
    
    std::string listSubtype = isMount ? "mount" : (write ? "write" : "cp_mv_rm");
    
    while (true) {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        g_operationCancelled.store(false);
        resetVerboseSets(operationFiles, skippedMessages, operationFails, uniqueErrorMessages);
        bool filterHistory = false;
        bool verbose = false;
        clear_history();
        
        if (!isUnmount) {
			removeNonExistentPathsFromCache();
			isAtISOList.store(true);
		}
        
        // Load files based on operation type
        if (needsClrScrn) {
            if (!isUnmount) {
                if (!clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak))
                    break;
            }
            std::cout << "\n\n";
            // Flag for initiating screen clearing on destructive list actions e.g. Umount/Mv/Rm
            umountMvRmBreak = false;
        }
        
        if (updateHasRun.load() && !isUnmount && !globalIsoFileList.empty()) {
            std::thread(refreshListAfterAutoUpdate, 1, std::ref(isAtISOList), 
                        std::ref(isImportRunning), std::ref(updateHasRun), std::ref(umountMvRmBreak),
                        std::ref(filteredFiles), std::ref(isFiltered), std::ref(listSubtype), std::ref(newISOFound)).detach();
        }
        
        
        std::cout << "\033[1A\033[K";
        
        // Generate prompt
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001" : "\001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001")
                           + operationColor + "\002" + operation 
                           + "\001\033[1;94m\002, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        
        if (!input.get())
            break;

        std::string inputString(input.get());
        
        // Handle special commands
        if (inputString == "?") {
            isAtISOList.store(false);
            helpSelections();
            needsClrScrn = true;
            continue;
        }

        if (inputString == "~") {
            // Toggle full list display based on operation type
            if (isMount) displayConfig::toggleFullListMount = !displayConfig::toggleFullListMount;
            else if (isUnmount) displayConfig::toggleFullListUmount = !displayConfig::toggleFullListUmount;
            else if (write) displayConfig::toggleFullListWrite = !displayConfig::toggleFullListWrite;
            else displayConfig::toggleFullListCpMvRm = !displayConfig::toggleFullListCpMvRm;
            needsClrScrn = true;
            continue;
        }

        // Handle empty input or return
        if (inputString.empty()) {
            if (isFiltered) {
                std::vector<std::string>().swap(filteredFiles);
                isFiltered = false;
                continue;
            } else {
                return;
            }
        }

        // Handle filtering operations
        if (inputString == "/" || (inputString[0] == '/' && inputString.length() > 1)) {
            bool isFilterPrompt = (inputString == "/");
            std::string searchString;
            
            if (isFilterPrompt) {
                // Interactive filter prompt
                while (true) {
                    resetVerboseSets(operationFiles, skippedMessages, operationFails, uniqueErrorMessages);

                    filterHistory = true;
                    loadHistory(filterHistory);
                    std::cout << "\033[1A\033[K";

                    std::string filterPrompt = "\001\033[1;38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + 
                                              operationColor + "\002" + operation + 
                                              "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";
                    std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);

                    if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                        
                        clear_history();
                        needsClrScrn = isFiltered ? true : false;
                        break;
                    }

                    searchString = searchQuery.get();
                    
                    // Perform filtering using the improved function
                    const std::vector<std::string>& sourceList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
                    
                    if (!searchString.empty()) {
                        auto newFilteredFiles = filterFiles(sourceList, searchString);
                        
                        bool filterUnchanged = newFilteredFiles.size() == sourceList.size();
                        
                        if (!filterUnchanged && !newFilteredFiles.empty()) {
                            add_history(searchQuery.get());
                            saveHistory(filterHistory);
                            needsClrScrn = true;
                            filteredFiles = std::move(newFilteredFiles);
                            isFiltered = true;
                            clear_history();
                            break;
                        }
                    }
                    clear_history();
                }
            } else {
                // Quick filter with /pattern
                searchString = inputString.substr(1);
                
                if (!searchString.empty()) {
                    const std::vector<std::string>& sourceList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
                    auto newFilteredFiles = filterFiles(sourceList, searchString);
                    
                    bool filterUnchanged = newFilteredFiles.size() == sourceList.size();
                    
                    if (!filterUnchanged && !newFilteredFiles.empty()) {
                        filterHistory = true;
                        loadHistory(filterHistory);
                        add_history(searchString.c_str());
                        saveHistory(filterHistory);
                        filteredFiles = std::move(newFilteredFiles);
                        isFiltered = true;
                        needsClrScrn = true;
                    }
                }
            }
            continue;
        }

        // Process operation for selected files
        processOperationForSelectedIsoFiles(inputString, isMount, isUnmount, write, isFiltered, 
                 filteredFiles, isoDirs, operationFiles, 
                 operationFails, uniqueErrorMessages, skippedMessages, verbose,
                 needsClrScrn, operation, isAtISOList, umountMvRmBreak, 
                 promptFlag, maxDepth, newISOFound);
    }
}


// Function to process operations from selectIsoFiles
void processOperationForSelectedIsoFiles(const std::string& inputString,bool isMount, bool isUnmount, bool write, bool isFiltered, const std::vector<std::string>& filteredFiles, std::vector<std::string>& isoDirs, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& skippedMessages, bool verbose, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool promptFlag, int& maxDepth, std::atomic<bool>& newISOFound) {
    
    clearScrollBuffer();
    // Default flags
    needsClrScrn = true;

    if (isMount) {
        isAtISOList.store(false);
        // Use const reference instead of copying
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
        processAndMountIsoFiles(inputString, activeList, operationFiles, skippedMessages, operationFails, uniqueErrorMessages, verbose);
    } else if (isUnmount) {
        umountMvRmBreak = true;
        isAtISOList.store(false);
        // For unmount operations, the list might need to be modified
        if (isFiltered) {
            prepareUnmount(inputString, filteredFiles, operationFiles, operationFails, uniqueErrorMessages, umountMvRmBreak, verbose);
        } else {
            prepareUnmount(inputString, isoDirs, operationFiles, operationFails, uniqueErrorMessages, umountMvRmBreak, verbose);
        }
    } else if (write) {
        isAtISOList.store(false);
        // Use const reference instead of copying
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
        writeToUsb(inputString, activeList, uniqueErrorMessages);
    } else {
        isAtISOList.store(false);
        // Disable filter history
        bool filterHistory = false;
        // Use const reference instead of copying
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : globalIsoFileList;
         processOperationInput(inputString, activeList, operation, operationFiles, operationFails, uniqueErrorMessages, promptFlag, maxDepth, umountMvRmBreak, filterHistory, verbose, newISOFound);
    }

    handleSelectIsoFilesResults(uniqueErrorMessages, operationFiles, operationFails, skippedMessages, operation, 
                                  verbose, isMount, isFiltered, umountMvRmBreak, isUnmount, needsClrScrn);
}


// Function to process results from selectIsoFiles
void handleSelectIsoFilesResults(std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& skippedMessages, const std::string& operation, bool verbose, bool isMount, bool isFiltered, bool umountMvRmBreak, bool isUnmount, bool& needsClrScrn) {
    // Result handling and display
    if (!uniqueErrorMessages.empty() && operationFiles.empty() && operationFails.empty() && skippedMessages.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::cout << "\n\033[1;91mNo valid input provided.\033[0;1m\n\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } else if (verbose) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::unordered_set<std::string> conditionalSet = isMount ? skippedMessages : std::unordered_set<std::string>{};
        std::unordered_set<std::string> emptySet{};
        verbosePrint(operationFiles, operationFails, conditionalSet, uniqueErrorMessages, isMount ? 2 : 1);
    }

    // Reset filter for certain operations
    if ((operation == "mv" || operation == "rm" || operation == "umount") && isFiltered && umountMvRmBreak) {
        clear_history();
        needsClrScrn = true;
    }

    // For non-umount operations, if there are no ISOs in the global list, inform the user.
    if (!isUnmount && globalIsoFileList.empty()) {
        clearScrollBuffer();
        needsClrScrn = true;
        std::cout << "\n\033[1;93mNo ISO available for " << operation << ".\033[0m\n\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }
}


// General function to tokenize input strings
void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<int>& processedIndices) {
    std::istringstream iss(input);
    std::string token;

    std::unordered_set<std::string> invalidInputs;
    std::unordered_set<std::string> invalidIndices;
    std::unordered_set<std::string> invalidRanges;

    while (iss >> token) {
        if (startsWithZero(token)) {
            invalidIndices.insert(token);
            continue;
        }

        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInputs.insert(token);
            continue;
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;
            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument&) {
                invalidInputs.insert(token);
                continue;
            } catch (const std::out_of_range&) {
                invalidRanges.insert(token);
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size() || start == 0 || end == 0) {
                invalidRanges.insert(token);
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                if (i >= 1 && i <= static_cast<int>(isoFiles.size())) {
                    if (processedIndices.find(i) == processedIndices.end()) {
                        processedIndices.insert(i);
                    }
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                if (processedIndices.find(num) == processedIndices.end()) {
                    processedIndices.insert(num);
                }
            } else {
                invalidIndices.insert(token);
            }
        } else {
            invalidInputs.insert(token);
        }
    }

    // Helper to format error messages with pluralization
    auto formatCategory = [](const std::string& singular, const std::string& plural,
                            const std::unordered_set<std::string>& items) {
        if (items.empty()) return std::string();
        std::ostringstream oss;
        oss << "\033[1;91m" << (items.size() > 1 ? plural : singular) << ": '";
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (it != items.begin()) oss << " ";
            oss << *it;
        }
        oss << "'.\033[0;1m";
        return oss.str();
    };

    // Add formatted messages with conditional pluralization
    if (!invalidInputs.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid input", "Invalid inputs", invalidInputs));
    }
    if (!invalidIndices.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid index", "Invalid indexes", invalidIndices));
    }
    if (!invalidRanges.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid range", "Invalid ranges", invalidRanges));
    }
}


// Function to get the total size of files
size_t getTotalFileSize(const std::vector<std::string>& files) {
    size_t totalSize = 0;
    for (const auto& file : files) {
        struct stat st;
        if (stat(file.c_str(), &st) == 0) {
            totalSize += st.st_size;
        }
    }
    return totalSize;
}


// Function to display progress bar for native operations
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose) {
    // Structs to handle terminal settings for non-blocking input
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);  // Get current terminal settings
    newt = oldt;  // Copy old settings to new
    newt.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply new settings

    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);  // Get current file descriptor flags
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);  // Set non-blocking mode

    const int barWidth = 50;  // Width of the progress bar
    bool enterPressed = false;  // Flag to detect if enter is pressed
    auto startTime = std::chrono::high_resolution_clock::now();  // Record start time

    std::stringstream ssFormatter;
    auto formatSize = [&ssFormatter](double bytes) -> std::string {
        const char* units[] = {" B", " KB", " MB", " GB"};  // Define units for sizes
        int unit = 0;
        double size = bytes;
        while (size >= 1024 && unit < 3) {
            size /= 1024;  // Convert to larger units if size exceeds 1024
            unit++;
        }
        ssFormatter.str("");
        ssFormatter.clear();
        ssFormatter << std::fixed << std::setprecision(2) << size << units[unit];  // Format the size
        return ssFormatter.str();
    };

    const bool bytesTrackingEnabled = (completedBytes != nullptr);  // Check if byte tracking is enabled
    std::string totalBytesFormatted;
    if (bytesTrackingEnabled) {
        totalBytesFormatted = formatSize(static_cast<double>(totalBytes));  // Format total bytes
    }

    // Main loop to update progress bar
    while (!isComplete->load(std::memory_order_acquire) || !enterPressed) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0);  // Non-blocking read to check for input

        // Load current progress information
        const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = bytesTrackingEnabled ? completedBytes->load(std::memory_order_acquire) : 0;

        // Check if all tasks are processed
        const bool allTasksProcessed = (completedTasksValue + failedTasksValue) >= totalTasks;
        if (allTasksProcessed) {
            isComplete->store(true, std::memory_order_release);  // Mark as complete if all tasks are done
        }

        // Calculate task and byte progress
        double tasksProgress = static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        double overallProgress = tasksProgress;
        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = std::max(bytesProgress, tasksProgress);  // Use the maximum of task and byte progress
        }

        // Calculate the position of the progress bar
        int progressPos = static_cast<int>(barWidth * overallProgress);

        // Calculate elapsed time and speed
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        double elapsedSeconds = elapsedTime.count() / 1000.0;
        double speed = bytesTrackingEnabled 
            ? (elapsedSeconds > 0.0 ? (static_cast<double>(completedBytesValue) / elapsedSeconds) : 0.0)
            : 0.0;

        // Construct the progress bar display
        std::stringstream ss;
        ss << "\r[";  // Start the progress bar
        for (int i = 0; i < barWidth; ++i) {
            ss << (i < progressPos ? "=" : (i == progressPos ? ">" : " "));  // Fill the bar based on progress
        }
        ss << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
            << "% (" << completedTasksValue << "/" << totalTasks << ")";

        // Add byte and speed information if enabled
        if (bytesTrackingEnabled) {
            ss << " (" << formatSize(static_cast<double>(completedBytesValue)) << "/" << totalBytesFormatted << ") "
                << formatSize(speed) << "/s";
        }

        // Add elapsed time
        ss << " Time Elapsed: " << std::fixed << std::setprecision(1) << elapsedSeconds << "s\033[K";
        std::cout << ss.str() << std::flush;

        // If processing is complete, show a final message
        if (isComplete->load(std::memory_order_acquire)) {
            std::cout << "\r[==================================================>] 100% ("
                      << completedTasks->load() << "/" << totalTasks << ") "
                      << (bytesTrackingEnabled ? "(" + formatSize(static_cast<double>(completedBytes->load())) + "/" + totalBytesFormatted + ") " : "")
                      << "Time Elapsed: " << std::fixed << std::setprecision(1) << (elapsedSeconds) << "s\033[K";
        }

        // Wait for user input (if needed)
        if (isComplete->load(std::memory_order_acquire) && !enterPressed) {
            // Disable certain key bindings temporarily
            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);
            rl_bind_keyseq("\033[A", prevent_readline_keybindings);
            rl_bind_keyseq("\033[B", prevent_readline_keybindings);

            enterPressed = true;
            std::cout << "\n\n";
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore terminal settings
            fcntl(STDIN_FILENO, F_SETFL, oldf);  // Restore file descriptor flags

            const std::string prompt = "\033[1;94mDisplay verbose output? (y/n):\033[0;1m ";
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);  // Prompt for verbose output

            if (input.get()) {
                *verbose = (std::string(input.get()) == "y" || std::string(input.get()) == "Y");
            }

            // Restore key bindings after the prompt
            rl_bind_keyseq("\033[A", rl_get_previous_history);
            rl_bind_keyseq("\033[B", rl_get_next_history);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Sleep for a short duration before checking progress again
        }
    }

    std::cout << std::endl;
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Final restoration of terminal settings
    fcntl(STDIN_FILENO, F_SETFL, oldf);  // Final restoration of file descriptor flags
}


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType) {
    static const char* defaultColor = "\033[0m";
    static const char* bold = "\033[1m";
    static const char* reset = "\033[0m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* blueBold = "\033[94;1m";
    static const char* magenta = "\033[95m";
    static const char* magentaBold = "\033[95;1m";
    static const char* orangeBold = "\033[1;38;5;208m";
    static const char* grayBold = "\033[38;5;245m";
        
    size_t maxIndex = items.size();
    size_t numDigits = std::to_string(maxIndex).length();

    // Precompute padded index strings
    std::vector<std::string> indexStrings(maxIndex);
    for (size_t i = 0; i < maxIndex; ++i) {
        indexStrings[i] = std::to_string(i + 1);
        indexStrings[i].insert(0, numDigits - indexStrings[i].length(), ' ');
    }

    std::ostringstream output;
    output << "\n"; // Initial newline for visual spacing

    for (size_t i = 0; i < items.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? red : green;
        std::string directory, filename, displayPath, displayHash;

        if (listType == "ISO_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i], listSubType);
            directory = dir;
            filename = fname;
        } else if (listType == "MOUNTED_ISOS") {
			std::string dirName = items[i];
    
			// Find the position of the first underscore
			size_t firstUnderscorePos = dirName.find('_');
    
			// Find the position of the last tilde
			size_t lastTildePos = dirName.find_last_of('~');
    
			// Extract displayPath (after first underscore and before last tilde)
			if (firstUnderscorePos != std::string::npos && lastTildePos != std::string::npos && lastTildePos > firstUnderscorePos) {
				displayPath = dirName.substr(firstUnderscorePos + 1, lastTildePos - (firstUnderscorePos + 1));
			} else {
				// If the conditions are not met, use the entire dirName (or handle it as needed)
				displayPath = dirName;
			}
    
			// Extract displayHash (from last tilde to the end, including the last tilde)
			if (lastTildePos != std::string::npos) {
				displayHash = dirName.substr(lastTildePos); // Start at lastTildePos instead of lastTildePos + 1
			} else {
				// If no tilde is found, set displayHash to an empty string (or handle it as needed)
				displayHash = "";
			}
		} else if (listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i], "conversions");

            bool isSpecialExtension = false;
            std::string extension = fname;
            size_t dotPos = extension.rfind('.');

            if (dotPos != std::string::npos) {
                extension = extension.substr(dotPos);
                toLowerInPlace(extension);
                isSpecialExtension = (extension == ".bin" || extension == ".img" ||
                                      extension == ".mdf" || extension == ".nrg");
            }

            if (isSpecialExtension) {
                directory = dir;
                filename = fname;
                sequenceColor = orangeBold;
            }
        }

        // Build output based on listType
        if (listType == "ISO_FILES") {
            output << sequenceColor << indexStrings[i] << ". "
                   << defaultColor << bold << directory
                   << defaultColor << bold << "/"
                   << magenta << filename << defaultColor << "\n";
        } else if (listType == "MOUNTED_ISOS") {
			if (displayConfig::toggleFullListUmount){
            output << sequenceColor << indexStrings[i] << ". "
                   << blueBold << "/mnt/iso_"
                   << magentaBold << displayPath << grayBold << displayHash << reset << "\n";
			} else {
				output << sequenceColor << indexStrings[i] << ". "
                   << magentaBold << displayPath << "\n";
			}
        } else if (listType == "IMAGE_FILES") {
		// Alternate sequence color like in "ISO_FILES"
		const char* sequenceColor = (i % 2 == 0) ? red : green;
    
			if (directory.empty() && filename.empty()) {
				// Standard case
				output << sequenceColor << indexStrings[i] << ". "
				<< reset << bold << items[i] << defaultColor << "\n";
			} else {
				// Special extension case (keep the filename sequence as orange bold)
				output << sequenceColor << indexStrings[i] << ". "
					<< reset << bold << directory << "/"
					<< orangeBold << filename << defaultColor << "\n";
			}
        }
    }

    std::cout << output.str();
}


// Hold valid input for general use
const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"},
    {'u', "umount_list"},
    {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"},
    {'w', "write_list"}
};


// Function to validate input dynamically
bool isValidInput(const std::string& input) {
    // Check if input starts with *cl or *fl
    if (input.size() < 4 || input[0] != '*' || 
        (input.substr(1, 2) != "cl" && input.substr(1, 2) != "fl")) {
        return false;
    }

    // Check for underscore and at least one setting character
    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) {
        return false;
    }

    // Validate each setting character
    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) {
        if (settingMap.find(c) == settingMap.end()) {
            return false;
        }
    }

    return true;
}


// Function to write default display modes to config file
void setDisplayMode(const std::string& inputSearch) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    std::vector<std::string> configLines;
    std::vector<std::string> settingKeys;
    bool validInput = true;
    std::string newValue;

    // Read existing config lines
    std::ifstream inFile(configPath);
    if (inFile.is_open()) {
        std::string line;
        while (std::getline(inFile, line)) {
            configLines.push_back(line);
        }
        inFile.close();
    }

    // Create directory if needed
    std::filesystem::path dirPath = std::filesystem::path(configPath).parent_path();
    if (!std::filesystem::exists(dirPath)) {
        if (!std::filesystem::create_directories(dirPath)) {
            std::cerr << "\n\033[1;91mFailed to create directory: \033[1;93m'" 
                      << dirPath.string() << "'\033[1;91m.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }

    // Parse input command and settings
    if (inputSearch.size() < 4 || inputSearch[0] != '*' || 
        (inputSearch.substr(1, 2) != "cl" && inputSearch.substr(1, 2) != "fl")) {
        std::cerr << "\n\033[1;91mInvalid input format. Use '*cl' or '*fl' prefix.\033[0;1m\n";
        validInput = false;
    } else {
        std::string command = inputSearch.substr(1, 2);
        size_t underscorePos = inputSearch.find('_', 3);
        if (underscorePos == std::string::npos || underscorePos + 1 >= inputSearch.size()) {
            std::cerr << "\n\033[1;91mExpected '_' followed by settings (e.g., *cl_mu).\033[0;1m\n";
            validInput = false;
        } else {
            std::string settingsStr = inputSearch.substr(underscorePos + 1);
            newValue = (command == "cl") ? "compact" : "full";

            std::unordered_set<std::string> uniqueKeys;
            for (char c : settingsStr) {
                auto it = settingMap.find(c);
                if (it != settingMap.end()) {
                    const std::string& key = it->second;
                    if (uniqueKeys.insert(key).second) {
                        settingKeys.push_back(key);
                    }
                } else {
                    std::cerr << "\n\033[1;91mInvalid setting character: '" << c << "'.\033[0;1m\n";
                    validInput = false;
                    break;
                }
            }
        }
    }

    if (!validInput || settingKeys.empty()) {
        if (validInput) std::cerr << "\n\033[1;91mNo valid settings specified.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Update config lines for each setting
    std::unordered_set<std::string> unprocessedSettings(settingKeys.begin(), settingKeys.end());
    for (auto& line : configLines) {
        for (auto it = unprocessedSettings.begin(); it != unprocessedSettings.end();) {
            const std::string& settingKey = *it;
            if (line.find(settingKey + " =") == 0) {
                line = settingKey + " = " + newValue;
                it = unprocessedSettings.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Add new settings if they didn't exist
    for (const auto& settingKey : unprocessedSettings) {
        configLines.push_back(settingKey + " = " + newValue);
    }

    // Write updated config to file
    std::ofstream outFile(configPath);
    if (outFile.is_open()) {
        for (const auto& line : configLines) {
            outFile << line << "\n";
        }
        outFile.close();

        // Update toggle flags for each affected setting
        for (const auto& settingKey : settingKeys) {
            if (settingKey == "mount_list") {
                displayConfig::toggleFullListMount = (newValue == "full");
            } else if (settingKey == "umount_list") {
                displayConfig::toggleFullListUmount = (newValue == "full");
            } else if (settingKey == "cp_mv_rm_list") {
                displayConfig::toggleFullListCpMvRm = (newValue == "full");
            } else if (settingKey == "conversion_lists") {
                displayConfig::toggleFullListConversions = (newValue == "full");
            } else if (settingKey == "write_list") {
                displayConfig::toggleFullListWrite = (newValue == "full");
            }
        }

        // Display confirmation
        std::cout << "\n\033[0;1mDisplay mode set to \033[1;92m" << newValue << "\033[0;1m for:\n";
        for (const auto& key : settingKeys) {
            std::cout << "  - " << key << "\n";
        }
        std::cout << "\033[0;1m";
    } else {
        std::cerr << "\n\033[1;91mFailed to write to config file: \033[1;93m'" 
                  << configPath << "'\033[1;91m.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Trim function to remove leading and trailing whitespaces and spaces between semicolons
std::string trimWhitespace(const std::string& str) {
    // Step 1: Trim leading and trailing spaces
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    
    if (first == std::string::npos || last == std::string::npos)
        return "";
    
    std::string trimmed = str.substr(first, (last - first + 1));
    
    // Step 2: Remove spaces around semicolons
    std::string result;
    for (size_t i = 0; i < trimmed.length(); ++i) {
        if (trimmed[i] == ' ') {
            // Skip spaces if they are before or after a semicolon
            bool isSpaceBeforeSemicolon = (i + 1 < trimmed.length() && trimmed[i + 1] == ';');
            bool isSpaceAfterSemicolon = (i > 0 && trimmed[i - 1] == ';');
            
            if (!isSpaceBeforeSemicolon && !isSpaceAfterSemicolon) {
                result += ' ';
            }
        } else if (trimmed[i] == ';') {
            // Add the semicolon and skip any spaces immediately before or after
            result += ';';
        } else {
            // Add non-space, non-semicolon characters
            result += trimmed[i];
        }
    }
    
    return result;
}


// Function to display how to select items from lists
void helpSelections() {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For Lists =====\033[0m\n" << std::endl;
    
    // Working with indices
    std::cout << "\033[1;32m1. Selecting Items:\033[0m\n"
              << "   • Single item: Enter a number (e.g., '1')\n"
              << "   • Multiple items: Separate with spaces (e.g., '1 5 6')\n"
              << "   • Range of items: Use a hyphen (e.g., '1-3')\n"
              << "   • Combine methods: '1-3 5 7-9'\n"
              << "   • Select all: Enter '00' (for mount/umount only)\n" << std::endl;
    
    // Special commands
    std::cout << "\033[1;32m2. Special Commands:\033[0m\n"
			  << "   • Enter \033[1;34m'~'\033[0m - Switch between compact and full list\n"
              << "   • Enter \033[1;34m'/'\033[0m - Filter the current list based on search terms (e.g., 'term' or 'term1;term2')\n"
              << "   • Enter \033[1;34m'/term1;term2'\033[0m - Directly filter the list for items containing 'term1' and 'term2'\n" << std::endl;
     // Selection tips
    std::cout << "\033[1;32m3. Tips:\033[0m\n"
              << "   • To quickly return from filtered lists to pre-selection, press \033[1;93mCtrl+d\033[0m\n"
              << "   • If filtering has no matches, no message or list update is issued\n" << std::endl;
              
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for directory prompts
void helpSearches(bool isCpMv, bool import2ISO) {
    std::signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
    disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For " 
          << (isCpMv ? "Cp/Mv FolderPath" : (import2ISO ? "Import2ISO FolderPath" : "Convert2ISO FolderPath")) 
          << " Prompt =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m" << (isCpMv ? " " : "1. ") << "Selecting FolderPaths:\033[0m\n"
              << (isCpMv ? " " : "   ") <<"• Single directory: Enter a directory (e.g., '/directory/')\n"
              << (isCpMv ? " " : "   ") <<"• Multiple directories: Separate with ; (e.g., '/directory1/;/directory2/')" << std::endl;
    if (isCpMv) {
        std::cout << " • Overwrite files for cp/mv: Append -o (e.g., '/directory/ -o' or '/directory1/;/directory2/ -o')\n" << std::endl;
    }
    if (!isCpMv) {
        std::cout << "\n\033[1;32m2. Special Cleanup Commands:\033[0m\n";
        if (!import2ISO) {
            std::cout << "   • Enter \033[1;33m'!clr'\033[0m - Clear corresponding RAM cache\n";
        }
        if (import2ISO) {
            std::cout << "   • Enter \033[1;33m'!clr'\033[0m - Clear on-disk ISO cache\n";
        }
		std::cout << "   • Enter \033[1;33m'!clr_paths'\033[0m - Clear folder path history\n"
				  << "   • Enter \033[1;33m'!clr_filter'\033[0m - Clear filter history\n" << std::endl;
		std::cout << "\033[1;32m3. Special Display Command:\033[0m\n";
        if (!import2ISO) {
            std::cout << "   • Enter \033[1;34m'ls'\033[0m - List corresponding cached entries\n\n";
        }
        if (import2ISO) {
            std::cout << "   • Enter \033[1;34m'stats'\033[0m - View on-disk ISO cache statistics\n" << std::endl;
        }
					
       std::cout << "\033[1;32m" << "4. Special Configuration Commands:\033[0m\n\n";
       
		if (import2ISO) { 
			std::cout << "   \033[1;38;5;208mA. Auto-Update ISO Cache:\033[0m\n"
                      << "      • Enter \033[1;35m'*auto_on'\033[0m or \033[1;35m'*auto_off'\033[0m - Enable/Disable ISO cache auto-update via stored folder paths (default: disabled)\n\n";
		}
				std::cout << "\033[1;38;5;208m   B. Set Default Display Modes (fl = full list, cl = compact list | default: cl, unmount → fl):\033[0m\n"
						<<  "      • Mount list:       Enter \033[1;35m'*fl_m'\033[0m or \033[1;35m'*cl_m'\033[0m\n"
						<<  "      • Umount list:      Enter \033[1;35m'*fl_u'\033[0m or \033[1;35m'*cl_u'\033[0m\n"
						<<  "      • cp/mv/rm list:    Enter \033[1;35m'*fl_o'\033[0m or \033[1;35m'*cl_o'\033[0m\n"
						<<  "      • Write list:       Enter \033[1;35m'*fl_w'\033[0m or \033[1;35m'*cl_w'\033[0m\n"
						<<  "      • Conversion lists: Enter \033[1;35m'*fl_c'\033[0m or \033[1;35m'*cl_c'\033[0m\n"
						<<  "      • Combine settings: Use multiple letters after \033[1;35m'*fl_'\033[0m or \033[1;35m'*cl_'\033[0m (e.g., \033[1;35m'*cl_mu'\033[0m for mount and umount lists)\n"
                  << std::endl;
    }
    
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for iso and device mapping
void helpMappings() {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For Mappings =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m Selecting Mappings:\033[0m\n"
			  << " • Mapping = NewISOIndex>RemovableUSBDevice\n"
              << " • Single mapping: Enter a mapping (e.g., '1>/dev/sdc')\n"
              << " • Multiple mappings: Separate with ; (e.g., '1>/dev/sdc;2>/dev/sdd' or '1>/dev/sdc;1>/dev/sdd')\n" << std::endl;
                  
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// For memory mapping string transformations
std::unordered_map<std::string, std::string> transformationCache;

// Function to extract directory and filename from a given path
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location) {
    // Use string_view for non-modifying operations
    static const std::array<std::pair<std::string_view, std::string_view>, 2> replacements = {{
        {"/home", "~"},
        {"/root", "/R"}
    }};

    // Find last slash efficiently
    auto lastSlashPos = path.find_last_of("/\\");
    if (lastSlashPos == std::string_view::npos) {
        return {"", std::string(path)};
    }

    // Early return for full list mode
    if (displayConfig::toggleFullListMount && location == "mount") {
        return {std::string(path.substr(0, lastSlashPos)), 
                std::string(path.substr(lastSlashPos + 1))};
    } else if (displayConfig::toggleFullListCpMvRm && location == "cp_mv_rm") {
		 return {std::string(path.substr(0, lastSlashPos)), 
                std::string(path.substr(lastSlashPos + 1))};
	} else if (displayConfig::toggleFullListConversions && location == "conversions") {
		return {std::string(path.substr(0, lastSlashPos)), 
                std::string(path.substr(lastSlashPos + 1))};
	} else if (displayConfig::toggleFullListWrite && location == "write") {
		return {std::string(path.substr(0, lastSlashPos)), 
                std::string(path.substr(lastSlashPos + 1))};
	}

    // Check cache first
    auto cacheIt = transformationCache.find(std::string(path));
    if (cacheIt != transformationCache.end()) {
        return {cacheIt->second, std::string(path.substr(lastSlashPos + 1))};
    }

    // Optimize directory shortening
    std::string processedDir;
    processedDir.reserve(path.length() / 2);  // More conservative pre-allocation

    size_t start = 0;
    while (start < lastSlashPos) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = lastSlashPos;

        // More efficient component truncation
        size_t componentLength = end - start;
        size_t truncatePos = std::min({
            componentLength, 
            path.find(' ', start) - start,
            path.find('-', start) - start,
            path.find('_', start) - start,
            path.find('.', start) - start,
            size_t(16)
        });

        processedDir.append(path.substr(start, truncatePos));
        processedDir.push_back('/');
        start = end + 1;
    }

    if (!processedDir.empty()) {
        processedDir.pop_back();  // Remove trailing slash

        // More efficient replacements using string_view
        for (const auto& [oldDir, newDir] : replacements) {
            size_t pos = 0;
            while ((pos = processedDir.find(oldDir, pos)) != std::string::npos) {
                processedDir.replace(pos, oldDir.length(), newDir);
                pos += newDir.length();
            }
        }
    }

    // Cache the result
    transformationCache[std::string(path)] = processedDir;

    return {processedDir, std::string(path.substr(lastSlashPos + 1))};
}
