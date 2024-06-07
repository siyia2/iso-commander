#include "../headers.h"
#include "../threadpool.h"

// UMOUNT STUFF


// Vector to store ISO mount/unmount errors
std::vector<std::string> errorMessages;
// Vector to store ISO unmounts
std::set<std::string> unmountedFiles;
// Vector to store ISO unmount errors
std::set<std::string> unmountedErrors;


// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";
	
	static std::mutex listMutex;
	
    // Vector to store names of mounted ISOs
    std::vector<std::string> isoDirs;
    
    // Lock mutex for accessing shared resource
    std::lock_guard<std::mutex> lock(listMutex);

    // Open the /mnt directory and find directories with names starting with "iso_"
    DIR* dir;
    struct dirent* entry;

    if ((dir = opendir(isoPath.c_str())) != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            // Check if the entry is a directory and has a name starting with "iso_"
            if (entry->d_type == DT_DIR && std::string(entry->d_name).find("iso_") == 0) {
                // Build the full path and extract the ISO name
                std::string fullDirPath = isoPath + "/" + entry->d_name;
                std::string isoName = entry->d_name + 4; // Remove "/mnt/iso_" part
                isoDirs.push_back(isoName);
            }
        }
        closedir(dir);
    } else {
        // Print an error message if there is an issue opening the /mnt directory
        std::cerr << "\033[1;91mError opening the /mnt directory.\033[0;1m\n";
        return;
    }

    // Display a list of mounted ISOs with ISO names in bold and alternating colors
    if (!isoDirs.empty()) {
		sortFilesCaseInsensitive(isoDirs);
        std::cout << "\033[0;1mList of mounted ISO(s):\033[0;1m\n"; // White and bold
        std::cout << " \n";
        
        size_t maxIndex = isoDirs.size();
		size_t numDigits = std::to_string(maxIndex).length();

        bool useRedColor = true; // Start with red color for sequence numbers

        for (size_t i = 0; i < isoDirs.size(); ++i) {
            // Determine color based on alternating pattern
            std::string sequenceColor = (useRedColor) ? "\033[31;1m" : "\033[32;1m";
            useRedColor = !useRedColor; // Toggle between red and green

            // Print sequence number with the determined color
            std::cout << sequenceColor << std::setw(numDigits) << i + 1 << ". ";

            // Print ISO directory path in bold and magenta
            std::cout << "\033[0;1m/mnt/iso_\033[1m\033[95m" << isoDirs[i] << "\033[0;1m\n";
        }
    }
}


//function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    namespace fs = std::filesystem;

    // Create a path object from the given string
    fs::path dirPath(path);

    // Check if the path exists and is a directory
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        // Handle the case when the path is invalid or not a directory
        return false;
    }

    // Iterate over the directory entries at the surface level
    auto dirIter = fs::directory_iterator(dirPath);
    auto end = fs::directory_iterator(); // Default constructor gives the "end" iterator

    // Check if there are any entries in the directory
    if (dirIter != end) {
        // If we find any entry (file or subdirectory) in the directory, it's not empty
        return false;
    }

    // If we reach here, it means the directory is empty
    return true;
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs) {
    // Determine batch size based on the number of isoDirs
    size_t batchSize = 1;
    if (isoDirs.size() > 100000 && isoDirs.size() > maxThreads) {
        batchSize = 100;
    } else if (isoDirs.size() > 10000 && isoDirs.size() > maxThreads) {
        batchSize = 50;
    } else if (isoDirs.size() > 1000 && isoDirs.size() > maxThreads) {
        batchSize = 25;
    } else if (isoDirs.size() > 100 && isoDirs.size() > maxThreads) {
        batchSize = 10;
    } else if (isoDirs.size() > 50 && isoDirs.size() > maxThreads) {
        batchSize = 5;
    } else if (isoDirs.size() > maxThreads) {
        batchSize = 2;
    }

    // Use std::async to unmount and remove the directories asynchronously
    auto unmountFuture = std::async(std::launch::async, [&isoDirs, batchSize]() {
        // Unmount directories in batches
        for (size_t i = 0; i < isoDirs.size(); i += batchSize) {
            std::string unmountBatchCommand = "umount -l";
            size_t batchEnd = std::min(i + batchSize, isoDirs.size());

            for (size_t j = i; j < batchEnd; ++j) {
                unmountBatchCommand += " " + shell_escape(isoDirs[j]) + " 2>/dev/null";
            }

            std::future<int> unmountFuture = std::async(std::launch::async, [&unmountBatchCommand]() {
                std::array<char, 128> buffer;
                std::string result;
                auto pclose_deleter = [](FILE* fp) { return pclose(fp); };
                std::unique_ptr<FILE, decltype(pclose_deleter)> pipe(popen(unmountBatchCommand.c_str(), "r"), pclose_deleter);
                if (!pipe) {
                    throw std::runtime_error("popen() failed!");
                }
                while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                    result += buffer.data();
                }
                return 0; // Return value doesn't matter for umount
            });

            unmountFuture.get();
        }

        // Check and remove empty directories
        std::vector<std::string> emptyDirs;
        for (const auto& isoDir : isoDirs) {
            if (isDirectoryEmpty(isoDir)) {
                emptyDirs.push_back(isoDir);
            } else {
                // Handle non-empty directory error
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDir << "'\033[1;91m.\033[0;1m";

                if (std::find(unmountedErrors.begin(), unmountedErrors.end(), errorMessage.str()) == unmountedErrors.end()) {
                    unmountedErrors.insert(errorMessage.str());
                }
            }
        }

        // Remove empty directories in batches
        while (!emptyDirs.empty()) {
            std::string removeDirCommand = "rmdir ";
            for (size_t i = 0; i < std::min(batchSize, emptyDirs.size()); ++i) {
                removeDirCommand += shell_escape(emptyDirs[i]) + " ";
            }

            std::future<int> removeDirFuture = std::async(std::launch::async, [&removeDirCommand]() {
                std::array<char, 128> buffer;
                std::string result;
                auto pclose_deleter = [](FILE* fp) { return pclose(fp); };
                std::unique_ptr<FILE, decltype(pclose_deleter)> pipe(popen(removeDirCommand.c_str(), "r"), pclose_deleter);
                if (!pipe) {
                    throw std::runtime_error("popen() failed!");
                }
                while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                    result += buffer.data();
                }
                return pipe.get() ? 0 : -1;
            });

            int removeDirResult = removeDirFuture.get();

            for (size_t i = 0; i < std::min(batchSize, emptyDirs.size()); ++i) {
                if (removeDirResult == 0) {
                    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(emptyDirs[i]);
                    std::string unmountedFileInfo = "\033[1mUnmounted: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0;1m.";
                    unmountedFiles.insert(unmountedFileInfo);
                } else {
                    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(emptyDirs[i]);
                    std::stringstream errorMessage;
                    errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;91m ...Please check it out manually.\033[0;1m";

                    if (std::find(unmountedErrors.begin(), unmountedErrors.end(), errorMessage.str()) == unmountedErrors.end()) {
                        unmountedErrors.insert(errorMessage.str());
                    }
                }
            }
            emptyDirs.erase(emptyDirs.begin(), emptyDirs.begin() + std::min(batchSize, emptyDirs.size()));
            
        }
    });

    unmountFuture.get();
}


// Function to print unmounted ISOs and errors
void printUnmountedAndErrors(bool invalidInput) {
	clearScrollBuffer();
	
    // Print unmounted files
    for (const auto& unmountedFile : unmountedFiles) {
        std::cout << "\n" << unmountedFile;
    }

    // Clear vectors
    unmountedFiles.clear();
    unmountedErrors.clear();
    
    if (invalidInput) {
				std::cout << "\n";
			}

    // Print unique error messages
    for (const auto& errorMessage : errorMessages) {
        if (uniqueErrorMessages.find(errorMessage) == uniqueErrorMessages.end()) {
            // If not found, store the error message and print it
            uniqueErrorMessages.insert(errorMessage);
            std::cerr << "\n\033[1;91m" << errorMessage << "\033[0m\033[1m";
        }
    }
    uniqueErrorMessages.clear();
}


// Main function for unmounting ISOs
void unmountISOs() {
    // Initialize necessary variables
    std::vector<std::string> isoDirs;
    std::mutex isoDirsMutex;
    const std::string isoPath = "/mnt";
    bool invalidInput = false, skipEnter = false, isFiltered = false, noValid = true;

    while (true) {
        // Initialize variables for each loop iteration
        std::vector<std::string> filteredIsoDirs, selectedIsoDirs, selectedIsoDirsFiltered;
        clearScrollBuffer();
        listMountedISOs();
        isoDirs.clear();
        errorMessages.clear();
        invalidInput = false;
        uniqueErrorMessages.clear();

        {
            // Lock the mutex to protect isoDirs from concurrent access
            std::lock_guard<std::mutex> lock(isoDirsMutex);

            // Populate isoDirs with directories that match the "iso_" prefix
            for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
                if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                    isoDirs.push_back(entry.path().string());
                }
            }

            sortFilesCaseInsensitive(isoDirs);
        }


        // Check if there are no matching directories
        if (isoDirs.empty()) {
            std::cerr << "\033[1;93mNo path(s) matching the '/mnt/iso_*' pattern found.\033[0m\033[1m\n";
            std::cout << "\n\033[1;32m↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
		
        // Prompt the user for input
        char* input = readline("\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), / ↵ to filter\001\033[1;94m\002 , or ↵ to return:\001\033[0m\002\001\033[1m\002 ");
        clearScrollBuffer();
        

        if (input[0] != '/' || (!(std::isspace(input[0]) || input[0] == '\0'))) {
            std::cout << "Please wait...\n";
        }

        // Check if the user wants to return to the main menu
        if (std::isspace(input[0]) || input[0] == '\0') {
			free(input);
            break;
        }

        // Check if the user wants to filter the list of ISOs
        if (strcmp(input, "/") == 0) {
			bool breakOuterLoop = false;
            while (true) {
				if (breakOuterLoop) {
					historyPattern = false;
					break;
				}
                clearScrollBuffer();
                isFiltered = true;
                historyPattern = true;
                loadHistory();
                std::string prompt;
                prompt = "\n\001\033[1;92m\002SearchQuery\001\033[1;94m\002 ↵ to filter \001\033[1;93m\002umount\001\033[1;94m\002 list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0m\033[1m\002";
                
                char* filterPattern = readline(prompt.c_str());
                clearScrollBuffer();
                
                if (filterPattern && filterPattern[0] != '\0') {
					std::cout << "\033[1mPlease wait...\033[1m\n";
					add_history(filterPattern); // Add the search query to the history
					saveHistory();
				}

				clear_history();

				if (std::isspace(filterPattern[0]) || filterPattern[0] == '\0') {
					free(filterPattern);
					skipEnter = false;
					isFiltered = false;
					noValid = false;
					historyPattern = false;
					break;
				}

				// Split the filterPattern string into tokens using the delimiter ';'
				std::vector<std::string> filterPatterns;
				std::stringstream ss(filterPattern);
				std::string token;
				while (std::getline(ss, token, ';')) {
					filterPatterns.push_back(token);
					std::transform(filterPatterns.back().begin(), filterPatterns.back().end(), filterPatterns.back().begin(), ::tolower);
				}
				free(filterPattern);

				// Filter the list of ISO directories based on the filter pattern
				filteredIsoDirs.clear();

				// Vector to store futures for tracking tasks' completion
				std::vector<std::future<void>> futures;

				// Calculate the number of directories per thread
				size_t numDirs = isoDirs.size();
				unsigned int numThreads = std::min(static_cast<unsigned int>(numDirs), maxThreads);
				size_t baseDirsPerThread = numDirs / numThreads;
				size_t remainder = numDirs % numThreads;

				// Launch filter tasks asynchronously
				for (size_t i = 0; i < numThreads; ++i) {
					size_t start = i * baseDirsPerThread + std::min(i, remainder);
					size_t end = start + baseDirsPerThread + (i < remainder ? 1 : 0);

					futures.push_back(std::async(std::launch::async, [&](size_t start, size_t end) {
						filterMountPoints(isoDirs, filterPatterns, filteredIsoDirs, isoDirsMutex, start, end);
					}, start, end));
				}

				// Wait for all filter tasks to complete
				for (auto& future : futures) {
					future.get();
				}

                // Check if any directories matched the filter
                if (filteredIsoDirs.empty()) {
                    clearScrollBuffer();
                    std::cout << "\033[1;91mNo ISO mountpoint(s) match the filter pattern.\033[0m\033[1m\n";
                    std::cout << "\n\033[1;32m↵ to continue...";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    clearScrollBuffer();
                } else {
                    // Display the filtered list and prompt the user for input
                    while (true) {
                        clearScrollBuffer();
                        sortFilesCaseInsensitive(filteredIsoDirs);
                        std::cout << "\033[1mFiltered results:\n\033[0m\033[1m" << std::endl;
                        size_t maxIndex = filteredIsoDirs.size();
						size_t numDigits = std::to_string(maxIndex).length();

						for (size_t i = 0; i < filteredIsoDirs.size(); ++i) {
							std::string afterSlash = filteredIsoDirs[i].substr(filteredIsoDirs[i].find_last_of("/") + 1);
							std::string afterUnderscore = afterSlash.substr(afterSlash.find("_") + 1);
							std::string color = (i % 2 == 0) ? "\033[1;31m" : "\033[1;32m"; // Red if even, Green if odd

							std::cout << color << "\033[1m"
                            << std::setw(numDigits) << std::setfill(' ') << (i + 1) << ".\033[0;1m /mnt/iso_"
                            << "\033[1;95m" << afterUnderscore << "\033[0;1m\n";
						}

                        // Prompt the user for the list of ISOs to unmount
                        char* chosenNumbers = readline("\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), or ↵ to return:\001\033[0m\002\001\033[1m\002 ");

                        if (std::isspace(chosenNumbers[0]) || chosenNumbers[0] == '\0') {
							free(chosenNumbers);
                            noValid = false;
                            skipEnter = true;
                            historyPattern = false;
                            break;
                        }

                        if (std::strcmp(chosenNumbers, "00") == 0) {
							free(chosenNumbers);
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
                            selectedIsoDirs = filteredIsoDirs;
                            isFiltered = true;
                            breakOuterLoop = true;
                            historyPattern = false;
                            break;
                        }

                        // Parse the user input to determine which ISOs to unmount
                        std::set<size_t> selectedIndices;
                        std::istringstream iss(chosenNumbers);
                        free(chosenNumbers);
                        for (std::string token; iss >> token;) {
                            try {
                                size_t dashPos = token.find('-');
                                if (dashPos != std::string::npos) {
                                    size_t start = std::stoi(token.substr(0, dashPos)) - 1;
                                    size_t end = std::stoi(token.substr(dashPos + 1)) - 1;
                                    if (start < filteredIsoDirs.size() && end < filteredIsoDirs.size()) {
                                        for (size_t i = std::min(start, end); i <= std::max(start, end); ++i) {
                                            selectedIndices.insert(i);
                                        }
                                    } else {
                                        errorMessages.push_back("Invalid range: '" + token + "'.");
                                        invalidInput = true;
                                    }
                                } else {
                                    size_t index = std::stoi(token) - 1;
                                    if (index < filteredIsoDirs.size()) {
                                        selectedIndices.insert(index);
                                    } else {
                                        errorMessages.push_back("Invalid index: '" + token + "'.");
                                        invalidInput = true;
                                    }
                                }
                            } catch (const std::invalid_argument&) {
									errorMessages.push_back("Invalid input: '" + token + "'.");
									invalidInput = true;
                            }
                        }               

                        selectedIsoDirsFiltered.clear();
                        for (size_t index : selectedIndices) {
                            selectedIsoDirsFiltered.push_back(filteredIsoDirs[index]);
                        }

                        if (!selectedIsoDirsFiltered.empty()) {
                            selectedIsoDirs = selectedIsoDirsFiltered;
                            skipEnter = false;
                            isFiltered = true;
                            historyPattern = false;
                            break; // Exit filter loop to process unmount
                        } else {
                            clearScrollBuffer();
                            std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                            std::cout << "\n\033[1;32m↵ to continue...";
                            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                        }
                    }
                }

                if (!selectedIsoDirsFiltered.empty() && isFiltered) {
					clearScrollBuffer();
					std::cout << "\033[1mPlease wait...\033[1m\n";
					isFiltered = true;
					historyPattern = false;
                    break;
                }
            }
        }

        // Check if the user wants to unmount all ISOs
        if (std::strcmp(input, "00") == 0) {
			free(input);
            selectedIsoDirs = isoDirs;
        } else if (!isFiltered) {
            // Parse the user input to determine which ISOs to unmount
            std::set<size_t> selectedIndices;
            std::istringstream iss(input);
            free(input);
            for (std::string token; iss >> token;) {
                try {
                    size_t dashPos = token.find('-');
                    if (dashPos != std::string::npos) {
                        size_t start = std::stoi(token.substr(0, dashPos)) - 1;
                        size_t end = std::stoi(token.substr(dashPos + 1)) - 1;
                        if (start < isoDirs.size() && end < isoDirs.size()) {
                            for (size_t i = std::min(start, end); i <= std::max(start, end); ++i) {
                                selectedIndices.insert(i);
                            }
                        } else {
                            errorMessages.push_back("Invalid range: '" + token + "'.");
                            invalidInput = true;
                        }
                    } else {
                        size_t index = std::stoi(token) - 1;
                        if (index < isoDirs.size()) {
                            selectedIndices.insert(index);
                        } else {
                            errorMessages.push_back("Invalid index: '" + token + "'.");
                            invalidInput = true;
                        }
                    }
                } catch (const std::invalid_argument&) {
                    errorMessages.push_back("Invalid input: '" + token + "'.");
                    invalidInput = true;
                }
            }

            if (!selectedIndices.empty()) {
                for (size_t index : selectedIndices) {
                    selectedIsoDirs.push_back(isoDirs[index]);
                }
            } else {
                clearScrollBuffer();
                if (noValid) {
                    std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                    std::cout << "\n\033[1;32m↵ to continue...";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                }
                noValid = true;
            }
        }

        // If there are selected ISOs, proceed to unmount them
        if (!selectedIsoDirs.empty()) {
            std::vector<std::future<void>> futures;
            unsigned int numThreads = std::min(static_cast<int>(selectedIsoDirs.size()), static_cast<int>(maxThreads));
            ThreadPool pool(numThreads);
            std::lock_guard<std::mutex> isoDirsLock(isoDirsMutex);

            // Divide the selected ISOs into batches for parallel processing
            size_t batchSize = (selectedIsoDirs.size() + maxThreads - 1) / maxThreads;
            std::vector<std::vector<std::string>> batches;
            for (size_t i = 0; i < selectedIsoDirs.size(); i += batchSize) {
                batches.emplace_back(selectedIsoDirs.begin() + i, std::min(selectedIsoDirs.begin() + i + batchSize, selectedIsoDirs.end()));
            }

            // Enqueue unmount tasks for each batch of ISOs
            for (const auto& batch : batches) {
                futures.emplace_back(pool.enqueue([batch]() {
                    std::lock_guard<std::mutex> highLock(Mutex4High);
                    unmountISO(batch);
                }));
            }

            // Wait for all unmount tasks to complete
            for (auto& future : futures) {
                future.wait();
            }
            
            printUnmountedAndErrors(invalidInput);

            // Prompt the user to press Enter to continue
            if (!skipEnter) {
                std::cout << "\n\n\033[1;32m↵ to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
            clearScrollBuffer();
            skipEnter = false;
            isFiltered = false;
        }
    }
}
