#include "../headers.h"
#include "../threadpool.h"


//	MOUNT STUFF

// Function to mount all ISOs indiscriminately
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    std::mutex mountAllmutex;
    unsigned int numThreads = std::min(static_cast<unsigned int>(isoFiles.size()), static_cast<unsigned int>(maxThreads));
    ThreadPool pool(numThreads);
    std::atomic<int> completedIsos(0);
    int totalIsos = static_cast<int>(isoFiles.size());
    std::atomic<bool> isComplete(false);

    // Create progress thread
    std::thread progressThread(displayProgressBar, std::ref(completedIsos), std::cref(totalIsos), std::ref(isComplete));

    // Vector to store futures
    std::vector<std::future<void>> futures;

    // Process all ISO files asynchronously
    for (size_t i = 0; i < isoFiles.size(); ++i) {
        futures.push_back(pool.enqueue([i, &isoFiles, &mountedFiles, &skippedMessages, &mountedFails, &mountAllmutex, &completedIsos]() {
            {
                std::lock_guard<std::mutex> lock(mountAllmutex);
                std::vector<std::string> isoFilesToMountLocal = { isoFiles[i] };
                mountIsoFile(isoFilesToMountLocal, mountedFiles, skippedMessages, mountedFails);
            }
            ++completedIsos;
        }));
    }

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }

    // Signal completion and wait for progress thread to finish
    isComplete = true;
    progressThread.join();
}


// Function to select and mount ISO files by number
void select_and_mount_files_by_number() {
	
	// Vector to store ISO mounts
	std::set<std::string> mountedFiles;
	// Vector to store skipped ISO mounts
	std::set<std::string> skippedMessages;
	// Vector to store failed ISO mounts
	std::set<std::string> mountedFails;
	// Vector to store ISO unique input errors
	std::set<std::string> uniqueErrorMessages;

    // Load ISO files from cache
    std::vector<std::string> isoFiles;
	isoFiles.reserve(100);

    // Main loop for selecting and mounting ISO files
    while (true) {
		
		// Remove non-existent paths from the cache after selection
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
		loadCache(isoFiles);
		
		// Check if the cache is empty
		if (isoFiles.empty()) {
			clearScrollBuffer();
			std::cout << "\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
			std::cout << " \n";
			std::cout << "\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			break;
		}
		
		bool verboseFiltered = false;
        clearScrollBuffer();
        std::cout << "\033[1;93m! IF EXPECTED ISO FILES ARE NOT ON THE LIST IMPORT THEM FROM THE MAIN MENU OPTIONS !\033[0;1m\n";
        
        std::string searchQuery;
        std::vector<std::string> filteredFiles;
        sortFilesCaseInsensitive(isoFiles);
        printIsoFileList(isoFiles);
		
        // Prompt user for input
        char* input = readline(
        "\n\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), / ↵ to filter, or ↵ to return:\001\033[0;1m\002 "
		);
        clearScrollBuffer();
        
        if (strcmp(input, "/") != 0 || (!(std::isspace(input[0]) || input[0] == '\0'))) {
			std::cout << "\033[1mPlease wait...\033[1m\n";
		}

        // Check if the user wants to return
        if (std::isspace(input[0]) || input[0] == '\0') {
			free(input);
            break;
        }

		if (strcmp(input, "/") == 0) {
			free(input);
			verboseFiltered = true;
			
			while (true) {
			
			clearScrollBuffer();
			historyPattern = true;
			loadHistory();
			
			// User pressed '/', start the filtering process
			std::string prompt = "\n\001\033[1;92m\002SearchQuery\001\033[1;94m\002 ↵ to filter \001\033[1;92m\002mount\001\033[1;94m\002 list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0;1m\002";
			
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
			// Check if the user wants to return
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
					while (true) {
						clearScrollBuffer();
						sortFilesCaseInsensitive(filteredFiles);
						std::cout << "\033[1mFiltered results:\033[0;1m\n";
						printIsoFileList(filteredFiles); // Print the filtered list of ISO files
					
						// Prompt user for input again with the filtered list
						char* input = readline("\n\n\001\033[1;92m\002Filtered ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), or ↵ to return:\001\033[0;1m\002 ");
					
						// Check if the user wants to return
						if (std::isspace(input[0]) || input[0] == '\0') {
							free(input);
							historyPattern = false;
							break;
						}
					
						if (std::strcmp(input, "00") == 0) {
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";
							// Restore the original list of ISO files
							isoFiles = filteredFiles;
							verboseFiltered = false;
							mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
						}

						// Check if the user provided input
						if (input[0] != '\0' && (strcmp(input, "/") != 0)) {
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";

							// Process the user input with the filtered list
							processAndMountIsoFiles(input, filteredFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
							free(input);
						
							clearScrollBuffer();

							printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
						}
					}
				}	
			} 
				} else {
					free(searchQuery);
					historyPattern = false;
					verboseFiltered = true;
					isoFiles = originalIsoFiles; // Revert to the original cache list
					break;
			}
		}
	}

        // Check if the user wants to mount all ISO files
		if (std::strcmp(input, "00") == 0) {
			mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
		}
        if (input[0] != '\0' && (strcmp(input, "/") != 0) && !verboseFiltered) {
            // Process user input to select and mount specific ISO files
            processAndMountIsoFiles(input, isoFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
            clearScrollBuffer();
            printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
            free(input);
        }          
    }
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


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    namespace fs = std::filesystem;
    
    for (const auto& isoFile : isoFilesToMount) {
        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();
        std::string mountPoint = "/mnt/iso_" + isoFileName;
        
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);
        
        // Check for root privileges
        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "'\033[0;1m\033[1;91m. Root privileges are required.\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            mountedFails.insert(errorMessage.str());
            continue;
        }
        
        // Check and create the mount point directory
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to create mount point: \033[1;93m'" << mountPoint 
                             << "'\033[0;1m\033[1;91m. Error: " << e.what() << "\033[0;1m";
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
                continue;
            }
        }
        
        // Initialize libmount context
        struct libmnt_context* cxt = mnt_new_context();
        if (!cxt) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to initialize mount context for: \033[1;93m'" 
                         << isoDirectory << "/" << isoFilename << "'\033[0;1m\033[1;91m.\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            mountedFails.insert(errorMessage.str());
            continue;
        }
        
        struct libmnt_fs* fs = mnt_new_fs();
        if (!fs) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to create new filesystem for: \033[1;93m'" 
                         << isoDirectory << "/" << isoFilename << "'\033[0;1m\033[1;91m.\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            mountedFails.insert(errorMessage.str());
            mnt_free_context(cxt);
            continue;
        }
        
        mnt_fs_set_source(fs, isoFile.c_str());
        mnt_fs_set_target(fs, mountPoint.c_str());
        mnt_fs_set_fstype(fs, "iso9660");
        mnt_fs_set_options(fs, "loop");
        mnt_context_set_fs(cxt, fs);
        
        int ret = mnt_context_mount(cxt);
        
        if (ret == -EBUSY) {
            // Mount point is already in use, which means it's already mounted
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename 
                           << "'\033[1;93m already mounted at: \033[1;94m'" << mountisoDirectory 
                           << "/" << mountisoFilename << "'\033[1;93m.\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            skippedMessages.insert(skippedMessage.str());
        } else if (ret != 0) {
            // Mount failure
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "'\033[0;1m\033[1;91m. Error code: " << -ret << "\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            fs::remove(mountPoint);
            mountedFails.insert(errorMessage.str());
        } else {
            // Successfully mounted
            std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0;1m"
                                          + "\033[1m mounted at: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename + "'\033[0;1m\033[1m.\033[0;1m";
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            mountedFiles.insert(mountedFileInfo);
        }
        
        mnt_free_fs(fs);
        mnt_free_context(cxt);
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages) {
    std::istringstream iss(input);
    std::istringstream issCount(input);
    
    std::set<std::string> tokens;
    std::string tokenCount;
    
    while (issCount >> tokenCount && tokens.size() < maxThreads) {
        if (tokenCount[0] == '-') continue;

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
                        tokens.insert(std::to_string(i));
                        if (tokens.size() >= maxThreads) {
                            break;
                        }
                    }
                }
            }
        } else if (std::all_of(tokenCount.begin(), tokenCount.end(), ::isdigit)) {
            int num = std::stoi(tokenCount);
            if (static_cast<std::vector<std::string>::size_type>(num) <= isoFiles.size()) {
                tokens.insert(tokenCount);
                if (tokens.size() >= maxThreads) {
                    break;
                }
            }
        }
    }
    
    unsigned int numThreads = std::min(static_cast<int>(tokens.size()), static_cast<int>(maxThreads));

    std::atomic<bool> invalidInput(false);
    std::mutex indicesMutex;
    std::set<int> processedIndices;
    std::set<int> validIndices;
    std::set<std::pair<int, int>> processedRanges;

    ThreadPool pool(numThreads);
    
    std::atomic<int> totalTasks(0);
    std::atomic<int> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    std::atomic<int> activeTaskCount(0);

    std::condition_variable taskCompletionCV;
    std::mutex taskCompletionMutex;

    std::mutex errorQueueMutex;
    std::queue<std::string> errorQueue;

    auto processTask = [&](int index) {
        bool shouldProcess = false;
        {
            std::lock_guard<std::mutex> lock(indicesMutex);
            if (validIndices.insert(index).second) {
                shouldProcess = true;
            }
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
        std::lock_guard<std::mutex> lock(errorQueueMutex);
        errorQueue.push(error);
        invalidInput.store(true, std::memory_order_relaxed);
    };

    std::string token;
    while (iss >> token) {
        if (token == "/") {
            break;
        }

        if (token != "00" && isAllZeros(token)) {
            addError("\033[1;91mInvalid index '0'.\033[0;1m");
            continue;
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            if (token.find('-', dashPos + 1) != std::string::npos || 
                (dashPos == 0 || dashPos == token.size() - 1 || !std::isdigit(token[dashPos - 1]) || !std::isdigit(token[dashPos + 1]))) {
                addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            }

            int start, end;
            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument&) {
                addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range&) {
                addError("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                addError("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[0;1m");
                continue;
            }

            std::pair<int, int> range(start, end);
            if (processedRanges.insert(range).second) {
                int step = (start <= end) ? 1 : -1;
                for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                    bool shouldProcess = false;
                    {
                        std::lock_guard<std::mutex> lock(indicesMutex);
                        if (processedIndices.insert(i).second) {
                            shouldProcess = true;
                        }
                    }
                    if (shouldProcess) {
                        totalTasks.fetch_add(1, std::memory_order_relaxed);
                        activeTaskCount.fetch_add(1, std::memory_order_relaxed);
                        pool.enqueue([&, i]() { processTask(i); });
                    }
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                bool shouldProcess = false;
                {
                    std::lock_guard<std::mutex> lock(indicesMutex);
                    if (processedIndices.insert(num).second) {
                        shouldProcess = true;
                    }
                }
                if (shouldProcess) {
                    totalTasks.fetch_add(1, std::memory_order_relaxed);
                    activeTaskCount.fetch_add(1, std::memory_order_relaxed);
                    pool.enqueue([&, num]() { processTask(num); });
                }
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                addError("\033[1;91mInvalid index '" + std::to_string(num) + "'.\033[0;1m");
            }
        } else {
            addError("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }

    // Process the error queue and add to uniqueErrorMessages
    {
        std::lock_guard<std::mutex> lock(errorQueueMutex);
        while (!errorQueue.empty()) {
            uniqueErrorMessages.insert(errorQueue.front());
            errorQueue.pop();
        }
    }

    // Set the total number of tasks value
    int totalTasksValue = totalTasks.load();

    // Start the progress bar in a separate thread
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), std::cref(totalTasksValue), std::ref(isProcessingComplete));

    // Wait for all tasks to complete
    {
        std::unique_lock<std::mutex> lock(taskCompletionMutex);
        taskCompletionCV.wait(lock, [&]() { return activeTaskCount.load() == 0; });
    }

    // Signal that processing is complete and wait for the progress thread to finish
    isProcessingComplete.store(true, std::memory_order_release);
    progressThread.join();
}
