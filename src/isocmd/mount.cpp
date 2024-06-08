#include "../headers.h"
#include "../threadpool.h"


//	MOUNT STUFF

void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails) {

  // Detect and use the minimum of available threads and ISOs to ensure efficient parallelism
  unsigned int numThreads = std::min(static_cast<int>(isoFiles.size()), static_cast<int>(maxThreads));

  // Create a ThreadPool with maxThreads
  ThreadPool pool(numThreads);

  // Process all ISO files asynchronously
  for (size_t i = 0; i < isoFiles.size(); ++i) {
    // Capture isoFiles by reference and i by value
    pool.enqueue([i, &isoFiles, &mountedFiles, &skippedMessages, &mountedFails]() {
      // No need to create a local copy of isoFiles[i]
      // Use isoFiles[i] directly within the lambda
      std::vector<std::string> isoFilesToMountLocal = { isoFiles[i] };

      // Call mountIsoFile with the vector of ISO files to mount and the mounted set
      mountIsoFile(isoFilesToMountLocal, mountedFiles, skippedMessages, mountedFails);
    });
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
	
	std::set<std::string> uniqueErrorMessages;
	
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
        clearScrollBuffer();
        std::cout << "\033[1;93mISO Cache is empty. Please refresh it from the main Menu Options.\033[0;1m\n";
        std::cout << " \n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Set to track mounted ISO files
    std::vector<std::string> isoFilesToMount;

    // Main loop for selecting and mounting ISO files
    while (true) {
		std::vector<std::string> isoFilesToMount;
		bool verboseFiltered = false;
        clearScrollBuffer();
        std::cout << "\033[1;93m ! IF EXPECTED ISO FILE(S) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\033[0;1m\n";
        std::cout << "\033[1;93m         	! ROOT ACCESS IS PARAMOUNT FOR SUCCESSFUL MOUNTS !\n\033[0;1m";

        // Remove non-existent paths from the cache after selection
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
        isoFiles = loadCache();
        std::string searchQuery;
        std::vector<std::string> filteredFiles;
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
						char* input = readline("\n\n\001\033[1;92m\002ISO(s)\001\033[1;94m\002 ↵ for \001\033[1;92m\002mount\001\033[1;94m\002 (e.g., '1-3', '1 5', '00' for all), or ↵ to return:\001\033[0;1m\002 ");
					
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
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails) {
    namespace fs = std::filesystem;
    std::vector<std::future<void>> futures;

    for (const auto& isoFile : isoFilesToMount) {
        futures.push_back(std::async(std::launch::async, [&isoFile, &mountedFiles, &skippedMessages, &mountedFails]() {
            fs::path isoPath(isoFile);
            std::string isoFileName = isoPath.stem().string();
            std::string mountPoint = "/mnt/iso_" + isoFileName;

            // Check and create the mount point directory
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                if (!fs::exists(mountPoint)) {
                    fs::create_directory(mountPoint);
                }
            }

            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
            auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);

            if (isAlreadyMounted(mountPoint)) {
                std::stringstream skippedMessage;
                skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m already mounted at: \033[1;94m'" << mountisoDirectory << "/" << mountisoFilename << "'\033[1;93m.\033[0;1m";

                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                skippedMessages.insert(skippedMessage.str());
                return;
            }

            // Initialize libmount context
            struct libmnt_context* cxt = mnt_new_context();
            struct libmnt_cache* cache = mnt_new_cache();
            struct libmnt_fs* fs = mnt_new_fs();

            mnt_fs_set_source(fs, isoFile.c_str());
            mnt_fs_set_target(fs, mountPoint.c_str());
            mnt_fs_set_fstype(fs, "iso9660");
            mnt_fs_set_options(fs, "loop");

            mnt_context_set_fs(cxt, fs);

            int ret = mnt_context_mount(cxt);
            if (ret != 0) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0;1m\033[1;91m.\033[0;1m";
                fs::remove(mountPoint);

                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            } else {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0;1m"
                                              + "\033[1m mounted at: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename + "'\033[0;1m\033[1m.\033[0;1m";
                mountedFiles.insert(mountedFileInfo);
            }

            mnt_free_fs(fs);
            mnt_free_cache(cache);
            mnt_free_context(cxt);
        }));
    }

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails,std::set<std::string>& uniqueErrorMessages) {
    std::istringstream iss(input);  // Create an input string stream from the input string

    // Determine the number of threads to use, based on the number of ISO files and hardware concurrency
    unsigned int numThreads = std::min(static_cast<int>(isoFiles.size()), static_cast<int>(std::thread::hardware_concurrency()));

    bool invalidInput = false;  // Flag to indicate invalid input
    std::set<int> processedIndices;  // Set to track processed indices
    std::set<int> validIndices;      // Set to track valid indices
    std::set<std::pair<int, int>> processedRanges;  // Set to track processed ranges

    ThreadPool pool(numThreads);  // Thread pool with the determined number of threads
    std::mutex MutexForProcessedIndices;  // Mutex for protecting access to processedIndices
    std::mutex MutexForValidIndices;      // Mutex for protecting access to validIndices

    std::string token;
    while (iss >> token) {  // Iterate through each token in the input
        if (token == "/") {  // Break the loop if a '/' is encountered
            break;
        }

        // Handle special case for input "00"
        if (token != "00" && isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0;1m");
            }
            continue;
        }

        // Check if the token contains a dash ('-'), indicating a range
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            // Check for invalid range format
            if (token.find('-', dashPos + 1) != std::string::npos || 
                (dashPos == 0 || dashPos == token.size() - 1 || !std::isdigit(token[dashPos - 1]) || !std::isdigit(token[dashPos + 1]))) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            }

            int start, end;
            try {
                // Parse the start and end of the range
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument&) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range&) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0;1m");
                continue;
            }

            // Validate range
            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0;1m");
                continue;
            }

            std::pair<int, int> range(start, end);
            if (processedRanges.find(range) == processedRanges.end()) {
                processedRanges.insert(range);

                // Determine step direction and iterate through the range
                int step = (start <= end) ? 1 : -1;
                for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                    if (processedIndices.find(i) == processedIndices.end()) {
                        processedIndices.insert(i);

                        pool.enqueue([&, i]() {  // Enqueue a task to the thread pool
                            std::lock_guard<std::mutex> validLock(MutexForValidIndices);  // Protect access to validIndices
                            if (validIndices.find(i) == validIndices.end()) {
                                validIndices.insert(i);
                                std::vector<std::string> isoFilesToMount;
                                isoFilesToMount.push_back(isoFiles[i - 1]);
                                mountIsoFile(isoFilesToMount, mountedFiles, skippedMessages, mountedFails);  // Mount the ISO file
                            }
                        });
                    }
                }
            }
        } else if (isNumeric(token)) {  // Handle single numeric token
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && processedIndices.find(num) == processedIndices.end()) {
                processedIndices.insert(num);

                pool.enqueue([&, num]() {  // Enqueue a task to the thread pool
                    std::lock_guard<std::mutex> validLock(MutexForValidIndices);  // Protect access to validIndices
                    if (validIndices.find(num) == validIndices.end()) {
                        validIndices.insert(num);
                        std::vector<std::string> isoFilesToMount;
                        isoFilesToMount.push_back(isoFiles[num - 1]);
                        mountIsoFile(isoFilesToMount, mountedFiles, skippedMessages, mountedFails);  // Mount the ISO file
                    }
                });
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[0;1m");
            }
        } else {  // Handle invalid token
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }
}


// Function to check if an ISO is already mounted
bool isAlreadyMounted(const std::string& mountPoint) {
    FILE* mountTable = setmntent("/proc/mounts", "r");
    if (!mountTable) {
        // Failed to open mount table
        return false;
    }

    mntent* entry;
    while ((entry = getmntent(mountTable)) != nullptr) {
        if (std::strcmp(entry->mnt_dir, mountPoint.c_str()) == 0) {
            // Found the mount point in the mount table
            endmntent(mountTable);
            return true;
        }
    }

    endmntent(mountTable);
    return false;
}
