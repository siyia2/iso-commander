// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// For storing isoFiles in RAM
std::vector<std::string> globalIsoFileList;

// Mutex to prevent race conditions when live updating ISO list
std::mutex updateListMutex;

// Function to automatically update on-disk cache if auto-update is on
void refreshListAfterAutoUpdate(int timeoutSeconds, std::atomic<bool>& isAtISO, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateRun, std::vector<std::string>& filteredFiles, std::vector<std::string>& sourceList, bool& isFiltered, std::string& listSubtype, std::atomic<bool>& newISOFound) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));
        
        if (!isImportRunning.load()) {
			if (newISOFound.load() && isAtISO.load()) {
				clearAndLoadFiles(filteredFiles, isFiltered, listSubtype);
				
				{
					std::lock_guard<std::mutex> lock(updateListMutex);
					sourceList = isFiltered ? filteredFiles : globalIsoFileList;  // Update sourceList
				}
            
				std::cout << "\n";
				rl_on_new_line(); 
				rl_redisplay();
			}
            updateRun.store(false);
            newISOFound.store(false);
            
            break;
        }
    }
}


// Main function to select and operate on ISOs by number for umount mount cp mv and rm
void selectForIsoFiles(const std::string& operation, bool& historyPattern, int& maxDepth, bool& verbose, std::atomic<bool>& updateRun, std::atomic<bool>& isAtISO, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    // Bind readline keys
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    
    std::set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
    std::vector<std::string> filteredFiles, sourceList;
    
    globalIsoFileList.reserve(100);
    sourceList.reserve(100);
    filteredFiles.reserve(100);
    
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool umountMvRmBreak = false;

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
    bool promptFlag = false;
    
    std::string listSubtype = isMount ? "mount" : (write ? "write" : "cp_mv_rm");
        
    while (true) {
		if (!isUnmount) isAtISO.store(true);
        verbose = false;
        operationFiles.clear();
        skippedMessages.clear();
        operationFails.clear();
        uniqueErrorMessages.clear();
        clear_history();
        removeNonExistentPathsFromCache();
        
        // Determine source list and load files based on operation type
        if (!isUnmount) {
            if (needsClrScrn) {
				if (!clearAndLoadFiles(filteredFiles, isFiltered, listSubtype)) break;
				{
					std::lock_guard<std::mutex> lock(updateListMutex);
					sourceList = isFiltered ? filteredFiles : globalIsoFileList;
				}
                std::cout << "\n\n";
            }
        } else {
            if (needsClrScrn) {
                if (!loadAndDisplayMountedISOs(sourceList, filteredFiles, isFiltered)) break;
                sourceList = isFiltered ? filteredFiles : sourceList;
                std::cout << "\n\n";
            }
        }
        
        if (updateRun.load() && !isUnmount && !globalIsoFileList.empty()) {
			std::thread(refreshListAfterAutoUpdate, 1, std::ref(isAtISO), 
				std::ref(isImportRunning), std::ref(updateRun), 
                std::ref(filteredFiles), std::ref(sourceList), 
                std::ref(isFiltered), std::ref(listSubtype), std::ref(newISOFound)).detach();
		}
        
        std::cout << "\033[1A\033[K";
        
        // Generate prompt
        std::string prompt = (isFiltered ? "\001\033[1;96m\002F⊳ \001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001" : "\001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001")
                           + operationColor + "\002" + operation 
                           + "\001\033[1;94m\002, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        
        // Handle input processing
        if (!input.get()) break;

        std::string inputString(input.get());
        
        // Help and toggle full list commands
        if (inputString == "?") {
			isAtISO.store(false);
            helpSelections();
            needsClrScrn = true;
            continue;
        }

        if (inputString == "~") {
            if (isMount) displayConfig::toggleFullListMount = !displayConfig::toggleFullListMount;
            else if (isUnmount) displayConfig::toggleFullListUmount = !displayConfig::toggleFullListUmount;
            else if (write) displayConfig::toggleFullListWrite = !displayConfig::toggleFullListWrite;
            else displayConfig::toggleFullListCpMvRm = !displayConfig::toggleFullListCpMvRm;
            continue;
        }

        // Handle empty input or return
        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                continue;
            } else {
                return;
            }
        }

        // Filtering logic with FilterTerms prompt
        if (inputString == "/") {
            while (true) {
                verbose = false;
                operationFiles.clear();
                skippedMessages.clear();
                operationFails.clear();
                uniqueErrorMessages.clear();

                historyPattern = true;
                loadHistory(historyPattern);
                // Move the cursor up 1 line and clear them
                std::cout << "\033[1A\033[K";

                // Generate filter prompt
                std::string filterPrompt = "\001\033[1;38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + 
                                           "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";
                std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);

                if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                    historyPattern = false;
                    clear_history();
                    if (isFiltered) {
                        needsClrScrn = true;
                    } else {
                        needsClrScrn = false;
                    }
                    break;
                }

                std::string inputSearch(searchQuery.get());
                
                // Filter files
                auto newFilteredFiles = filterFiles(isFiltered ? filteredFiles : sourceList, inputSearch);
                sortFilesCaseInsensitive(newFilteredFiles);

                // Check if filter is meaningful
                bool filterUnchanged = (isMount && newFilteredFiles.size() == globalIsoFileList.size()) ||
                                       (isUnmount && newFilteredFiles.size() == sourceList.size());
                
                if (!filterUnchanged && !newFilteredFiles.empty()) {
                    add_history(searchQuery.get());
                    saveHistory(historyPattern);
                    needsClrScrn = true;
                    filteredFiles = std::move(newFilteredFiles);
                    isFiltered = true;
                    historyPattern = false;
                    clear_history();
                    break;
                }
                historyPattern = false;
                clear_history();
            }
            continue;
        }

        // Quick filter when starting with '/'
        if (inputString[0] == '/' && inputString.length() > 1) {
            std::string searchTerm = inputString.substr(1);
            
            // Filter files
            auto newFilteredFiles = filterFiles(isFiltered ? filteredFiles : sourceList, searchTerm);
            sortFilesCaseInsensitive(newFilteredFiles);

            // Check if filter is meaningful
            bool filterUnchanged = (isMount && newFilteredFiles.size() == globalIsoFileList.size()) ||
                                   (isUnmount && newFilteredFiles.size() == sourceList.size());
            
            if (!filterUnchanged && !newFilteredFiles.empty()) {
                historyPattern = true;
                loadHistory(historyPattern);
                add_history(searchTerm.c_str());
                saveHistory(historyPattern);
                filteredFiles = std::move(newFilteredFiles);
                isFiltered = true;
                needsClrScrn = true;
            }
            continue;
        }

        // Operation processing
        clearScrollBuffer();
        needsClrScrn = true;
        
        if (isMount) {
            processAndMountIsoFiles(inputString, sourceList, operationFiles, skippedMessages, operationFails, uniqueErrorMessages, verbose);
        } else if (isUnmount) {
            umountMvRmBreak = true;
            prepareUnmount(inputString, sourceList, operationFiles, operationFails, uniqueErrorMessages, umountMvRmBreak, verbose);
        } else if (write) {
            writeToUsb(inputString, sourceList, uniqueErrorMessages);
        } else {
            processOperationInput(inputString, sourceList, operation, operationFiles, operationFails, uniqueErrorMessages, promptFlag, maxDepth, umountMvRmBreak, historyPattern, verbose, newISOFound);
        }

        // Result handling and display
        if (!uniqueErrorMessages.empty() && operationFiles.empty() && operationFails.empty() && skippedMessages.empty()) {
            clearScrollBuffer();
            needsClrScrn = true;
            std::cout << "\n\033[1;91mNo valid input provided for " << operation << ".\033[0;1m\n\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else if (verbose) {
            clearScrollBuffer();
            needsClrScrn = true;
            verbosePrint(operationFiles, operationFails, 
                         (isMount ? skippedMessages : std::set<std::string>{}), 
                         {}, uniqueErrorMessages, 
                         isMount ? 2 : 1);
        }

        // Reset filter for certain operations
        if ((operation == "mv" || operation == "rm" || operation == "umount") && isFiltered && umountMvRmBreak) {
            historyPattern = false;
            clear_history();
            isFiltered = false;
            needsClrScrn = true;
        }

        // Handle empty source list
        if (sourceList.empty()) {
            clearScrollBuffer();
            needsClrScrn = true;
            std::cout << "\n\033[1;93mNo ISO available for " << operation << ".\033[0m\n\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }
}


// General function to tokenize input strings
void tokenizeInput(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::set<int>& processedIndices) {
    std::istringstream iss(input);
    std::string token;

    std::set<std::string> invalidInputs;
    std::set<std::string> invalidIndices;
    std::set<std::string> invalidRanges;

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
                            const std::set<std::string>& items) {
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
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose) {
    // Set up non-blocking input
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Set stdin to non-blocking mode
    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    
    const int barWidth = 50;
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Precompute formatted total bytes string if applicable
    const bool bytesTrackingEnabled = (completedBytes != nullptr);
    std::string totalBytesFormatted;
    std::stringstream ssFormatter;
    if (bytesTrackingEnabled) {
        auto formatSize = [&ssFormatter](size_t bytes) -> std::string {
            const char* units[] = {" B", " KB", " MB", " GB"};
            int unit = 0;
            double size = static_cast<double>(bytes);
            while (size >= 1024 && unit < 3) {
                size /= 1024;
                unit++;
            }
            ssFormatter.str("");
            ssFormatter.clear();
            ssFormatter << std::fixed << std::setprecision(2) << size << units[unit];
            return ssFormatter.str();
        };
        totalBytesFormatted = formatSize(totalBytes);
    }

    // Reusable components
    auto formatSize = [&ssFormatter](size_t bytes) -> std::string {
        const char* units[] = {" B", " KB", " MB", " GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        ssFormatter.str("");
        ssFormatter.clear();
        ssFormatter << std::fixed << std::setprecision(2) << size << units[unit];
        return ssFormatter.str();
    };

    try {
        while (!isComplete->load(std::memory_order_relaxed) || !enterPressed) {
            // Discard any input during progress update
            char ch;
            while (read(STDIN_FILENO, &ch, 1) > 0);

            // Load atomics once per iteration
            const size_t completedTasksValue = completedTasks->load(std::memory_order_relaxed);
            const size_t completedBytesValue = bytesTrackingEnabled ? completedBytes->load(std::memory_order_relaxed) : 0;

            // Calculate progress
            const double tasksProgress = static_cast<double>(completedTasksValue) / totalTasks;
            double overallProgress = tasksProgress;
            if (bytesTrackingEnabled) {
                const double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
                overallProgress = std::max(bytesProgress, tasksProgress);
            }
            const int progressPos = static_cast<int>(barWidth * overallProgress);

            // Calculate timing and speed
            const auto currentTime = std::chrono::high_resolution_clock::now();
            const auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
            const double elapsedSeconds = elapsedTime.count() / 1000.0;
            const double speed = bytesTrackingEnabled ? (completedBytesValue / elapsedSeconds) : 0;

            // Build output string efficiently
            std::stringstream ss;
            ss << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                ss << (i < progressPos ? "=" : (i == progressPos ? ">" : " "));
            }
            ss << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
               << "% (" << completedTasksValue << "/" << totalTasks << ")";

            if (bytesTrackingEnabled) {
                ss << " (" << formatSize(completedBytesValue) << "/" << totalBytesFormatted << ") "
                   << formatSize(static_cast<size_t>(speed)) << "/s";
            }

            ss << " Time Elapsed: " << std::fixed << std::setprecision(1) << elapsedSeconds << "s\033[K";
            std::cout << ss.str() << std::flush;

            // Check completion condition
            if (completedTasksValue >= totalTasks && !enterPressed) {
                rl_bind_key('\f', prevent_readline_keybindings);
                rl_bind_key('\t', prevent_readline_keybindings);
                rl_bind_keyseq("\033[A", prevent_readline_keybindings);
                rl_bind_keyseq("\033[B", prevent_readline_keybindings);

                enterPressed = true;
                std::cout << "\n\n";
                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                fcntl(STDIN_FILENO, F_SETFL, oldf);

                const std::string prompt = "\033[1;94mDisplay verbose output? (y/n):\033[0;1m ";
                std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
                
                if (input.get()) {
                    *verbose = (std::string(input.get()) == "y" || std::string(input.get()) == "Y");
                }

                rl_bind_keyseq("\033[A", rl_get_previous_history);
                rl_bind_keyseq("\033[B", rl_get_next_history);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } catch (...) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fcntl(STDIN_FILENO, F_SETFL, oldf);
        throw;
    }

    std::cout << std::endl;
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
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


// Valid input for isValidInput
const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"},
    {'u', "umount_list"},
    {'f', "cp_mv_rm_list"},
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


// Function to write default display modes toconfig file
void setDisplayMode(const std::string& inputSearch) {
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

            // Map characters to settings (e.g., 'm' → mount_list)
            std::unordered_map<char, std::string> settingMap = {
                {'m', "mount_list"},
                {'u', "umount_list"},
                {'f', "cp_mv_rm_list"},
                {'c', "conversion_lists"},
                {'w', "write_list"}
            };

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


// Function to clear path and filter history
void clearHistory(const std::string& inputSearch) {
    const std::string basePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/";
    std::string filePath;
    std::string historyType;

    if (inputSearch == "!clr_paths") {
        filePath = basePath + "iso_commander_history_cache.txt";
        historyType = "Path";
    } else if (inputSearch == "!clr_filter") {
        filePath = basePath + "iso_commander_filter_cache.txt";
        historyType = "Filter";
    } else {
        std::cerr << "\n\001\033[1;91mInvalid command: \001\033[1;93m'" 
                  << inputSearch << "'\001\033[1;91m." << std::endl;
        return;
    }

    if (std::remove(filePath.c_str()) != 0) {
        std::cerr << "\n\001\033[1;91mError clearing " << historyType << " history: \001\033[1;93m'" 
                  << filePath << "'\001\033[1;91m. File missing or inaccessible." << std::endl;
    } else {
        std::cout << "\n\001\033[1;92m" << historyType << " history cleared successfully." << std::endl;
        clear_history();
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to display how to select items from lists
void helpSelections() {
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
              << "   • Enter \033[1;34m'/term1;term2'\033[0m - Directly filter the list for items containing 'term1' and 'term2'\n\n"
              << "   - Note: If filtering has no matches, no message or list update is issued\n" << std::endl;
              
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for directory prompts
void helpSearches(bool isCpMv) {
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For FolderPath Prompts =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m1. Selecting FolderPaths:\033[0m\n"
              << "   • Single directory: Enter a directory (e.g., '/directory/')\n"
              << "   • Multiple directories: Separate with ; (e.g., '/directory1/;/directory2/')\n"
              << "   • Overwrite files for cp/mv: Append |^O (e.g., '/directory/ |^O' or '/directory1/;/directory2/ |^O')\n" << std::endl;
    if (isCpMv) std::cout << "   - Note: Special Commands are only available within Convert2ISO&ImportISO FolderPath prompts \n" << std::endl;
    if (!isCpMv) {
		std::cout << "\033[1;32m2. Special Cleanup Commands:\033[0m\n"
				<< "   • Enter \033[1;33m'!clr'\033[0m - Clear cache:\n"
				<< "     - In Convert2ISO: Clears corresponding RAM cache\n"
				<< "     - In ImportISO: Clears on-disk ISO cache\n"
				<< "   • Enter \033[1;33m'!clr_paths'\033[0m - Clear folder path history\n"
				<< "   • Enter \033[1;33m'!clr_filter'\033[0m - Clear filter history\n" << std::endl;
              
		std::cout << "\033[1;32m3. Special Display Commands:\033[0m\n"
				<< "   • Enter \033[1;34m'ls'\033[0m - List cached image file entries (Convert2ISO only)\n"
				<< "   • Enter \033[1;34m'stats'\033[0m - View on-disk ISO cache statistics (ImportISO only)\n" << std::endl;
              
		std::cout << "\033[1;32m4. Special Configuration Commands:\033[0m\n\n"
			<< "    \033[1;38;5;208m1. Auto-Update ISO Cache:\033[0m\n"
			<< "        • Enter \033[1;35m'*auto_on'\033[0m or \033[1;35m'*auto_off'\033[0m - Enable/Disable ISO cache auto-update via stored folder paths (ImportISO only)\n\n"
			<< "    \033[1;38;5;208m2. Set Default Display Modes (fl = full list, cl = compact list):\033[0m\n"
			<< "        • Mount list:       Enter \033[1;35m'*fl_m'\033[0m or \033[1;35m'*cl_m'\033[0m\n"
			<< "        • Umount list:      Enter \033[1;35m'*fl_u'\033[0m or \033[1;35m'*cl_u'\033[0m\n"
			<< "        • cp/mv/rm list:    Enter \033[1;35m'*fl_f'\033[0m or \033[1;35m'*cl_f'\033[0m\n"
			<< "        • Write list:       Enter \033[1;35m'*fl_w'\033[0m or \033[1;35m'*cl_w'\033[0m\n"
			<< "        • Conversion lists: Enter \033[1;35m'*fl_c'\033[0m or \033[1;35m'*cl_c'\033[0m\n"
			<< "        • Combine settings: Use multiple letters after \033[1;35m'*fl_'\033[0m or \033[1;35m'*cl_'\033[0m (e.g., \033[1;35m'*cl_mu'\033[0m for mount and umount lists)\n"
			<< std::endl;
	}
                
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for iso and device mapping
void helpMappings() {
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For Mappings =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m1. Selecting Mappings:\033[0m\n"
			  << "   • Mapping = NewISOIndex>RemovableUSBDevice\n"
              << "   • Single mapping: Enter a mapping (e.g., '1>/dev/sdc')\n"
              << "   • Multiple mappings: Separate with ; (e.g., '1>/dev/sdc;2>/dev/sdd' or '1>/dev/sdc;1>/dev/sdd')\n" << std::endl;
                  
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
