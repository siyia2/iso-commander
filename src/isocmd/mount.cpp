#include "../headers.h"
#include "../threadpool.h"

//	MOUNT STUFF

// Global variable to store failed ISO files
std::set<std::string> globalFailedISOs;

//Global mutex for failed ISO files
std::mutex globalFailedISOMutex;


// Function to mount all ISOs indiscriminately
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
    std::atomic<int> completedIsos(0);
    std::atomic<bool> isComplete(false);
    unsigned int numThreads = std::min(static_cast<unsigned int>(isoFiles.size()), static_cast<unsigned int>(maxThreads));
    ThreadPool pool(numThreads);
    
    std::mutex Mutex4CompletedIsos; // Mutex for completedIsos
    
    int totalIsos = static_cast<int>(isoFiles.size());
    
    // Create progress thread
    std::thread progressThread(displayProgressBar, std::ref(completedIsos), std::cref(totalIsos), std::ref(isComplete));
    
    // Process all ISO files asynchronously
    std::vector<std::future<void>> futures;
    futures.reserve(100);
        for (const auto& isoFile : isoFiles) {
            futures.push_back(pool.enqueue([&isoFile, &mountedFiles, &skippedMessages, &mountedFails, &completedIsos, &Mutex4CompletedIsos]() {
                // Protect increment of completedIsos with a mutex
                {
                    std::lock_guard<std::mutex> guard(Mutex4CompletedIsos);
                    mountIsoFile({isoFile}, mountedFiles, skippedMessages, mountedFails);
                    ++completedIsos;
                }
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
			std::cout << "\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
			std::cout << " \n";
			std::cout << "\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			break;
		}
		
		bool isFiltered = false;
		bool noProcessing = false;
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
        if (strcmp(input, "clr") == 0) {
			clearScrollBuffer();
			free(input);
			noProcessing = true;
			globalFailedISOs.clear();
			std::cout << "\n\033[1;93mMount failure status for ISO(s) got reset.\033[0;1m\n";
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		}

        
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
			isFiltered = true;
			
			while (true) {
			
			clearScrollBuffer();
			historyPattern = true;
			loadHistory();
			
			// User pressed '/', start the filtering process
			std::string prompt = "\n\001\033[1;92m\002Term(s)\001\033[1;94m\002 ↵ to filter \001\033[1;92m\002mount\001\033[1;94m\002 list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0;1m\002";
			
			char* searchQuery = readline(prompt.c_str());
			clearScrollBuffer();
			
			
			if (searchQuery && searchQuery[0] != '\0') {
				std::cout << "\033[1mPlease wait...\033[1m\n";
				if (strcmp(searchQuery, "/") != 0) {
					add_history(searchQuery); // Add the search query to the history
					saveHistory();
				}
			}
			clear_history();
			
			// Store the original isoFiles vector
			std::vector<std::string> originalIsoFiles = isoFiles;
			// Check if the user wants to return
			if (!(searchQuery[0] == '\0'|| strcmp(searchQuery, "/") == 0)) {
        

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
						mountedFiles.clear();
						skippedMessages.clear();
						mountedFails.clear();
						uniqueErrorMessages.clear();
						clearScrollBuffer();
						sortFilesCaseInsensitive(filteredFiles);
						std::cout << "\033[1mFiltered results:\033[0;1m\n";
						printIsoFileList(filteredFiles); // Print the filtered list of ISO files
					
						// Prompt user for input again with the filtered list
						char* inputFiltered = readline("\n\n\001\033[1;92m\002Filtered ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), or ↵ to return:\001\033[0;1m\002 ");
						
						if (strcmp(inputFiltered, "clr") == 0) {
							clearScrollBuffer();
							free(inputFiltered);
							noProcessing = true;
							globalFailedISOs.clear();
							std::cout << "\n\033[1;93mMount failure status for ISO(s) got reset.\033[0;1m\n";
							std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
							std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
					
						}
					
						// Check if the user wants to return
						if (std::isspace(inputFiltered[0]) || inputFiltered[0] == '\0') {
							free(inputFiltered);
							historyPattern = false;
							break;
						}
					
						if (std::strcmp(inputFiltered, "00") == 0) {
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";
							// Restore the original list of ISO files
							isoFiles = filteredFiles;
							isFiltered = false;
							mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
							free(inputFiltered);
							clearScrollBuffer();
							if (verbose) {
								printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
							}
						} else if (inputFiltered[0] != '\0' && (strcmp(inputFiltered, "/") != 0) && !noProcessing) { // Check if the user provided input
							clearScrollBuffer();
							std::cout << "\033[1mPlease wait...\033[1m\n";

							// Process the user input with the filtered list
							processAndMountIsoFiles(inputFiltered, filteredFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
							free(inputFiltered);
						
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
					free(searchQuery);
					historyPattern = false;
					isFiltered = true;
					isoFiles = originalIsoFiles; // Revert to the original cache list
					break;
			}
		}
	}

        // Check if the user wants to mount all ISO files
		if (std::strcmp(input, "00") == 0) {
			mountAllIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails);
			free(input);
			clearScrollBuffer();
			if (verbose) {
				printMountedAndErrors(mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
			}
		} else if (input[0] != '\0' && (strcmp(input, "/") != 0) && !isFiltered && !noProcessing) {
            // Process user input to select and mount specific ISO files
            processAndMountIsoFiles(input, isoFiles, mountedFiles, skippedMessages, mountedFails, uniqueErrorMessages);
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


// Function to check if a mountpoint isAlreadyMounted
bool isAlreadyMounted(const std::string& mountPoint) {
    struct statvfs vfs;
    if (statvfs(mountPoint.c_str(), &vfs) != 0) {
        return false; // Error or doesn't exist
    }

    // Check if it's a mount point
    return (vfs.f_flag & ST_NODEV) == 0;
}


void mountIsoFile(const std::vector<std::string>& isoFilesToMount,std::set<std::string>& mountedFiles,std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails) {
	namespace fs = std::filesystem;
    
    const std::vector<std::string> fsTypes = {
        "iso9660", "udf", "hfsplus", "rockridge", "joliet", "isofs", "auto"
    };

    for (const auto& isoFile : isoFilesToMount) {
        // Extract directory and filename from isoFile
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);

        // Check if this ISO file is in the global failed set
        {
            std::lock_guard<std::mutex> lock(globalFailedISOMutex);
            if (globalFailedISOs.find(isoFile) != globalFailedISOs.end()) {
                // Skip this ISO file if it's in the global failed set
                std::stringstream skippedMessage;
                skippedMessage << "\033[1;93mISO: \033[1;91m'" << isoDirectory << "/" << isoFilename 
                               << "'\033[1;93m skipped due to previous mount failure {badFS}. To clear status 'clr' ↵ in selection for mount.\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    mountedFails.insert(skippedMessage.str());
                }
                continue;
            }
        }

        // Construct unique mount point based on isoFile
        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();
        
        // Create a hash of the full path
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        
        // Convert hash to a base36 string
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }
        
        // Create a unique identifier
        std::string uniqueId = isoFileName + "\033[38;5;245m~" + shortHash + "\033[1;94m";
        std::string mountPoint = "/mnt/iso_" + uniqueId;

        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);

        // Check if mount point is already mounted
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
        
        // Check for root privileges (unchanged)
        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "'\033[0m\033[1;91m. Root privileges are required.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
            continue;
        }
        
        // Check and create the mount point directory
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
        
        for (const auto& fsType : fsTypes) {
            // Initialize libmount context
            struct libmnt_context* cxt = mnt_new_context();
            if (!cxt) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to initialize mount context for: \033[1;93m'" 
                             << isoDirectory << "/" << isoFilename << "'\033[0m\033[1;91m.\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    mountedFails.insert(errorMessage.str());
                }
                break;
            }
            
            // Set mount options directly on the context
            mnt_context_set_source(cxt, isoFile.c_str());
            mnt_context_set_target(cxt, mountPoint.c_str());
            mnt_context_set_fstype(cxt, fsType.c_str());  // Ensure fsType is a valid filesystem type
            mnt_context_set_options(cxt, "loop,ro");
            
            // Attempt to mount (unchanged)
            int ret = mnt_context_mount(cxt);
            
            // Check if mount was successful
            if (ret == 0) {
                // Successfully mounted
                std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
												+ "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
												+ "'\033[0;1m. {" + fsType + "}\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    mountedFiles.insert(mountedFileInfo);
                }
                mountSuccess = true;
                mnt_free_context(cxt);
                break;
            }
            
            // Free the context
            mnt_free_context(cxt);
        }
        
        if (!mountSuccess) {
            // Mount failure after trying all filesystem types
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[1;91m.\033[0;1m {badFS}";
            fs::remove(mountPoint);
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }

            // Store the failed ISO file in the global set
            {
                std::lock_guard<std::mutex> lock(globalFailedISOMutex);
                globalFailedISOs.insert(isoFile);
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
                        tokens.insert(std::to_string(i));
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
				tokens.insert(tokenCount);
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
        shouldProcess = validIndices.insert(index).second;
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
		uniqueErrorMessages.insert(error);
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
				shouldProcessRange = processedRanges.insert(range).second;
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
			bool shouldProcess = processedIndices.insert(num).second;
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
