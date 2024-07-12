#include "../headers.h"
#include "../threadpool.h"

//	MOUNT STUFF

// Function to mount all ISOs indiscriminately
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    std::atomic<int> completedIsos(0);
    std::atomic<bool> isComplete(false);
    unsigned int numThreads = std::min(static_cast<unsigned int>(isoFiles.size()), static_cast<unsigned int>(maxThreads));
    ThreadPool pool(numThreads);

    int totalIsos = static_cast<int>(isoFiles.size());

    // Calculate chunk size
    size_t chunkSize = (totalIsos + numThreads - 1) / numThreads;

    // Create progress thread
    std::thread progressThread(displayProgressBar, std::ref(completedIsos), totalIsos, std::ref(isComplete));
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (size_t i = 0; i < isoFiles.size(); i += chunkSize) {
        futures.push_back(pool.enqueue([&, i, chunkSize]() {
            std::vector<std::string> chunkFiles;
            chunkFiles.reserve(chunkSize);

            for (size_t j = i; j < std::min(i + chunkSize, isoFiles.size()); ++j) {
                chunkFiles.push_back(isoFiles[j]);
            }

            // Directly call function assuming downstream mutex ensures thread safety
            mountIsoFiles(chunkFiles, mountedFiles, skippedMessages, mountedFails);
            completedIsos.fetch_add(chunkFiles.size(), std::memory_order_relaxed);
        }));
    }

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }

    // Signal completion
    isComplete.store(true);

    // Wait for progress thread to finish
    if (progressThread.joinable()) {
        progressThread.join();
    }
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
		mountedFiles.clear();
		skippedMessages.clear();
		mountedFails.clear();
		uniqueErrorMessages.clear();
		// Remove non-existent paths from the cache after selection
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
		loadCache(isoFiles);
		
		// Check if the cache is empty
		if (isoFiles.empty()) {
			clearScrollBuffer();
			std::cout << "\n\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
			std::cout << " \n";
			std::cout << "\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			break;
		}
		
		bool isFiltered = false;
		bool search = true;
        clearScrollBuffer();
        std::cout << "\033[1;93m! IF EXPECTED ISO FILES ARE NOT ON THE LIST IMPORT THEM FROM THE MAIN MENU OPTIONS !\033[0;1m\n";
        
        std::string searchQuery;
        std::vector<std::string> filteredFiles;
        sortFilesCaseInsensitive(isoFiles);
        printIsoFileList(isoFiles);
		
        // Prompt user for input
       char* rawInput = readline("\n\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ");

		// Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);
        
        std::string mainInputString(input.get());		
        
        clearScrollBuffer();
        if (strcmp(input.get(), "/") != 0 || (!(std::isspace(input.get()[0]) || input.get()[0] == '\0'))) {
			std::cout << "\033[1mPlease wait...\033[1m\n";
		}
		
        if (std::isspace(input.get()[0]) || input.get()[0] == '\0') {
            break;
        } else if (strcmp(input.get(), "/") == 0) {
			isFiltered = true;
			
			while (search) {
			
			clearScrollBuffer();
			historyPattern = true;
			loadHistory();
			
			// User pressed '/', start the filtering process
			std::string prompt = "\n\001\033[1;92m\002Term(s)\001\033[1;94m\002 ↵ to filter \001\033[1;92m\002mount\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";

			// Prompt user for input
			char* rawSearchQuery = readline(prompt.c_str());

			// Use std::unique_ptr to manage memory for rawSearchQuery
			std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
			
			std::string inputSearch(searchQuery.get());

			clearScrollBuffer();
			
			
			if (searchQuery && searchQuery.get()[0] != '\0') {
				std::cout << "\033[1mPlease wait...\033[1m\n";
				if (strcmp(searchQuery.get(), "/") != 0) {
					add_history(searchQuery.get());
					saveHistory();
				}
			}
			clear_history();
			
			// Store the original isoFiles vector
			std::vector<std::string> originalIsoFiles = isoFiles;
			// Check if the user wants to return
			if (searchQuery && !(searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0)) {
        

			if (searchQuery != nullptr) {
				std::vector<std::string> filteredFiles = filterFiles(isoFiles, inputSearch);

				if (filteredFiles.empty()) {
					clearScrollBuffer();
					std::cout << "\n\033[1;91mNo matches found.\033[0;1m\n";
					std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
					std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
				} else {
					while (true) {
						mountedFiles.clear();
						skippedMessages.clear();
						mountedFails.clear();
						uniqueErrorMessages.clear();
						clearScrollBuffer();
						sortFilesCaseInsensitive(filteredFiles);
						std::cout << "\033[1mFiltered results:\033[0;1m\n";
						printIsoFileList(filteredFiles); // Print the filtered list of ISO files
					
						// Prompt user for input again with the filtered list
						std::string prompt = "\n\n\001\033[1;92m\002Filtered ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ";

						// Prompt user for input
						char* rawInputFiltered = readline(prompt.c_str());

						// Use std::unique_ptr to manage memory for rawInputFiltered
						std::unique_ptr<char, decltype(&std::free)> inputFiltered(rawInputFiltered, &std::free);
						
						std::string inputFilteredString(inputFiltered.get());
						
						
						if (inputFiltered.get()[0] == '/') {
							search = true;
							break;
						}
						
						if (std::isspace(inputFiltered.get()[0]) || inputFiltered.get()[0] == '\0') {
							search = false;
							historyPattern = false;
							break;
						} else if (std::strcmp(inputFiltered.get(), "00") == 0) {
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";
							// Restore the original list of ISO files
							isoFiles = filteredFiles;
							isFiltered = false;
							mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
							clearScrollBuffer();
							if (verbose) {
								printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
							}
						} else if (inputFiltered.get()[0] != '\0' && (strcmp(inputFiltered.get(), "/") != 0)) { // Check if the user provided input
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";

							// Process the user input with the filtered list
							processAndMountIsoFiles(inputFilteredString, filteredFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
						
							clearScrollBuffer();
							if (!uniqueErrorMessages.empty() && mountedFiles.empty() && skippedMessages.empty() && mountedFails.empty()) {
								verbose = false;
								std::cout << "\n\033[1;91mNo valid input provided for mount\033[0;1m\n";
								std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
								std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
							}

							if (verbose) {
								printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
							}
						}
					}
				}	
			} 
				} else {
					historyPattern = false;
					isFiltered = true;
					isoFiles = originalIsoFiles; // Revert to the original cache list
					break;
			}
		}
	}

        // Check if the user wants to mount all ISO files
		if (std::strcmp(input.get(), "00") == 0) {
			mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
			clearScrollBuffer();
			if (verbose) {
				printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
			}
		} else if (input.get()[0] != '\0' && (strcmp(input.get(), "/") != 0) && !isFiltered) {
            // Process user input to select and mount specific ISO files
            processAndMountIsoFiles(mainInputString, isoFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
            clearScrollBuffer();
            if (!uniqueErrorMessages.empty() && mountedFiles.empty() && skippedMessages.empty() && mountedFails.empty()) {
				    verbose = false;
				    std::cout << "\n\033[1;91mNo valid input provided for mount\033[0;1m\n";
				    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
					std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			}
				    
            if (verbose) {
				printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
			}
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
    verbose = false;
    
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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    for (const auto& isoFile : isoFiles) {
        namespace fs = std::filesystem;

        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();

        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);

        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        std::string uniqueId = isoFileName + "\033[38;5;245m~" + shortHash + "\033[1;94m";
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);
        
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
        
        // Create a libmount context
        struct libmnt_context *ctx = mnt_new_context();
        if (!ctx) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to create mount context for: \033[1;93m'" << isoFile << "'\033[0m";
            mountedFails.insert(errorMessage.str());
            continue;
        }

        // Set common mount options
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");

        // Set filesystem types to try
        std::string fsTypeList = "iso9660,udf,hfsplus,rockridge,joliet,isofs";
        mnt_context_set_fstype(ctx, fsTypeList.c_str());

        // Attempt to mount
        int ret = mnt_context_mount(ctx);

        bool mountSuccess = (ret == 0);
        std::string successfulFsType;

        if (mountSuccess) {
            // If successful, we can get the used filesystem type
            const char* usedFsType = mnt_context_get_fstype(ctx);
            if (usedFsType) {
                successfulFsType = usedFsType;
            }
        }

        // Clean up the context
        mnt_free_context(ctx);
        
        if (mountSuccess) {
            std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
                                        + "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
                                        + "'\033[0;1m.";
            if (successfulFsType != "auto") {
                mountedFileInfo += " {" + successfulFsType + "}";
            }
            mountedFileInfo += "\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFiles.insert(mountedFileInfo);
            }
        } else {
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
    
    
    std::vector<int> indicesToAdd;
    indicesToAdd.reserve(maxThreads);
    
    std::atomic<int> totalTasks(0);
    std::atomic<int> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    std::atomic<int> activeTaskCount(0);

    std::condition_variable taskCompletionCV;
    std::mutex taskCompletionMutex;

    std::mutex errorMutex;

    auto processTask = [&](const std::vector<std::string>& filesToMount) {
        mountIsoFiles(filesToMount, mountedFiles, skippedMessages, mountedFails);

        completedTasks.fetch_add(filesToMount.size(), std::memory_order_relaxed);
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

        if (startsWithZero(token)) {
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

    std::vector<int> indicesToProcess;
    {
        std::lock_guard<std::mutex> lock(indicesMutex);
        indicesToProcess = std::move(indicesToAdd);
    }
	
	// Check if we have any valid indices to process
    if (indicesToProcess.empty()) {
        addError("\033[1;91mNo valid input provided for mount.\033[0;1m");
        return;  // Exit the function early
    }
    
    unsigned int numThreads = std::min(static_cast<int>(indicesToProcess.size()), static_cast<int>(maxThreads));
    ThreadPool pool(numThreads);

    size_t chunkSize = std::max(1UL, (indicesToProcess.size() + maxThreads - 1) / maxThreads);
    totalTasks.store(indicesToProcess.size(), std::memory_order_relaxed);
    activeTaskCount.store((indicesToProcess.size() + chunkSize - 1) / chunkSize, std::memory_order_relaxed);

    for (size_t i = 0; i < indicesToProcess.size(); i += chunkSize) {
        size_t end = std::min(i + chunkSize, indicesToProcess.size());
        pool.enqueue([&, i, end]() {
            std::vector<std::string> filesToMount;
            filesToMount.reserve(end - i);
            for (size_t j = i; j < end; ++j) {
                filesToMount.push_back(isoFiles[indicesToProcess[j] - 1]);
            }
            processTask(filesToMount);
        });
    }

    if (!indicesToProcess.empty()) {
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
