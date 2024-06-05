#include "../headers.h"
#include "../threadpool.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here

std::mutex fileCheckMutex;
    
// Set to track processed error messages to avoid duplicate error reporting
std::set<std::string> processedErrors;

// Set to track succesful conversions
std::set<std::string> successOuts;

// Set to track skipped conversions
std::set<std::string> skippedOuts;

// Set to track failed conversions
std::set<std::string> failedOuts;

// Set to track deleted conversions for ccd2iso only
std::set<std::string> deletedOuts;



// GENERAL

// Function to check if a file already exists
bool fileExistsConversions(const std::string& fullPath) {
    std::lock_guard<std::mutex> lock(fileCheckMutex);
        return std::filesystem::exists(fullPath);
}

// Function to print verbose conversion messages
void verboseConversion() {
    // Lambda function to print each element in a set followed by a newline
    auto printWithNewline = [](const std::set<std::string>& outs) {
        for (const auto& out : outs) {
            std::cout << out << "\033[0;1m\n"; // Print each element in the set
        }
        if (!outs.empty()) {
            std::cout << "\n"; // Print an additional newline if the set is not empty
        }
    };

    // Print each set of messages with a newline after each set
    printWithNewline(successOuts);   // Print success messages
    printWithNewline(skippedOuts);   // Print skipped messages
    printWithNewline(deletedOuts);   // Print deleted messages
    printWithNewline(processedErrors); // Print error messages
    
    // Clear all sets after printing
    successOuts.clear();   // Clear the set of success messages
    skippedOuts.clear();   // Clear the set of skipped messages
    deletedOuts.clear();   // Clear the set of deleted messages
    processedErrors.clear(); // Clear the set of error messages
}


// Function to select and convert files based on user's choice of file type
void select_and_convert_files_to_iso(const std::string& fileTypeChoice) {
    // Initialize variables
    std::vector<std::string> files;
    std::vector<std::string> directoryPaths;
    std::set<std::string> uniquePaths;
    std::set<std::string> invalidDirectoryPaths;
    bool flag = false;

    std::string fileExtension;
    std::string fileTypeName;
    
    // Convert the first character of fileTypeChoice to lowercase
    std::string fileType = std::string(1, std::tolower(fileTypeChoice[0])) + fileTypeChoice.substr(1);

    // Determine file extension and type name based on user input
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin;.img";
        fileTypeName = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileTypeName = "MDF";
    } else {
        // Print error message for unsupported file types
        std::cout << "Invalid file type choice. Supported types: BIN/IMG, MDF\n";
        return;
    }

    // Load search history
    loadHistory();
    
    // Prompt user to input directory paths
    std::string inputPaths = readInputLine("\033[1;94mDirectory path(s) ↵ (multi-path separator: \033[1m\033[1;93m;\033[0;1m\033[1;94m) to search for \033[1m\033[1;92m" + fileExtension + " \033[1;94mfiles, or ↵ to return:\n\033[0;1m");
    clearScrollBuffer();
    
    if (!inputPaths.empty()) {
        // Save search history if input paths are provided
        std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
        saveHistory();
    }

    // Clear command line history
    clear_history();

    // Record start time for performance measurement
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Split inputPaths into individual directory paths
    std::istringstream iss(inputPaths);
    std::string path;
    while (std::getline(iss, path, ';')) {
        size_t start = path.find_first_not_of(" \t");
        size_t end = path.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            std::string cleanedPath = path.substr(start, end - start + 1);
            if (uniquePaths.find(cleanedPath) == uniquePaths.end()) {
                if (directoryExists(cleanedPath)) {
                    directoryPaths.push_back(cleanedPath);
                    uniquePaths.insert(cleanedPath);
                } else {
                    std::string invalid = "\n\033[1;91m" + cleanedPath;
                    invalidDirectoryPaths.insert(invalid);
                }
            }
        }
    }
    if (!invalidDirectoryPaths.empty()) {
		clearScrollBuffer();
		if (directoryPaths.empty()) {
			std::cout << "\033[1;91mNo valid paths provided.";
		} else {
			std::cout << "\033[1;93mThe following invalid path(s) will be omitted from search:\n";
			for (const auto& invalidPath : invalidDirectoryPaths) {
				std::cerr << invalidPath;
			}
		}
		invalidDirectoryPaths.clear();
		std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		clearScrollBuffer();
		std::cout << "\033[1mPlease wait...\033[1m" << std::endl;
	}
	
	// Return if no directory paths are provided
    if (directoryPaths.empty()) {
        return;
    }

    // Search for files based on file type
    bool newFilesFound = false;
    if (fileType == "bin" || fileType == "img") {
        files = findFiles(directoryPaths, "bin", [&](const std::string& fileName, const std::string& filePath) {
            flag = false;
            newFilesFound = true;
        });
    } else if (fileType == "mdf") {
        files = findFiles(directoryPaths, "mdf", [&](const std::string& fileName, const std::string& filePath) {
            flag = true;
            newFilesFound = true;
        });
    }

    // Display message if no new files are found
    if (!newFilesFound && !files.empty()) {
        clearScrollBuffer();
        std::cout << "\n";
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\033[1;91mNo new " << fileExtension << " file(s) over 5MB found. \033[1;92m" << files.size() << " file(s) are cached in RAM from previous searches.\033[0;1m\n";
        std::cout << "\n";
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    // Display message if no files are found
    if (files.empty()) {
        clearScrollBuffer();
        std::cout << "\n";
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\033[1;91mNo " << fileExtension << " file(s) over 5MB found in the specified path(s) or cached in RAM.\n\033[0;1m";
        std::cout << "\n";
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Main loop for file selection and conversion
    while (true) {
        // Display file list and prompt user for input
        clearScrollBuffer();
        std::cout << "\033[94;1mSUCCESSFUL CONVERSIONS ARE AUTOMATICALLY ADDED INTO ISO CACHE\033[0;1m\033[0;1m\n\n";
        printFileList(files);

        clear_history();

        std::string prompt = "\n\001\033[1;38;5;208m\002" + fileTypeName + " \001\033[1;94m\002file(s) ↵ for conversion (e.g., '1-3', '1 5'), / ↵ to filter, or ↵ to return:\001\033[0;1m\002 ";
        char* input = readline(prompt.c_str());

        // Check if user wants to return
        if (std::isspace(input[0]) || input[0] == '\0') {
            free(input);
            clearScrollBuffer();
            break;
        }
			bool isFiltered = false;
			if (strcmp(input, "/") == 0) { // Check if the input is "/"
				isFiltered = true;
				while (true) { // Enter an infinite loop for handling input
				// Clear history for a fresh start
				clear_history();

				historyPattern = true; // Set history pattern to true
				loadHistory(); // Load history from previous sessions

				clearScrollBuffer(); // Clear scroll buffer to prepare for new content

				std::string prompt; // Define a string variable for the input prompt
				if (fileType == "bin" || fileType == "img") { // Check the file type
				// Prompt for BIN/IMG files
					prompt = "\n\001\033[1;92m\002SearchQuery\001\033[1;94m\002 ↵ to filter \001\033[1;38;5;208m\002BIN/IMG\001\033[1;94m\002 list (ccase-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0;1m\002";
				} else if (fileType == "mdf") {
				// Prompt for MDF files
					prompt = "\n\001\033[1;92m\002SearchQuery\001\033[1;94m\002 ↵ to filter \001\033[1;38;5;208m\002MDF\001\033[1;94m\002 conversion list (case-insensitive, multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), or ↵ to return: \001\033[0;1m\002";
				}

				char* searchQuery = readline(prompt.c_str()); // Get input from the user

				clearScrollBuffer(); // Clear scroll buffer to prepare for new content

				if (searchQuery != nullptr && searchQuery[0] != '\0') { // Check if the search query is not empty
					std::cout << "\033[1mPlease wait...\033[1m\n"; // Inform user to wait
					add_history(searchQuery); // Add the search query to the history
					saveHistory(); // Save the history
				}
				clear_history(); // Clear history for fresh start

				if (std::isspace(searchQuery[0]) || searchQuery[0] == '\0') { // Check if the search query is empty or contains only spaces
					free(searchQuery); // Free memory allocated for search query
					historyPattern = false; // Set history pattern to false
					isFiltered = false;
					break; // Exit the loop
				}

				// Filter files based on the search query
				std::vector<std::string> filteredFiles = filterFiles(files, searchQuery);
				free(searchQuery); // Free memory allocated for search query

				if (filteredFiles.empty()) { // Check if no files match the search query
					clearScrollBuffer(); // Clear scroll buffer
					std::cout << "\033[1;91mNo file(s) match the search query.\033[0;1m\n"; // Inform user
					std::cout << "\n\033[1;32m↵ to continue...\033[0;1m"; // Prompt user to continue
					std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
				} else {
					while (true) { // Enter another loop for handling filtered results
						clearScrollBuffer(); // Clear scroll buffer
						clear_history(); // Clear history for fresh start
						std::cout << "\033[1mFiltered results:\n\033[0;1m\n"; // Display filtered results header
						printFileList(filteredFiles); // Print filtered file list

						std::string filterPrompt; // Define a string variable for filter prompt
						if (fileType == "bin" || fileType == "img") { // Check file type
							// Prompt for BIN/IMG files
							filterPrompt = "\n\001\033[1;94m\033[1;38;5;208m\002BIN/IMG\001\033[1;94m\002 file(s) ↵ for conversion (e.g., '1-3', '1 5'), or ↵ to return:\001\033[0;1m\002 ";
						} else if (fileType == "mdf") {
							// Prompt for MDF files
							filterPrompt = "\n\n\001\033[1;94m\033[1;38;5;208m\002MDF\001\033[1;94m\002 file(s) ↵ for conversion (e.g., '1-3', '1 5'), or ↵ to return:\001\033[0;1m\002 ";
						}
						char* filterInput = readline(filterPrompt.c_str()); // Get input for file conversion

						// Check if the user wants to return
						if (std::isspace(input[0]) || input[0] == '\0') {
							free(input); // Free memory allocated for input
							historyPattern = false; // Set history pattern to false
							isFiltered = false;
							break; // Exit the loop
						}

						if (std::isspace(filterInput[0]) || filterInput[0] == '\0') { // Check if filter input is empty or contains only spaces
							free(filterInput); // Free memory allocated for filter input
							historyPattern = false; // Set history pattern to false
							isFiltered = false;
							break; // Exit the loop
						}
						if (isFiltered) {
							clearScrollBuffer(); // Clear scroll buffer
							std::cout << "\033[1mPlease wait...\n\033[1m\n"; // Inform user to wait
							processInput(filterInput, filteredFiles, inputPaths, flag); // Process user input
							free(filterInput); // Free memory allocated for filter input
							
							clearScrollBuffer(); // Clear scroll buffer
							std::cout << "\n"; // Print newline
			
							verboseConversion();

							std::cout << "\033[1;32m↵ to continue...\033[0;1m"; // Prompt user to continue
							std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
						}
					}
				}
			}
		} else {
			// If input is not "/", process the input
			clearScrollBuffer(); // Clear scroll buffer
			std::cout << "\033[1mPlease wait...\n\033[1m\n"; // Inform user to wait
			processInput(input, files, inputPaths, flag); // Process input
			free(input); // Free memory allocated for input
			
			clearScrollBuffer(); // Clear scroll buffer
			std::cout << "\n"; // Print newline
			
			verboseConversion();
			
		std::cout << "\033[1;32m↵ to continue...\033[0;1m"; // Prompt user to continue
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		}
	}
}


// Function to process user input and convert selected BIN files to ISO format
void processInput(const std::string& input, const std::vector<std::string>& fileList, const std::string& inputPaths, bool flag) {

    // Mutexes to protect the critical sections
    std::mutex indicesMutex;
    std::mutex errorsMutex;

    // Create a string stream to tokenize the input
    std::istringstream iss(input);
    std::string token;
    
    // Set to track processed indices to avoid duplicates
	std::set<int> processedIndices;

    // Vector to store asynchronous tasks for file conversion
    std::vector<std::future<void>> futures;

    // Protect the critical section with a lock
    std::lock_guard<std::mutex> lock(indicesMutex);
    
    ThreadPool pool(maxThreads);
    
    // Get current user and group
    char* current_user = getlogin();
    if (current_user == nullptr) {
		std::cerr << "Error getting current user: " << strerror(errno) << "\033[0;1m\n";
		return;
    }
    gid_t current_group = getegid();
    if (current_group == static_cast<unsigned int>(-1)) {
		std::cerr << "\033[1;91mError getting current group:\033[0;1m " << strerror(errno) << "\033[0;1m\n";
		return;
    }
    std::string user_str(current_user);
    std::string group_str = std::to_string(static_cast<unsigned int>(current_group));

    // Function to execute asynchronously
    auto asyncConvertBINToISO = [&](const std::string& selectedFile) {
        convertBINToISO(selectedFile);
		};
		// Function to execute asynchronously
		auto asyncConvertMDFToISO = [&](const std::string& selectedFile) {
        convertMDFToISO(selectedFile);
		};
	
	// Start the timer
  //  auto start_time = std::chrono::high_resolution_clock::now();
    // Iterate through the tokens in the input string
    while (iss >> token) {
        // Create a string stream to further process the token
        std::istringstream tokenStream(token);
        int start, end;
        char dash;

        // Check if the token can be converted to an integer (starting index)
        if (tokenStream >> start) {
            // Check for the presence of a dash, indicating a range
            if (tokenStream >> dash) {
                if (dash == '-') {
                    // Parse the end of the range
                    if (tokenStream >> end) {
                        // Check for additional hyphens in the range
                        if (tokenStream >> dash) {
                            // Add an error message for ranges with more than one hyphen
                            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[0;1m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                processedErrors.insert(errorMessage);
                            }
                        } else if (start > 0 && end > 0 && start <= static_cast<int>(fileList.size()) && end <= static_cast<int>(fileList.size())) {  // Check if the range is valid
                            // Handle a valid range
                            int step = (start <= end) ? 1 : -1; // Determine the step based on the range direction
                            int selectedIndex;  // Declare selectedIndex outside of the loop

                            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                                selectedIndex = i - 1;
                                if (selectedIndex >= 0 && static_cast<std::vector<std::string>::size_type>(selectedIndex) < fileList.size()) {
                                    if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                                        std::string selectedFile = fileList[selectedIndex];
                                        if (!flag) {
											futures.push_back(pool.enqueue(asyncConvertBINToISO, selectedFile));
										} else {
											futures.push_back(pool.enqueue(asyncConvertMDFToISO, selectedFile));
										}
										processedIndices.insert(selectedIndex);
    
										
                                    }
                                } else {
                                    // Add an error message for an invalid range
                                    std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0;1m";
                                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                        // Protect the critical section with a lock
                                        std::lock_guard<std::mutex> lock(errorsMutex);
                                        processedErrors.insert(errorMessage);
                                    }
                                    break; // Exit the loop to avoid further errors
                                }
                            }
                        } else {
                            // Add an error message for an invalid range
                            std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0;1m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                processedErrors.insert(errorMessage);
                            }
                        }
                    } else {
                        // Add an error message for an invalid range format
                        std::string errorMessage = "\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0;1m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            processedErrors.insert(errorMessage);
                        }
                    }
                } else {
                    // Add an error message for an invalid character after the dash
                    std::string errorMessage = "\033[1;91mInvalid character after dash in range: '" + token + "'.\033[0;1m";
                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                        // Protect the critical section with a lock
                        std::lock_guard<std::mutex> lock(errorsMutex);
                        processedErrors.insert(errorMessage);
                    }
                }
            } else if (static_cast<std::vector<std::string>::size_type>(start) >= 1 && static_cast<std::vector<std::string>::size_type>(start) <= fileList.size()) {
                // Process a single index
                int selectedIndex = start - 1;
                if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                    if (selectedIndex >= 0 && static_cast<std::vector<std::string>::size_type>(selectedIndex) < fileList.size()) {
                        // Convert BIN to ISO asynchronously and store the future in the vector
                        std::string selectedFile = fileList[selectedIndex];
						if (!flag) {
							futures.push_back(pool.enqueue(asyncConvertBINToISO, selectedFile));
						} else {
						futures.push_back(pool.enqueue(asyncConvertMDFToISO, selectedFile));
					}
						processedIndices.insert(selectedIndex);
							
                    } else {
                        // Add an error message for an invalid file index
                        std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[0;1m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            processedErrors.insert(errorMessage);
                        }
                    }
                }
            } else {
                // Add an error message for an invalid file index
                std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[0;1m";
                if (processedErrors.find(errorMessage) == processedErrors.end()) {
                    // Protect the critical section with a lock
                    std::lock_guard<std::mutex> lock(errorsMutex);
                    processedErrors.insert(errorMessage);
                }
            }
        } else {
            // Add an error message for an invalid input
            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[0;1m";
            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                // Protect the critical section with a lock
                std::lock_guard<std::mutex> lock(errorsMutex);
                processedErrors.insert(errorMessage);
            }
        }
    }

    // Wait for all futures to finish
    for (auto& future : futures) {
        future.wait();
    }
    
    promptFlag = false;
    
    manualRefreshCache(inputPaths);
    
    // Stop the timer after completing the mounting process
  //  auto end_time = std::chrono::high_resolution_clock::now();
    // Calculate and print the elapsed time
  //  auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
 //   std::cout << "\n";
    // Print the time taken for the entire process in bold with one decimal place
  //  std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        
}


// Function to search for .bin and .img files over 5MB
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback) {
    // Vector to store cached invalid paths
    static std::vector<std::string> cachedInvalidPaths;
    
    // Vector to store permission errors
    std::set<std::string> uniqueInvalidPaths;

    // Static variables to cache results for reuse
    static std::vector<std::string> binImgFilesCache;
    
    // Static variables to cache results for reuse
    static std::vector<std::string> mdfMdsFilesCache;
    
    // Set to store processed paths
    static std::set<std::string> processedPathsMdf;
    
    // Set to store processed paths
    static std::set<std::string> processedPathsBin;
    
    bool blacklistMdf =false;

    // Vector to store file names that match the criteria
    std::set<std::string> fileNames;

    // Clear the cachedInvalidPaths before processing a new set of paths
    cachedInvalidPaths.clear();

    bool printedEmptyLine = false;  // Flag to track if an empty line has been printed
    
    // Mutex to ensure thread safety
    std::mutex mutex4search;
    
    // Start the timer
//    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Preallocate enough space for the futures vector
        size_t totalFiles = 0;
        for (const auto& path : paths) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    totalFiles++;
                }
            }
        }
        
        std::vector<std::future<void>> futures;
        futures.reserve(totalFiles);
		
        // Counter to track the number of ongoing tasks
        unsigned int numOngoingTasks = 0;

        // Iterate through input paths
		for (const auto& path : paths) {
			if (mode == "bin") {
				// Check if the path has already been processed in "bin" mode
				if (processedPathsBin.find(path) != processedPathsBin.end()) {
					continue; // Skip already processed paths
				}
			} else if (mode == "mdf") {
				// Check if the path has already been processed in "mdf" mode
				if (processedPathsMdf.find(path) != processedPathsMdf.end()) {
					continue; // Skip already processed paths
				}
			}

            try {
                // Use a lambda function to process files asynchronously
                auto processFileAsync = [&](const std::filesystem::directory_entry& entry) {
                    std::string fileName = entry.path().string();
                    std::string filePath = entry.path().parent_path().string();  // Get the path of the directory

                    // Call the callback function to inform about the found file
                    callback(fileName, filePath);

                    // Lock the mutex to ensure safe access to shared data (fileNames and numOngoingTasks)
                    std::lock_guard<std::mutex> lock(mutex4search);

                    // Add the file name to the shared data
                    fileNames.insert(fileName);

                    // Decrement the ongoing tasks counter
                    --numOngoingTasks;
                };

                // Use async to process files concurrently
                // Iterate through files in the given directory and its subdirectories
                if (mode == "bin") {
					blacklistMdf = false;
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                        if (entry.is_regular_file()) {
                            // Checks .bin .img blacklist
                            if (blacklist(entry, blacklistMdf)) {
                                // Check if the file is already present in the cache to avoid duplicates
                                std::string fileName = entry.path().string();
                                if (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) == binImgFilesCache.end()) {
                                    // Process the file asynchronously
                                    if (numOngoingTasks < maxThreads) {
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    } else {
                                        // Wait for one of the ongoing tasks to complete before adding a new task
                                        for (auto& future : futures) {
                                            if (future.valid()) {
                                                future.get();
                                            }
                                        }
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    }
                                }
                            }
                        }
                        processedPathsBin.insert(path);
                    }
                } else {                    
                    blacklistMdf = true;
                    
                    // Iterate through files in the given directory and its subdirectories
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                        if (entry.is_regular_file()) { 
                            if (blacklist(entry, blacklistMdf)) {
                                // Check if the file is already present in the cache to avoid duplicates
                                std::string fileName = entry.path().string();
                                if (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) == mdfMdsFilesCache.end()) {
                                    // Process the file asynchronously
                                    if (numOngoingTasks < maxThreads) {
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    } else {
                                        // Wait for one of the ongoing tasks to complete before adding a new task
                                        for (auto& future : futures) {
                                            if (future.valid()) {
                                                future.get();
                                            }
                                        }
                                        // Increment the ongoing tasks counter
                                        ++numOngoingTasks;
                                        std::lock_guard<std::mutex> lock(mutex4search);
                                        // Process the file asynchronously
                                        futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                                    }
                                }
                            }
                        }
                    }

                    // Wait for remaining asynchronous tasks to complete
                    for (auto& future : futures) {
                        // Check if the future is valid
                        if (future.valid()) {
                            // Block until the future is ready
                            future.get();
                        }
                    }
                    
                    // Add the processed path to the set
                    processedPathsMdf.insert(path);
                }
            } catch (const std::filesystem::filesystem_error& e) {
                std::lock_guard<std::mutex> lock(mutex4search);

                // Check if the exception is related to a permission error
                const std::error_code& ec = e.code();
                if (ec == std::errc::permission_denied) {
                    // Check if the path is unique
                    if (uniqueInvalidPaths.insert(path).second) {
                        // If it's a new path, print an empty line before printing the error (only once)
                        if (!printedEmptyLine) {
                            std::cout << "\n";
                            printedEmptyLine = true;
                        }
                        // Handle permission error differently, you can choose to skip or print a specific message
                        std::cerr << "\033[1;91mInsufficient permissions for directory path: \033[1;93m'" << path << "'\033[1;91m.\033[0;1m\n";
                    }
                } else if (std::find(cachedInvalidPaths.begin(), cachedInvalidPaths.end(), path) == cachedInvalidPaths.end()) {
                    if (!printedEmptyLine) {
                        // Print an empty line before starting to print invalid paths (only once)
                        std::cout << "\n";
                        printedEmptyLine = true;
                    }

                    // Print the specific error details for non-permission errors
                    std::cerr << "\033[1;91m" << e.what() << ".\033[0;1m\n";

                    // Add the invalid path to cachedInvalidPaths to avoid duplicate error messages
                    cachedInvalidPaths.push_back(path);
                }
            }
        }

    } catch (const std::filesystem::filesystem_error& e) {
        if (!printedEmptyLine) {
            // Print an empty line before starting to print invalid paths (only once)
            std::cout << "\n";
            printedEmptyLine = true;
        }
        // Handle filesystem errors for the overall operation
        std::cerr << "\033[1;91m" << e.what() << ".\033[0;1m\n";
    }

    // Print success message if files were found
    if (!fileNames.empty()) {
    
        // Stop the timer after completing the mounting process
    //    auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\n";
        if (mode == "bin") {
			clearScrollBuffer();
			std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[0;1m" << ".\033[1;93m " << binImgFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[0;1m\n";
		} else {
			clearScrollBuffer();
			std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[0;1m" << ".\033[1;93m " << mdfMdsFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[0;1m\n";
		}
        // Calculate and print the elapsed time
   //     std::cout << "\n";
   //     auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
    //    std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        std::cout << "\n";
        std::cout << "\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    
    std::lock_guard<std::mutex> lock(mutex4search);
    if (mode == "bin") {
        // Update the cache by appending fileNames to binImgFilesCache
        binImgFilesCache.insert(binImgFilesCache.end(), fileNames.begin(), fileNames.end());

        // Return the combined results
        return binImgFilesCache;
    }
    
    if (mode == "mdf") {
        // Update the cache by appending fileNames to mdfMdsFilesCache
        mdfMdsFilesCache.insert(mdfMdsFilesCache.end(), fileNames.begin(), fileNames.end());
        
        // Return the combined results
        return mdfMdsFilesCache;
    }

    // Return an empty vector if mode is neither "bin" nor "mdf"
    return std::vector<std::string>();
}


// Blacklist function for MDF BIN IMG
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();

    // Convert the extension to lowercase for case-insensitive comparison
    std::string extLower = ext;
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](char c) {
        return std::tolower(c);
    });

    // Combine extension checks
    if (!blacklistMdf) {
		if (!((extLower == ".bin" || extLower == ".img"))) {
			return false;
		}
	} else {
		if (!((extLower == ".mdf"))) {
			return false;
		}
	}

    // Check file size
    if (std::filesystem::file_size(entry) <= 5'000'000) {
        return false;
    }

    // Use a set for blacklisted keywords
    std::set<std::string> blacklistKeywords = {
        "block", "list", "sdcard", "index", "data", "shader", "navmesh",
        "obj", "terrain", "script", "history", "system", "vendor", "flora",
        "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
    };

    // Add blacklisted keywords for .mdf extension
    if (extLower == ".mdf") {
        blacklistKeywords.insert({
           // "flora", "terrain", "script", "history", "system", "vendor",
           // "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
        });
    }

    // Convert the filename to lowercase for additional case-insensitive comparisons
    std::string filenameLowerNoExt = filenameLower;
    filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size()); // Remove extension
    std::transform(filenameLowerNoExt.begin(), filenameLowerNoExt.end(), filenameLowerNoExt.begin(), [](char c) {
        return std::tolower(c);
    });

    // Check if any blacklisted word is present in the filename
    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}



// Function to print found BIN/IMG files with alternating colored sequence numbers
void printFileList(const std::vector<std::string>& fileList) {
    // ANSI escape codes for text formatting
    const std::string bold = "\033[1m";
    const std::string reset = "\033[0m";
    const std::string red = "\033[31;1m"; // Red color
    const std::string green = "\033[32;1m"; // Green color
    const std::string orangeBold = "\033[1;38;5;208m";

    // Toggle between red and green for sequence number coloring
    bool useRedColor = true;

    // Print header for file selection
    std::cout << bold << "Select file(s) to convert to " << bold << "\033[1;92mISO(s)\033[0;1m:\n";
    std::cout << "\n";

    // Counter for line numbering
    int lineNumber = 1;
    int stew = 1;
    
    if (fileList.size() >= 10) {
			stew = 2;
		} else if (fileList.size() >= 100) {
			stew = 3;
		} else if (fileList.size() >= 1000) {
			stew = 4;
		} else if (fileList.size() >= 10000) {
			stew = 5;
		} else if (fileList.size() >= 100000) {
			stew = 6;
		}	

    // Apply formatting once before the loop
    std::cout << std::right << std::setw(stew);

    for (const auto& filename : fileList) {
        // Extract directory and filename
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(filename);

        const std::size_t dotPos = fileNameOnly.find_last_of('.');
        std::string extension;

        // Check if the file has a .bin, .img, or .mdf extension (case-insensitive)
        if (dotPos != std::string::npos) {
            extension = fileNameOnly.substr(dotPos);
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            if (extension == ".bin" || extension == ".img" || extension == ".mdf") {
                // Determine color for sequence number based on toggle
                std::string sequenceColor = (useRedColor) ? red : green;
                useRedColor = !useRedColor; // Toggle between red and green

                // Print sequence number in the determined color and the rest in default color
                std::cout << sequenceColor << std::setw(2) << std::right << lineNumber << ". " << reset;
                std::cout << bold << directory << bold << "/" << orangeBold << fileNameOnly << reset << "\033[0;1m\n";
            } else {
                // Print entire path and filename with the default color
                std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << "\033[0;1m\n";
            }
        } else {
            // No extension found, print entire path and filename with the default color
            std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << "\033[0;1m\n";
        }

        // Increment line number
        lineNumber++;
    }
}


// BIN/IMG CONVERSION FUNCTIONS


// Function to convert a BIN file to ISO format
void convertBINToISO(const std::string& inputPath) {
	
	auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m does not exist.\033[0;1m\n";
        return;
    }

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Check if the output ISO file already exists
    if (fileExistsConversions(outputPath)) {
		std::string skipMessage = "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m";
        skippedOuts.insert(skipMessage);
        return;  // Skip conversion if the file already exists
    }

    // Execute the conversion using ccd2iso, with shell-escaped paths
    std::string conversionCommand = "ccd2iso " + shell_escape(inputPath) + " " + shell_escape(outputPath);
    int conversionStatus = std::system(conversionCommand.c_str());
	auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);
    // Check the result of the conversion
    if (conversionStatus == 0) {
        std::string successMessage = "\033[1mImage file converted to ISO:\033[0;1m \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m";
        successOuts.insert(successMessage);
    } else {
        std::string FailedMessage = "\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m failed.\033[0;1m";
        failedOuts.insert(FailedMessage);

        // Delete the partially created ISO file
        if (std::remove(outputPath.c_str()) == 0) {
            std::string DeletedMessage = "\033[1;92mDeleted incomplete ISO file:\033[1;91m '" + outDirectory + "/" + outFileNameOnly + "'\033[1;92m.\033[0;1m";
            deletedOuts.insert(DeletedMessage);
        } else {
            std::string DeletedMessage = "\033[1;91mFailed to delete partially created ISO file: \033[1;93m'" + outDirectory + "/" + outFileNameOnly + "'\033[1;91m.\033[0;1m";
            deletedOuts.insert(DeletedMessage);
        }
    }
}


// Check if ccd2iso is installed on the system
bool isCcd2IsoInstalled() {
    // Use the system command to check if ccd2iso is available
    if (std::system("which ccd2iso > /dev/null 2>&1") == 0) {
        return true; // ccd2iso is installed
    } else {
        return false; // ccd2iso is not installed
    }
}


// MDF CONVERSION FUNCTIONS


// Function to convert an MDF file to ISO format using mdf2iso
void convertMDFToISO(const std::string& inputPath) {
	auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m does not exist.\033[0;1m\n";
        return;
    }

    // Check if the corresponding .iso file already exists
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
    if (fileExistsConversions(outputPath)) {
        std::string skipMessage = "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m";
        skippedOuts.insert(skipMessage);
        return;
    }

    // Escape the inputPath before using it in shell commands
    std::string escapedInputPath = shell_escape(inputPath);

    // Define the output path for the ISO file with only the .iso extension
    std::string isooutputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Escape the outputPath before using it in shell commands
    std::string escapedOutputPath = shell_escape(outputPath);

    // Continue with the rest of the conversion logic...

    // Execute the conversion using mdf2iso
    std::string conversionCommand = "mdf2iso " + escapedInputPath + " " + escapedOutputPath;
	auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);
    // Capture the output of the mdf2iso command
    FILE* pipe = popen(conversionCommand.c_str(), "r");
    if (!pipe) {
        std::string FailedMessage = "\033[1;91mFailed to execute conversion command\033[0;1m";
        failedOuts.insert(FailedMessage);
        return;
    }

    char buffer[128];
    std::string conversionOutput;
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        conversionOutput += buffer;
    }

    int conversionStatus = pclose(pipe);

    if (conversionStatus == 0) {
        // Check if the conversion output contains the "already ISO9660" message
        if (conversionOutput.find("already ISO") != std::string::npos) {
            std::string FailedMessage = "\033[1;91mThe selected file \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m is already in ISO format, maybe rename it to .iso?. Skipped conversion.\033[0;1m";
            failedOuts.insert(FailedMessage);
        } else {
            std::string successMessage = "\033[1mImage file converted to ISO: \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m\033[1m.\033[0;1m";
            successOuts.insert(successMessage);
        }
    } else {
        std::string FailedMessage = "\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m failed.\033[0;1m";
        failedOuts.insert(FailedMessage);
	}
}


// Function to check if mdf2iso is installed
bool isMdf2IsoInstalled() {
    // Construct a command to check if mdf2iso is in the system's PATH
    std::string command = "which " + shell_escape("mdf2iso");

    // Execute the command and check the result
    if (std::system((command + " > /dev/null 2>&1").c_str()) == 0) {
        return true;  // mdf2iso is installed
    } else {
        return false;  // mdf2iso is not installed
    }
}
