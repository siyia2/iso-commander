#include "../headers.h"


// For storing isoFiles in RAM
std::vector<std::string> globalIsoFileList;

// Main function to select and operate on ISOs by number for umount mount cp mv and rm
void selectForIsoFiles(const std::string& operation, bool& historyPattern, int& maxDepth, bool& verbose) {
    // Calls prevent_clear_screen and tab completion
    rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
    rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
    
    std::set<std::string> operationFiles, skippedMessages, operationFails, uniqueErrorMessages;
    std::vector<std::string> filteredFiles, isoDirs;
    globalIsoFileList.reserve(100);
    bool isFiltered = false;
    bool needsClrScrn = true;
    bool umountMvRmBreak = false;
    
    // Determine operation color based on operation type
    std::string operationColor = (operation == "rm") ? "\033[1;91m" :
                                 (operation == "cp") ? "\033[1;92m" : 
                                 (operation == "mv") ? "\033[1;93m" :
                                 (operation == "mount") ? "\033[1;92m" : 
                                 (operation == "umount") ? "\033[1;93m" : "\033[1;95m";
    std::string process = operation;
    bool isMount = (operation == "mount");
    bool isUnmount = (operation == "umount");
    bool promptFlag = false; // PromptFlag for cache refresh, defaults to false for move and other operations
    
    while (true) {
        // Verbose output is to be disabled unless specified by progressbar function downstream
        verbose = false;

        operationFiles.clear();
        skippedMessages.clear();
        operationFails.clear();
        uniqueErrorMessages.clear();

        if (needsClrScrn && !isUnmount) {
			umountMvRmBreak = false;
            if (!clearAndLoadFiles(filteredFiles, isFiltered)) break;
            std::cout << "\n\n";
        } else if (needsClrScrn && isUnmount) {
			umountMvRmBreak = false;
            if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered)) break;
            std::cout << "\n\n";
		}
        
        // Move the cursor up 1 line and clear them
        std::cout << "\033[1A\033[K";

        std::string prompt = isFiltered 
            ? "\001\033[1;96m\002Filtered \001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + 
              "\001\033[1;94m\002 (e.g., 1-3,1 5," + (isMount || isUnmount ? "00=all" : "") + "), ~ ↵ (un)fold, / ↵ filter, ↵ return:\001\033[0;1m\002 "
            : "\001\033[1;92m\002ISO\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + 
              "\001\033[1;94m\002 (e.g., 1-3,1 5," + (isMount || isUnmount ? "00=all" : "") + "), ~ ↵ (un)fold, / ↵ filter, ↵ return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());

        if (inputString == "~") {
            toggleFullList = !toggleFullList;
            needsClrScrn = true;
            continue;
        }

        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                continue;
            } else {
                return;
            }
        } else if (inputString == "/") {
            while (true) {
                verbose = false;
                operationFiles.clear();
                skippedMessages.clear();
                operationFails.clear();
                uniqueErrorMessages.clear();

                clear_history();
                historyPattern = true;
                loadHistory(historyPattern);
                // Move the cursor up 1 line and clear them
                std::cout << "\033[1A\033[K";

                // Generate prompt
				std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + operationColor + "\002" + operation + 
                                           " \001\033[1;94m\002list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";
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
                
                // Decide the current list to filter
                std::vector<std::string>& currentFiles = !isUnmount 
				? (isFiltered ? filteredFiles : globalIsoFileList)
				: (isFiltered ? filteredFiles : isoDirs);

                // Apply the filter on the current list
                auto newFilteredFiles = filterFiles(currentFiles, inputSearch);
                sortFilesCaseInsensitive(newFilteredFiles);

                if ((newFilteredFiles.size() == globalIsoFileList.size() && isMount) || (newFilteredFiles.size() == isoDirs.size() && isUnmount)) {
                    isFiltered = false;
                    break;
                }

                if (!newFilteredFiles.empty()) {
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
        } else {
			std::vector<std::string>& currentFiles = isFiltered 
			? filteredFiles 
			: (!isUnmount ? globalIsoFileList : isoDirs);

            clearScrollBuffer();
            std::cout << "\033[1m\n";
            needsClrScrn = true;

            if (isMount && inputString == "00") {
                // Special case for mounting all files
                std::cout << "\033[0;1m";
                currentFiles = globalIsoFileList;
                processAndMountIsoFiles(inputString, currentFiles, operationFiles, skippedMessages, operationFails, uniqueErrorMessages, verbose);
            } else if (isMount){
				clearScrollBuffer();
                needsClrScrn = true;
                std::cout << "\033[0;1m\n";
					processAndMountIsoFiles(inputString, currentFiles, operationFiles, skippedMessages, operationFails, uniqueErrorMessages, verbose);
			} else if (isUnmount) {
            // Unmount-specific logic
            std::vector<std::string> selectedIsoDirs;
            
            if (inputString == "00") {
                selectedIsoDirs = currentFiles;
                umountMvRmBreak = true;
            } else {
                umountMvRmBreak = true;
            }
            
			prepareUnmount(inputString, selectedIsoDirs, currentFiles, operationFiles, operationFails, uniqueErrorMessages, umountMvRmBreak, verbose);
            needsClrScrn = true;
                 
        } else {
            // Generic operation processing for copy, move, remove
            std::cout << "\033[0;1m\n";
            processOperationInput(inputString, currentFiles, operation, operationFiles, operationFails, uniqueErrorMessages, promptFlag, maxDepth, umountMvRmBreak, historyPattern, verbose);
        }

            // Check and print results
            if (!uniqueErrorMessages.empty() && operationFiles.empty() && skippedMessages.empty() && operationFails.empty() && isMount) {
                clearScrollBuffer();
                needsClrScrn = true;
                std::cout << "\n\033[1;91mNo valid input provided for " << operation << "\033[0;1m\n\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            } else if (verbose) {
                clearScrollBuffer();
                needsClrScrn = true;
                if (isMount){
					verbosePrint(operationFiles, operationFails, skippedMessages, {}, uniqueErrorMessages, 2);
				} else if (isUnmount){
					verbosePrint(operationFiles, operationFails, {}, {}, uniqueErrorMessages, 1);
				} else {
					verbosePrint(operationFiles, operationFails, {}, {}, uniqueErrorMessages, 1);
				}
            }

            // Additional logic for non-mount operations
            if ((process == "mv" || process == "rm" || process == "umount") && isFiltered && umountMvRmBreak) {
                historyPattern = false;
                clear_history();
                isFiltered = false;
                needsClrScrn = true;
            }

            if (currentFiles.empty()) {
                clearScrollBuffer();
                needsClrScrn = true;
                std::cout << "\n\033[1;93mNo ISO available for " << operation << ".\033[0m\n\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return;
            }
        }
    }
}


// General function to tokenize input strings
void tokenizeInput(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::set<int>& processedIndices) {
    std::istringstream iss(input);
    std::string token;

    while (iss >> token) {

        // Check if the token starts with zero and treat it as a non-existent index
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

            // Early range validity check
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[0;1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && processedIndices.find(i) == processedIndices.end()) {
                    processedIndices.insert(i); // Mark as processed
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    uniqueErrorMessages.emplace("\033[1;91mInvalid index '" + std::to_string(i) + "'.\033[0;1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);

            // Early range validity check for single index
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                if (processedIndices.find(num) == processedIndices.end()) {
                    processedIndices.insert(num); // Mark index as processed
                }
            } else {
                uniqueErrorMessages.emplace("\033[1;91mInvalid index: '" + std::to_string(num) + "'.\033[0;1m");
            }
        } else {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }
}


// Function to display progress bar for native operations
void displayProgressBar(const std::atomic<size_t>& completedIsos, const size_t& totalIsos, std::atomic<bool>& isComplete, bool& verbose) {
    const int barWidth = 50;
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Set up non-blocking input
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Set stdin to non-blocking mode
    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    try {
        while (!isComplete.load() || !enterPressed) {
            // Flush any pending input
            char ch;
            while (read(STDIN_FILENO, &ch, 1) > 0) {
                // Discard any input during progress
            }

            size_t completedValue = completedIsos.load();
            double progress = static_cast<double>(completedValue) / totalIsos;
            int pos = static_cast<int>(barWidth * progress);
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
            double elapsedSeconds = elapsedTime.count() / 1000.0;

            std::cout << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) std::cout << "=";
                else if (i == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::setw(3) << std::fixed << std::setprecision(1)
                      << (progress * 100.0) << "% (" << completedValue << "/" << totalIsos << ") "
                      << "Time Elapsed: " << std::setprecision(1) << elapsedSeconds << "s";

            if (completedValue == totalIsos && !enterPressed) {
                enterPressed = true;
                
                // Restore terminal to original settings before getting input
                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                fcntl(STDIN_FILENO, F_SETFL, oldf);

                std::string confirmation;
                std::cout << "\n\n\033[1;94mDisplay verbose output? (y/n):\033[0;1m ";
                std::getline(std::cin, confirmation);
                
                if (confirmation == "y" || confirmation == "Y") {
                    verbose = true;
                } else {
                    verbose = false;
                }
            } else {
                std::cout.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Update every 100ms
            }
        }
    }
    catch (...) {
		// Flush any pending input in case of any exceptions
		char ch;
		while (read(STDIN_FILENO, &ch, 1) > 0) {
			// Discard any input during progress
		}
        // Ensure terminal is restored in case of any exceptions
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fcntl(STDIN_FILENO, F_SETFL, oldf);
        throw;
    }

    // Print a newline after completion
    std::cout << std::endl;

    // Ensure terminal is restored to original settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    
}


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType) {
    static const char* defaultColor = "\033[0m";
    static const char* bold = "\033[1m";
    static const char* reset = "\033[0m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* blueBold = "\033[94;1m";
    static const char* magenta = "\033[95m";
    static const char* magentaBold = "\033[95;1m";
    static const char* orangeBold = "\033[1;38;5;208m";
    
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
        std::string directory, filename, displayPath;

        if (listType == "ISO_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i]);
            directory = dir;
            filename = fname;
        } else if (listType == "MOUNTED_ISOS") {
            std::string dirName = items[i];
            size_t lastSlashPos = dirName.find_last_of('/');
            if (lastSlashPos != std::string::npos) {
                dirName = dirName.substr(lastSlashPos + 1);
            }
            size_t underscorePos = dirName.find('_');
            displayPath = ((underscorePos != std::string::npos) ? dirName.substr(underscorePos + 1) : dirName);
        } else if (listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i]);

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
            output << sequenceColor << indexStrings[i] << ". "
                   << blueBold << "/mnt/iso_"
                   << magentaBold << displayPath << reset << "\n";
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


//For toggling long/short paths in lists and verbose
bool toggleFullList = false;

// For memory mapping string transformations
std::unordered_map<std::string, std::string> transformationCache;

// Function to extract directory and filename from a given path
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path) {
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
    if (toggleFullList) {
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
