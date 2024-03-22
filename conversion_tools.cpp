#include "sanitization_extraction_readline.h"
#include "conversion_tools.h"

static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here

std::mutex fileCheckMutex;

// GENERAL


// Function to check if a file already exists
bool fileExistsConversions(const std::string& fullPath) {
    std::lock_guard<std::mutex> lock(fileCheckMutex);
        return std::filesystem::exists(fullPath);
} 


bool endsWith(const std::string& fullString, const std::string& ending) {
    if (fullString.length() >= ending.length()) {
        return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
    } else {
        return false;
    }
}


// Function to convert a string to lowercase
std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}


// BIN/IMG CONVERSION FUNCTIONS



bool blacklistBin(const std::filesystem::path& entry) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();

    // Convert the extension to lowercase for case-insensitive comparison
    std::string extLower = ext;
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](char c) {
        return std::tolower(c);
    });

    // Combine extension check
    if (!(extLower == ".bin" || extLower == ".img")) {
        return false;
    }

    // Check file size
    if (std::filesystem::file_size(entry) <= 5'000'000) {
        return false;
    }

    // Convert the filename to lowercase for additional case-insensitive comparisons
    std::string filenameLowerNoExt = filenameLower;
    filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size()); // Remove extension
    std::transform(filenameLowerNoExt.begin(), filenameLowerNoExt.end(), filenameLowerNoExt.begin(), [](char c) {
        return std::tolower(c);
    });

    // Use a set for blacklisted keywords
    static const std::unordered_set<std::string> blacklistKeywords = {
        "block", "list", "sdcard", "index", "data", "shader", "navmesh",
        "obj", "terrain", "script", "history", "system", "vendor",
        "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
    };

    // Check if any blacklisted word is present in the filename
    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}


// Function to search for .bin and .img files over 5MB
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback) {
    // Vector to store cached invalid paths
    static std::vector<std::string> cachedInvalidPaths;
    
    // Vector to store permission errors
    std::set<std::string> uniqueInvalidPaths;

    // Static variables to cache results for reuse
    static std::vector<std::string> binImgFilesCache;
    
    // Set to store processed paths
    static std::set<std::string> processedPaths;

    // Vector to store file names that match the criteria
    std::vector<std::string> fileNames;

    // Clear the cachedInvalidPaths before processing a new set of paths
    cachedInvalidPaths.clear();

    bool printedEmptyLine = false;  // Flag to track if an empty line has been printed
    
    // Mutex to ensure thread safety
    std::mutex mutex4search;
    
    // Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();

    try {

        // Counter to track the number of ongoing tasks
        unsigned int numOngoingTasks = 0;

        // Use a vector to store futures for ongoing tasks
        std::vector<std::future<void>> futures;

        // Iterate through input paths
        for (const auto& path : paths) {
            // Check if the path has already been processed
            if (processedPaths.find(path) != processedPaths.end()) {
                continue; // Skip already processed paths
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
                    fileNames.push_back(fileName);

                    // Decrement the ongoing tasks counter
                    --numOngoingTasks;
                };

                // Use async to process files concurrently
                // Iterate through files in the given directory and its subdirectories
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    if (entry.is_regular_file()) {
							// Checks .bin .img blacklist
							if (blacklistBin(entry)) {
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
                                        future.get();
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
                processedPaths.insert(path);

            } catch (const std::filesystem::filesystem_error& e) {
                std::lock_guard<std::mutex> lock(mutex4search);

                // Check if the exception is related to a permission error
				const std::error_code& ec = e.code();
				if (ec == std::errc::permission_denied) {
					// Check if the path is unique
					if (uniqueInvalidPaths.insert(path).second) {
						// If it's a new path, print an empty line before printing the error (only once)
						if (!printedEmptyLine) {
							std::cout << " " << std::endl;
							printedEmptyLine = true;
						}
							// Handle permission error differently, you can choose to skip or print a specific message
							std::cerr << "\033[1;91mInsufficient permissions for directory path: \033[1;93m'" << path << "'\033[1;91m.\033[1;0m" << std::endl;
					}
                } else if (std::find(cachedInvalidPaths.begin(), cachedInvalidPaths.end(), path) == cachedInvalidPaths.end()) {
                    if (!printedEmptyLine) {
                        // Print an empty line before starting to print invalid paths (only once)
                        std::cout << " " << std::endl;
                        printedEmptyLine = true;
                    }

                    // Print the specific error details for non-permission errors
                    std::cerr << "\033[1;91m" << e.what() << ".\033[1;0m" << std::endl;

                    // Add the invalid path to cachedInvalidPaths to avoid duplicate error messages
                    cachedInvalidPaths.push_back(path);
                }
            }
        }

    } catch (const std::filesystem::filesystem_error& e) {
        if (!printedEmptyLine) {
            // Print an empty line before starting to print invalid paths (only once)
            std::cout << " " << std::endl;
            printedEmptyLine = true;
        }
        // Handle filesystem errors for the overall operation
        std::cerr << "\033[1;91m" << e.what() << ".\033[1;0m" << std::endl;
        std::cin.ignore();
    }

    // Print success message if files were found
    if (!fileNames.empty()) {
		// Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << " " << std::endl;
        std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[1;0m" << ".\033[1;93m " << binImgFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    // Remove duplicates from fileNames by sorting and using unique erase idiom
    std::sort(fileNames.begin(), fileNames.end());
    fileNames.erase(std::unique(fileNames.begin(), fileNames.end()), fileNames.end());
    
    std::lock_guard<std::mutex> lock(mutex4search);

    // Update the cache by appending fileNames to binImgFilesCache
    binImgFilesCache.insert(binImgFilesCache.end(), fileNames.begin(), fileNames.end());

    // Return the combined results
    return binImgFilesCache;
}


// Main function to select directories and convert BIN/IMG files to ISO format
void select_and_convert_files_to_iso() {
    // Initialize vectors to store BIN/IMG files and directory paths
    std::vector<std::string> binImgFiles;
    std::vector<std::string> directoryPaths;

    // Declare previousPaths as a static variable
    static std::vector<std::string> previousPaths;

    // Read input for directory paths (allow multiple paths separated by semicolons)
    std::string inputPaths = readInputLine("\033[1;94mEnter the directory path(s) (if many, separate them with \033[1m\033[1;93m;\033[1;0m\033[1;94m) to search for \033[1m\033[1;92m.bin \033[1;94mand \033[1m\033[1;92m.img\033[1;94m files, or press Enter to return:\n\033[1;0m");
	
	// Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();
	
    // Use semicolon as a separator to split paths
    std::istringstream iss(inputPaths);
    std::string path;
    while (std::getline(iss, path, ';')) {
        // Trim leading and trailing whitespaces from each path
        size_t start = path.find_first_not_of(" \t");
        size_t end = path.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            directoryPaths.push_back(path.substr(start, end - start + 1));
        }
    }
	
    // Check if directoryPaths is empty
    if (directoryPaths.empty()) {
        return;
    }
    // Flag to check if new files are found
	bool newFilesFound = false;

	// Call the findBinImgFiles function to populate the cache
	binImgFiles = findBinImgFiles(directoryPaths, [&binImgFiles, &newFilesFound](const std::string& fileName, const std::string& filePath) {
    // Your callback logic here, if needed
    newFilesFound = true;
	});

	// Print a message only if no new files are found
	if (!newFilesFound && !binImgFiles.empty()) {
		std::cout << " " << std::endl;
		// Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
		std::cout << "\033[1;91mNo new .bin .img file(s) over 5MB found. \033[1;92m" << binImgFiles.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
		std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
	}

    if (binImgFiles.empty()) {
		std::cout << " " << std::endl;
		// Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\033[1;91mNo .bin or .img file(s) over 5MB found in the specified path(s) or cached in RAM.\n\033[1;0m";
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
        return;
        
    } else {
        while (true) {
            std::system("clear");
            // Print the list of BIN/IMG files
            printFileListBin(binImgFiles);
            
            std::cout << " " << std::endl;
            // Prompt user to choose a file or exit
            char* input = readline("\033[1;94mChoose BIN/IMG file(s) for \033[1;92mconversion\033[1;94m (e.g., '1-3' '1 2', or press Enter to return):\033[1;0m ");
        
            // Break the loop if the user presses Enter
            if (input[0] == '\0') {
                std::system("clear");
                break;
            }

            std::system("clear");
            // Process user input
            processInputBin(input, binImgFiles);
            std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
            std::cin.ignore();
        }
    }
}


// Function to print found BIN/IMG files
void printFileListBin(const std::vector<std::string>& fileList) {
    // ANSI escape codes for text formatting
    const std::string bold = "\033[1m";
    const std::string reset = "\033[0m";
    const std::string greenBold = "\033[1;38;5;208m";

    // Print header for file selection
    std::cout << bold << "Select file(s) to convert to " << bold << "\033[1;92mISO(s)\033[1;0m:\n";
    std::cout << " " << std::endl;

    // Counter for line numbering
    int lineNumber = 1;

    // Apply formatting once before the loop
    std::cout << std::right << std::setw(2);

    for (const auto& filename : fileList) {
        // Extract directory and filename
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(filename);

        const std::size_t dotPos = fileNameOnly.find_last_of('.');
        
        if (dotPos != std::string::npos) {
            std::string extension = fileNameOnly.substr(dotPos);
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            if (extension == ".img" || extension == ".bin") {
                // Print path in white and filename in green and bold
                std::cout << std::setw(2) << std::right  << lineNumber << ". " << bold << directory << bold << "/" << greenBold << fileNameOnly << reset << std::endl;
            } else {
                // Print entire path and filename in white
                std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << std::endl;
            }
        } else {
            // No extension found, print entire path and filename in white
            std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << std::endl;
        }

        // Increment line number
        lineNumber++;
    }
}


// Function to process user input and convert selected BIN files to ISO format
void processInputBin(const std::string& input, const std::vector<std::string>& fileList) {

    // Mutexes to protect the critical sections
    std::mutex indicesMutex;
    std::mutex errorsMutex;

    // Create a string stream to tokenize the input
    std::istringstream iss(input);
    std::string token;

	// Set to track processed indices to avoid duplicates
    std::set<int> processedIndices;
    
    // Set to track processed error messages to avoid duplicate error reporting
    std::set<std::string> processedErrors;

    // Define and populate uniqueValidIndices before this line
    std::set<int> uniqueValidIndices;

    // Vector to store asynchronous tasks for file conversion
    std::vector<std::future<void>> futures;

    // Vector to store error messages
    std::vector<std::string> errorMessages;

    // Protect the critical section with a lock
    std::lock_guard<std::mutex> lock(indicesMutex);
    
    ThreadPool pool(maxThreads);

    // Function to execute asynchronously
    auto asyncConvertBINToISO = [&](const std::string& selectedFile) {
        convertBINToISO(selectedFile);
    };
	// Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();
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
                            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[1;0m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                errorMessages.push_back(errorMessage);
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
                                        // Convert BIN to ISO asynchronously and store the future in the vector
                                        std::string selectedFile = fileList[selectedIndex];
                                        uniqueValidIndices.insert(selectedIndex);
                                        futures.push_back(pool.enqueue(asyncConvertBINToISO, selectedFile));
                                        processedIndices.insert(selectedIndex);
                                    }
                                } else {
                                    // Add an error message for an invalid range
                                    std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m";
                                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                        // Protect the critical section with a lock
                                        std::lock_guard<std::mutex> lock(errorsMutex);
                                        errorMessages.push_back(errorMessage);
                                        processedErrors.insert(errorMessage);
                                    }
                                    break; // Exit the loop to avoid further errors
                                }
                            }
                        } else {
                            // Add an error message for an invalid range
                            std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                errorMessages.push_back(errorMessage);
                                processedErrors.insert(errorMessage);
                            }
                        }
                    } else {
                        // Add an error message for an invalid range format
                        std::string errorMessage = "\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[1;0m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            errorMessages.push_back(errorMessage);
                            processedErrors.insert(errorMessage);
                        }
                    }
                } else {
                    // Add an error message for an invalid character after the dash
                    std::string errorMessage = "\033[1;91mInvalid character after dash in range: '" + token + "'.\033[1;0m";
                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                        // Protect the critical section with a lock
                        std::lock_guard<std::mutex> lock(errorsMutex);
                        errorMessages.push_back(errorMessage);
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
                        uniqueValidIndices.insert(selectedIndex);
                        futures.push_back(pool.enqueue(asyncConvertBINToISO, selectedFile));
                        processedIndices.insert(selectedIndex); 
                    } else {
                        // Add an error message for an invalid file index
                        std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[1;0m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            errorMessages.push_back(errorMessage);
                            processedErrors.insert(errorMessage);
                        }
                    }
                }
            } else {
                // Add an error message for an invalid file index
                std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[1;0m";
                if (processedErrors.find(errorMessage) == processedErrors.end()) {
                    // Protect the critical section with a lock
                    std::lock_guard<std::mutex> lock(errorsMutex);
                    errorMessages.push_back(errorMessage);
                    processedErrors.insert(errorMessage);
                }
            }
        } else {
            // Add an error message for an invalid input
            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[1;0m";
            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                // Protect the critical section with a lock
                std::lock_guard<std::mutex> lock(errorsMutex);
                errorMessages.push_back(errorMessage);
                processedErrors.insert(errorMessage);
            }
        }
    }

    // Wait for all futures to finish
    for (auto& future : futures) {
        future.wait();
    }
	
	if (!errorMessages.empty() && !uniqueValidIndices.empty()) {
		std::cout << " " << std::endl;	
	}
	
    // Print error messages
    for (const auto& errorMessage : errorMessages) {
        std::cout << errorMessage << std::endl;
    }
    std::cout << " " << std::endl;
    
    // Stop the timer after completing the mounting process
    auto end_time = std::chrono::high_resolution_clock::now();
    // Calculate and print the elapsed time
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        
}


// Function to convert a BIN file to ISO format
void convertBINToISO(const std::string& inputPath) {
	auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m does not exist.\033[1;0m" << std::endl;
        return;
    }

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Check if the output ISO file already exists
    if (fileExistsConversions(outputPath)) {
        std::cout << "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" << directory << "/" << fileNameOnly << "'\033[1;93m. Skipped conversion.\033[1;0m" << std::endl;
        return;  // Skip conversion if the file already exists
    }

    // Execute the conversion using ccd2iso, with shell-escaped paths
    std::string conversionCommand = "ccd2iso " + shell_escape(inputPath) + " " + shell_escape(outputPath);
    int conversionStatus = std::system(conversionCommand.c_str());
	auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath);
    // Check the result of the conversion
    if (conversionStatus == 0) {
        std::cout << "\033[1mImage file converted to ISO:\033[1;0m \033[1;92m'" << outDirectory << "/" << outFileNameOnly << "'\033[1;0m\033[1m.\033[1;0m" << std::endl;
    } else {
        std::cout << "\n\033[1;91mConversion of \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m failed.\033[1;0m" << std::endl;

        // Delete the partially created ISO file
        if (std::remove(outputPath.c_str()) == 0) {
            std::cout << "\n\033[1;92mDeleted incomplete ISO file:\033[1;91m '" << outDirectory << "/" << outFileNameOnly << "'\033[1;92m.\033[1;0m" << std::endl;
        } else {
            std::cerr << "\n\033[1;91mFailed to delete partially created ISO file: \033[1;93m'" << outDirectory << "/" << outFileNameOnly << "'\033[1;91m.\033[1;0m" << std::endl;
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


bool blacklistMDF(const std::filesystem::path& entry) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();

    // Convert the extension to lowercase for case-insensitive comparison
    std::string extLower = ext;
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](char c) {
        return std::tolower(c);
    });

    // Combine extension check
    if (!(extLower == ".mdf")) {
        return false;
    }

    // Check file size
    if (std::filesystem::file_size(entry) <= 5'000'000) {
        return false;
    }

    // Convert the filename to lowercase for additional case-insensitive comparisons
    //std::string filenameLowerNoExt = filenameLower;
    //filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size()); // Remove extension
    //std::transform(filenameLowerNoExt.begin(), filenameLowerNoExt.end(), filenameLowerNoExt.begin(), [](char c) {
      //  return std::tolower(c);
    //});

    // Use a set for blacklisted keywords
    //static const std::unordered_set<std::string> blacklistKeywords = {
      //  "block", "list", "sdcard", "index", "data", "shader", "navmesh",
      //  "obj", "flora", "terrain", "script", "history", "system", "vendor",
      //  "cache", "dictionary", "initramfs", "map", "setup", "encrypt"
   // };

    // Check if any blacklisted word is present in the filename
   // for (const auto& keyword : blacklistKeywords) {
      //  if (filenameLowerNoExt.find(keyword) != std::string::npos) {
         //   return false;
       // }
   // }

    return true;
}


// Function to search for .mdf and .mds files over 5MB
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback) {
    // Vector to store cached invalid paths
    static std::vector<std::string> cachedInvalidPaths;
    
    // Vector to store permission errors
    std::set<std::string> uniqueInvalidPaths;

    // Static variables to cache results for reuse
    static std::vector<std::string> mdfMdsFilesCache;

    // Set to store processed paths
    static std::set<std::string> processedPaths;

    // Vector to store file names that match the criteria
    std::vector<std::string> fileNames;

    // Clear the cachedInvalidPaths before processing a new set of paths
    cachedInvalidPaths.clear();

    bool printedEmptyLine = false;  // Flag to track if an empty line has been printed
    
    // Mutex to ensure thread safety
    std::mutex mutex4search;
    
    // Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();

    try {

        // Counter to track the number of ongoing tasks
        unsigned int numOngoingTasks = 0;

        // Use a vector to store futures for ongoing tasks
        std::vector<std::future<void>> futures;

        // Iterate through input paths
        for (const auto& path : paths) {
            // Check if the path has already been processed
            if (processedPaths.find(path) != processedPaths.end()) {
                continue; // Skip already processed paths
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
                    fileNames.push_back(fileName);

                    // Decrement the ongoing tasks counter
                    --numOngoingTasks;
                };

                // Use a vector to store futures for ongoing tasks
                std::vector<std::future<void>> futures;

                // Iterate through files in the given directory and its subdirectories
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    if (entry.is_regular_file()) { 
                        if (blacklistMDF(entry)) {
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
                                        future.get();
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
                processedPaths.insert(path);

            } catch (const std::filesystem::filesystem_error& e) {
                std::lock_guard<std::mutex> lock(mutex4search);

                // Check if the exception is related to a permission error
				const std::error_code& ec = e.code();
				if (ec == std::errc::permission_denied) {
				// Check if the path is unique
					if (uniqueInvalidPaths.insert(path).second) {
					// If it's a new path, print an empty line before printing the error (only once)
						if (!printedEmptyLine) {
							std::cout << " " << std::endl;
							printedEmptyLine = true;
						}
							// Handle permission error differently, you can choose to skip or print a specific message
					}		std::cerr << "\033[1;91mInsufficient permissions for directory path: \033[1;93m'" << path << "'\033[1;91m.\033[1;0m" << std::endl;
                } else if (std::find(cachedInvalidPaths.begin(), cachedInvalidPaths.end(), path) == cachedInvalidPaths.end()) {
                    if (!printedEmptyLine) {
                        // Print an empty line before starting to print invalid paths (only once)
                        std::cout << " " << std::endl;
                        printedEmptyLine = true;
                    }

                    // Print the specific error details for non-permission errors
                    std::cerr << "\033[1;91m" << e.what() << ".\033[1;0m" << std::endl;

                    // Add the invalid path to cachedInvalidPaths to avoid duplicate error messages
                    cachedInvalidPaths.push_back(path);
                }
            }
        }

    } catch (const std::filesystem::filesystem_error& e) {
        if (!printedEmptyLine) {
            // Print an empty line before starting to print invalid paths (only once)
            std::cout << " " << std::endl;
            printedEmptyLine = true;
        }
        // Handle filesystem errors for the overall operation
        std::cerr << "\033[1;91m" << e.what() << "\033[1;0m" << std::endl;
        std::cin.ignore();
    }

    // Print success message if files were found
    if (!fileNames.empty()) {
		// Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << " " << std::endl;
        std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[1;0m" << ".\033[1;93m " << mdfMdsFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    // Remove duplicates from fileNames by sorting and using unique erase idiom
    std::sort(fileNames.begin(), fileNames.end());
    fileNames.erase(std::unique(fileNames.begin(), fileNames.end()), fileNames.end());
    
    std::lock_guard<std::mutex> lock(mutex4search);

    // Update the cache by appending fileNames to mdfMdsFilesCache
    mdfMdsFilesCache.insert(mdfMdsFilesCache.end(), fileNames.begin(), fileNames.end());
    

    // Return the combined results
    return mdfMdsFilesCache;
}


// Function to interactively select and convert MDF files to ISO
void select_and_convert_files_to_iso_mdf() {
	
	// Initialize vectors to store MDF/MDS files and directory paths
    std::vector<std::string> mdfMdsFiles;
    std::vector<std::string> directoryPaths;
    
    // Declare previousPaths as a static variable
    static std::vector<std::string> previousPaths;

	
    // Read input for directory paths (allow multiple paths separated by semicolons)
    std::string inputPaths = readInputLine("\033[1;94mEnter the directory path(s) (if many, separate them with \033[1m\033[1;93m;\033[1;0m\033[1;94m) to search for \033[1m\033[1;92m.mdf\033[1;94m files, or press Enter to return:\n\033[1;0m");
    
    // Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Use semicolon as a separator to split paths
    std::istringstream iss(inputPaths);
    std::string path;
    while (std::getline(iss, path, ';')) {
        // Trim leading and trailing whitespaces from each path
        size_t start = path.find_first_not_of(" \t");
        size_t end = path.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            directoryPaths.push_back(path.substr(start, end - start + 1));
        }
    }

    // Check if directoryPaths is empty
    if (directoryPaths.empty()) {
        return;
    }

    // Flag to check if new .mdf files are found
    bool newMdfFilesFound = false;

    // Call the findMdsMdfFiles function to populate the cache
    mdfMdsFiles = findMdsMdfFiles(directoryPaths, [&mdfMdsFiles, &newMdfFilesFound](const std::string& fileName, const std::string& filePath) {
        newMdfFilesFound = true;
    });

    // Print a message only if no new .mdf files are found
    if (!newMdfFilesFound && !mdfMdsFiles.empty()) {
        std::cout << " " << std::endl;
        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\033[1;91mNo new .mdf file(s) over 5MB found. \033[1;92m" << mdfMdsFiles.size() << " file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    if (mdfMdsFiles.empty()) {
        std::cout << " " << std::endl;
        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();
        std::cout << "\033[1;91mNo .mdf file(s) over 5MB found in the specified path(s) or cached in RAM.\n\033[1;0m";
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
        return;
    }

    // Continue selecting and converting files until the user decides to exit
    while (true) {
        std::system("clear");
        printFileListMdf(mdfMdsFiles);

        std::cout << " " << std::endl;
        // Prompt the user to enter file numbers or 'exit'
        char* input = readline("\033[1;94mChoose MDF file(s) for \033[1;92mconversion\033[1;94m (e.g., '1-2' or '1 2', or press Enter to return):\033[1;0m ");

        if (input[0] == '\0') {
            std::system("clear");
            break;
        }

        std::system("clear");
            // Process user input
            processInputMDF(input, mdfMdsFiles);
            std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
            std::cin.ignore();
	}
}

// Function to print found MDF files
void printFileListMdf(const std::vector<std::string>& fileList) {
    // ANSI escape codes for text formatting
    const std::string bold = "\033[1m";
    const std::string reset = "\033[0m";
    const std::string orangeBold = "\033[1;38;5;208m";

    // Print header for file selection
    std::cout << bold << "Select file(s) to convert to " << bold << "\033[1;92mISO(s)\033[1;0m:\n";
    std::cout << " " << std::endl;

    // Counter for line numbering
    int lineNumber = 1;

    // Apply formatting once before the loop
    std::cout << std::right << std::setw(2);

    for (std::size_t i = 0; i < fileList.size(); ++i) {
        const std::string& filename = fileList[i];

        // Extract directory and filename
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(filename);

        const std::size_t dotPos = fileNameOnly.find_last_of('.');

        // Check if the file has a ".mdf" extension (case-insensitive)
        if (dotPos != std::string::npos) {
            std::string extension = fileNameOnly.substr(dotPos);
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            if (extension == ".mdf") {
                // Print path in white and filename in orange and bold
                std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << directory << bold << "/" << orangeBold << fileNameOnly << reset << std::endl;
            } else {
                // Print entire path and filename in white
                std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << std::endl;
            }
        } else {
            // No extension found, print entire path and filename in white
            std::cout << std::setw(2) << std::right << lineNumber << ". " << bold << filename << reset << std::endl;
        }

        // Increment line number
        lineNumber++;
    }
}


// Function to process user input and convert selected MDF files to ISO format
void processInputMDF(const std::string& input, const std::vector<std::string>& fileList) {

    // Mutexes to protect the critical sections
    std::mutex indicesMutex;
    std::mutex errorsMutex;
    
    // Create a string stream to tokenize the input
    std::istringstream iss(input);
    std::string token;

    // Set to track processed indices to avoid duplicates
    std::set<int> processedIndices;

    // Set to track processed error messages to avoid duplicate error reporting
    std::set<std::string> processedErrors;

    // Define and populate uniqueValidIndices before this line
    std::set<int> uniqueValidIndices;

    // Vector to store asynchronous tasks for file conversion
    std::vector<std::future<void>> futures;

    // Vector to store error messages
    std::vector<std::string> errorMessages;

    // Protect the critical section with a lock
    std::lock_guard<std::mutex> lock(indicesMutex);
    
    ThreadPool pool(maxThreads);

    // Function to execute asynchronously
    auto asyncConvertMDFToISO = [&](const std::string& selectedFile) {
        convertMDFToISO(selectedFile);
    };
	// Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();
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
                            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[1;0m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                errorMessages.push_back(errorMessage);
                                processedErrors.insert(errorMessage);
                            }
                        } else if (static_cast<std::vector<std::string>::size_type>(start) > 0 &&
								   static_cast<std::vector<std::string>::size_type>(end) > 0 &&
								   static_cast<std::vector<std::string>::size_type>(start) <= fileList.size() &&
								   static_cast<std::vector<std::string>::size_type>(end) <= fileList.size()) {  // Check if the range is valid
                            // Handle a valid range
                            int step = (start <= end) ? 1 : -1; // Determine the step based on the range direction
                            int selectedIndex;  // Declare selectedIndex outside of the loop

                            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                                selectedIndex = i - 1;
                                if (selectedIndex >= 0 && static_cast<std::vector<std::string>::size_type>(selectedIndex) < fileList.size()) {
                                    if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                                        // Convert MDF to ISO asynchronously and store the future in the vector
                                        std::string selectedFile = fileList[selectedIndex];
                                        uniqueValidIndices.insert(selectedIndex);
                                        futures.push_back(pool.enqueue(asyncConvertMDFToISO, selectedFile));
                                        processedIndices.insert(selectedIndex);         
                                    }
                                } else {
                                    // Add an error message for an invalid range
                                    std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m";
                                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                        // Protect the critical section with a lock
                                        std::lock_guard<std::mutex> lock(errorsMutex);
                                        errorMessages.push_back(errorMessage);
                                        processedErrors.insert(errorMessage);
                                    }
                                    break; // Exit the loop to avoid further errors
                                }
                            }
                        } else {
                            // Add an error message for an invalid range
                            std::string errorMessage = "\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m";
                            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                                // Protect the critical section with a lock
                                std::lock_guard<std::mutex> lock(errorsMutex);
                                errorMessages.push_back(errorMessage);
                                processedErrors.insert(errorMessage);
                            }
                        }
                    } else {
                        // Add an error message for an invalid range format
                        std::string errorMessage = "\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[1;0m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            errorMessages.push_back(errorMessage);
                            processedErrors.insert(errorMessage);
                        }
                    }
                } else {
                    // Add an error message for an invalid character after the dash
                    std::string errorMessage = "\033[1;91mInvalid character after dash in range: '" + token + "'.\033[1;0m";
                    if (processedErrors.find(errorMessage) == processedErrors.end()) {
                        // Protect the critical section with a lock
                        std::lock_guard<std::mutex> lock(errorsMutex);
                        errorMessages.push_back(errorMessage);
                        processedErrors.insert(errorMessage);
                    }
                }
            } else if (static_cast<std::vector<std::string>::size_type>(start) >= 1 && static_cast<std::vector<std::string>::size_type>(start) <= fileList.size()) {
                // Process a single index
                int selectedIndex = start - 1;
                if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                    if (selectedIndex >= 0 && static_cast<std::vector<std::string>::size_type>(selectedIndex) < fileList.size()) {
                        // Convert MDF to ISO asynchronously and store the future in the vector
                        std::string selectedFile = fileList[selectedIndex];
                        uniqueValidIndices.insert(selectedIndex);  
                        futures.push_back(pool.enqueue(asyncConvertMDFToISO, selectedFile));
                        processedIndices.insert(selectedIndex);
                    } else {
                        // Add an error message for an invalid file index
                        std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[1;0m";
                        if (processedErrors.find(errorMessage) == processedErrors.end()) {
                            // Protect the critical section with a lock
                            std::lock_guard<std::mutex> lock(errorsMutex);
                            errorMessages.push_back(errorMessage);
                            processedErrors.insert(errorMessage);
                        }
                    }
                }
            } else {
                // Add an error message for an invalid file index
                std::string errorMessage = "\033[1;91mFile index '" + std::to_string(start) + "' does not exist.\033[1;0m";
                if (processedErrors.find(errorMessage) == processedErrors.end()) {
                    // Protect the critical section with a lock
                    std::lock_guard<std::mutex> lock(errorsMutex);
                    errorMessages.push_back(errorMessage);
                    processedErrors.insert(errorMessage);
                }
            }
        } else {
            // Add an error message for an invalid input
            std::string errorMessage = "\033[1;91mInvalid input: '" + token + "'.\033[1;0m";
            if (processedErrors.find(errorMessage) == processedErrors.end()) {
                // Protect the critical section with a lock
                std::lock_guard<std::mutex> lock(errorsMutex);
                errorMessages.push_back(errorMessage);
                processedErrors.insert(errorMessage);
            }
        }
    }

    // Wait for all futures to finish
    for (auto& future : futures) {
        future.wait();
    }
	
	if (!errorMessages.empty() && !uniqueValidIndices.empty()) {
		std::cout << " " << std::endl;	
	}
	
    // Print error messages
    for (const auto& errorMessage : errorMessages) {
        std::cout << errorMessage << std::endl;
    }
    std::cout << " " << std::endl;
    
    // Stop the timer after completing the mounting process
    auto end_time = std::chrono::high_resolution_clock::now();
    // Calculate and print the elapsed time
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
}


// Function to convert an MDF file to ISO format using mdf2iso
void convertMDFToISO(const std::string& inputPath) {
	auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath);
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m does not exist.\033[1;0m" << std::endl;
        return;
    }

    // Check if the corresponding .iso file already exists
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
    if (fileExistsConversions(outputPath)) {
        std::cout << "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" << directory << "/" << fileNameOnly << "'\033[1;93m. Skipped conversion.\033[1;0m" << std::endl;
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
        std::cout << "\033[1;91mFailed to execute conversion command\033[1;0m" << std::endl;
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
            std::cout << "\033[1;91mThe selected file \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m is already in ISO format, maybe rename it to .iso?. Skipped conversion.\033[1;0m" << std::endl;
        } else {
            std::cout << "\033[1mImage file converted to ISO: \033[1;92m'" << outDirectory << "/" << outFileNameOnly << "'\033[1;0m\033[1m.\033[1;0m" << std::endl;
        }
    } else {
        std::cout << "\n\033[1;91mConversion of \033[1;93m'" << directory << "/" << fileNameOnly << "'\033[1;91m failed.\033[1;0m" << std::endl;
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
