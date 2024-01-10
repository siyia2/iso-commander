#include "sanitization_readline.h"
#include "conversion_tools.h"


static std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
static std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here

std::mutex fileCheckMutex;

// GENERAL \\

bool fileExistsConversions(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(fileCheckMutex);
    return std::filesystem::exists(filePath);
} 



// BIN/IMG CONVERSION FUNCTIONS	\\


// Function to search for .bin and .img files under 10MB
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback) {
    // Vector to store cached invalid paths
    static std::vector<std::string> cachedInvalidPaths;

    // Static variables to cache results for reuse
    static std::vector<std::string> binImgFilesCache;

    // Vector to store file names that match the criteria
    std::vector<std::string> fileNames;

    // Clear the cachedInvalidPaths before processing a new set of paths
    cachedInvalidPaths.clear();

    bool printedEmptyLine = false;  // Flag to track if an empty line has been printed

    try {
        // Mutex to ensure thread safety
        static std::mutex mutex4search;

        // Determine the maximum number of threads to use based on hardware concurrency; fallback is 2 threads
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

        // Use a thread pool for parallel processing
        std::vector<std::future<void>> futures;

        // Iterate through input paths
        for (const auto& path : paths) {
            try {
                // Use a lambda function to process files asynchronously
                auto processFileAsync = [&](const std::filesystem::directory_entry& entry) {
                    std::string fileName = entry.path().string();
                    std::string filePath = entry.path().parent_path().string();  // Get the path of the directory

                    // Call the callback function to inform about the found file
                    callback(fileName, filePath);

                    // Lock the mutex to ensure safe access to shared data (fileNames)
                    std::lock_guard<std::mutex> lock(mutex4search);
                    fileNames.push_back(fileName);
                };

                // Use async to process files concurrently
                futures.clear();

                // Iterate through files in the given directory and its subdirectories
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    if (entry.is_regular_file()) {
                        // Check if the file has a ".bin" or ".img" extension and is larger than or equal to 10,000,000 bytes
                        std::string ext = entry.path().extension();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                            return std::tolower(c);
                        });

                        if ((ext == ".bin" || ext == ".img") && std::filesystem::file_size(entry) >= 10'000'000) {
                            // Check if the file is already present in the cache to avoid duplicates
                            std::string fileName = entry.path().string();
                            if (std::find(binImgFilesCache.begin(), binImgFilesCache.end(), fileName) == binImgFilesCache.end()) {
                                // Process the file asynchronously
                                futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                            }
                        }
                    }
                }

                // Wait for all asynchronous tasks to complete
                for (auto& future : futures) {
                    future.get();
                }

            } catch (const std::filesystem::filesystem_error& e) {
                // Handle filesystem errors for the current directory
                if (!printedEmptyLine) {
                    // Print an empty line before starting to print invalid paths (only once)
                    std::cout << " " << std::endl;
                    printedEmptyLine = true;
                }
                if (std::find(cachedInvalidPaths.begin(), cachedInvalidPaths.end(), path) == cachedInvalidPaths.end()) {
                    std::cerr << "\033[1;91mInvalid directory path: '" << path << "'. Excluded from search." << "\033[1;0m" << std::endl;
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
        std::cout << " " << std::endl;
        std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[1;0m" << ".\033[1;93m " << binImgFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    // Remove duplicates from fileNames by sorting and using unique erase idiom
    std::sort(fileNames.begin(), fileNames.end());
    fileNames.erase(std::unique(fileNames.begin(), fileNames.end()), fileNames.end());

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
		std::cout << "\033[1;91mNo new .bin .img file(s) over 10MB found. \033[1;92m" << binImgFiles.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
		std::cout << " " << std::endl;
		std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
		std::cin.ignore();
	}

    if (binImgFiles.empty()) {
		std::cout << " " << std::endl;
        std::cout << "\033[1;91mNo .bin or .img file(s) over 10MB found in the specified path(s) or cached in RAM.\n\033[1;0m";
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
        
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


void printFileListBin(const std::vector<std::string>& fileList) {
    std::cout << "\033[1mSelect file(s) to convert to \033[1m\033[1;92mISO(s)\033[1;0m:\n";
    std::cout << " " << std::endl;

    for (std::size_t i = 0; i < fileList.size(); ++i) {
        const std::string& filename = fileList[i];
        const std::size_t lastSlashPos = filename.find_last_of('/');
        const std::string path = (lastSlashPos != std::string::npos) ? filename.substr(0, lastSlashPos + 1) : "";
        const std::string fileNameOnly = (lastSlashPos != std::string::npos) ? filename.substr(lastSlashPos + 1) : filename;

        const std::size_t dotPos = fileNameOnly.find_last_of('.');

        // Check if the file has a ".img" or ".bin" extension
        if (dotPos != std::string::npos && (fileNameOnly.compare(dotPos, std::string::npos, ".img") == 0 || fileNameOnly.compare(dotPos, std::string::npos, ".bin") == 0)) {
            // Print path in white and filename in green and bold
            std::cout << std::setw(2) << std::right << i + 1 << ". \033[1m" << path << "\033[1m\033[38;5;208m" << fileNameOnly << "\033[1;0m" << std::endl;
        } else {
            // Print entire path and filename in white
            std::cout << std::setw(2) << std::right << i + 1 << ". \033[1m" << filename << std::endl;
        }
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

    unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    // Vector to store asynchronous tasks for file conversion
    std::vector<std::future<void>> futures;

    // Vector to store error messages
    std::vector<std::string> errorMessages;

    // Protect the critical section with a lock
    std::lock_guard<std::mutex> lock(indicesMutex);

    // Function to execute asynchronously
    auto asyncConvertBINToISO = [&](const std::string& selectedFile, unsigned int maxThreads) {
        convertBINToISO(selectedFile);
    };

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
                        } else if (start > 0 && end > 0 && start <= fileList.size() && end <= fileList.size()) {  // Check if the range is valid
                            // Handle a valid range
                            int step = (start <= end) ? 1 : -1; // Determine the step based on the range direction
                            int selectedIndex;  // Declare selectedIndex outside of the loop

                            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                                selectedIndex = i - 1;
                                if (selectedIndex >= 0 && selectedIndex < fileList.size()) {
                                    if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                                        // Convert BIN to ISO asynchronously and store the future in the vector
                                        std::string selectedFile = fileList[selectedIndex];
                                        futures.push_back(std::async(std::launch::async, asyncConvertBINToISO, selectedFile, maxThreads));
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
            } else if (start >= 1 && start <= fileList.size()) {
                // Process a single index
                int selectedIndex = start - 1;
                if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                    if (selectedIndex >= 0 && selectedIndex < fileList.size()) {
                        // Convert BIN to ISO asynchronously and store the future in the vector
                        std::string selectedFile = fileList[selectedIndex];
                        futures.push_back(std::async(std::launch::async, asyncConvertBINToISO, selectedFile, maxThreads));
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

    // Print error messages
    for (const auto& errorMessage : errorMessages) {
        std::cout << errorMessage << std::endl;
    }
    std::cout << " " << std::endl;
}


// Function to convert a BIN file to ISO format
void convertBINToISO(const std::string& inputPath) {
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << inputPath << "'\033[1;91m does not exist.\033[1;0m" << std::endl;
        return;
    }

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Check if the output ISO file already exists
    if (fileExistsConversions(outputPath)) {
        std::cout << "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" << inputPath << "'\033[1;93m. Skipped conversion.\033[1;0m" << std::endl;
        return;  // Skip conversion if the file already exists
    }

    // Execute the conversion using ccd2iso, with shell-escaped paths
    std::string conversionCommand = "ccd2iso " + shell_escape(inputPath) + " " + shell_escape(outputPath);
    int conversionStatus = std::system(conversionCommand.c_str());

    // Check the result of the conversion
    if (conversionStatus == 0) {
        std::cout << "\033[1mImage file converted to ISO:\033[1;0m \033[1;92m'" << outputPath << "'\033[1;0m\033[1m.\033[1;0m" << std::endl;
    } else {
        std::cout << "\033[1;91mConversion of \033[1;93m'" << inputPath << "'\033[1;91m failed.\033[1;0m" << std::endl;

        // Delete the partially created ISO file
        if (std::remove(outputPath.c_str()) == 0) {
            std::cout << "\033[1;91mDeleted partially created ISO file:\033[1;93m '" << outputPath << "'\033[1;91m failed.\033[1;0m" << std::endl;
        } else {
            std::cerr << "\033[1;91mFailed to delete partially created ISO file: \033[1;93m'" << outputPath << "'\033[1;91m.\033[1;0m" << std::endl;
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


// MDF CONVERSION FUNCTIONS	\\


// Function to search for .mdf and .mds files under 10MB
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback) {
    // Vector to store cached invalid paths
    static std::vector<std::string> cachedInvalidPaths;

    // Static variables to cache results for reuse
    static std::vector<std::string> mdfMdsFilesCache;

    // Vector to store file names that match the criteria
    std::vector<std::string> fileNames;

    // Clear the cachedInvalidPaths before processing a new set of paths
    cachedInvalidPaths.clear();

    bool printedEmptyLine = false;  // Flag to track if an empty line has been printed

    try {
        // Mutex to ensure thread safety
        static std::mutex mutex4search;

        // Determine the maximum number of threads to use based on hardware concurrency; fallback is 2 threads
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

        // Use a thread pool for parallel processing
        std::vector<std::future<void>> futures;

        // Iterate through input paths
        for (const auto& path : paths) {
            try {
                // Use a lambda function to process files asynchronously
                auto processFileAsync = [&](const std::filesystem::directory_entry& entry) {
                    std::string fileName = entry.path().string();
                    std::string filePath = entry.path().parent_path().string();  // Get the path of the directory

                    // Call the callback function to inform about the found file
                    callback(fileName, filePath);

                    // Lock the mutex to ensure safe access to shared data (fileNames)
                    std::lock_guard<std::mutex> lock(mutex4search);
                    fileNames.push_back(fileName);
                };

                // Use thread pool to process files concurrently
                futures.clear();

                // Iterate through files in the given directory and its subdirectories
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    if (entry.is_regular_file()) {
                        // Check if the file has a ".mdf" or ".mds" extension and is larger than or equal to 10,000,000 bytes
                        std::string ext = entry.path().extension();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                            return std::tolower(c);
                        });

                        if ((ext == ".mdf" || ext == ".mds") && std::filesystem::file_size(entry) >= 10'000'000) {
                            // Check if the file is already present in the cache to avoid duplicates
                            std::string fileName = entry.path().string();
                            if (std::find(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), fileName) == mdfMdsFilesCache.end()) {
                                // Process the file asynchronously using the thread pool
                                futures.emplace_back(std::async(std::launch::async, processFileAsync, entry));
                            }
                        }
                    }
                }

                // Wait for all asynchronous tasks to complete
                for (auto& future : futures) {
                    future.get();
                }

            } catch (const std::filesystem::filesystem_error& e) {
                if (!printedEmptyLine) {
                    // Print an empty line before starting to print invalid paths (only once)
                    std::cout << " " << std::endl;
                    printedEmptyLine = true;
                }
                // Handle filesystem errors for the current directory
                if (std::find(cachedInvalidPaths.begin(), cachedInvalidPaths.end(), path) == cachedInvalidPaths.end()) {
                    std::cerr << "\033[1;91mInvalid directory path: '" << path << "'. Excluded from search." << "\033[1;0m" << std::endl;
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
        std::cout << " " << std::endl;
        std::cout << "\033[1;92mFound " << fileNames.size() << " matching file(s)\033[1;0m" << ".\033[1;93m " << mdfMdsFilesCache.size() << " matching file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    // Remove duplicates from fileNames by sorting and using unique erase idiom
    std::sort(fileNames.begin(), fileNames.end());
    fileNames.erase(std::unique(fileNames.begin(), fileNames.end()), fileNames.end());

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
        std::cout << "\033[1;91mNo new .mdf file(s) over 10MB found. \033[1;92m" << mdfMdsFiles.size() << " file(s) cached in RAM from previous searches.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.ignore();
    }

    if (mdfMdsFiles.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mNo .mdf file(s) over 10MB found in the specified path(s) or cached in RAM.\n\033[1;0m";
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


void printFileListMdf(const std::vector<std::string>& fileList) {
    std::cout << "\033[1mSelect file(s) to convert to \033[1m\033[1;92mISO(s)\033[1;0m:\n";
    std::cout << " " << std::endl;

    for (std::size_t i = 0; i < fileList.size(); ++i) {
        const std::string& filename = fileList[i];
        const std::size_t lastSlashPos = filename.find_last_of('/');
        const std::string path = (lastSlashPos != std::string::npos) ? filename.substr(0, lastSlashPos + 1) : "";
        const std::string fileNameOnly = (lastSlashPos != std::string::npos) ? filename.substr(lastSlashPos + 1) : filename;

        const std::size_t dotPos = fileNameOnly.find_last_of('.');

        // Check if the file has a ".mdf" extension
        if (dotPos != std::string::npos && fileNameOnly.compare(dotPos, std::string::npos, ".mdf") == 0) {
            // Print path in white and filename in orange and bold
            std::cout << std::setw(2) << std::right << i + 1 << ". \033[1m" << path << "\033[1m\033[38;5;208m" << fileNameOnly << "\033[1;0m" << std::endl;
        } else {
            // Print entire path and filename in white
            std::cout << std::setw(2) << std::right << i + 1 << ". \033[1m" << filename << std::endl;
        }
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

    unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    // Vector to store asynchronous tasks for file conversion
    std::vector<std::future<void>> futures;

    // Vector to store error messages
    std::vector<std::string> errorMessages;

    // Protect the critical section with a lock
    std::lock_guard<std::mutex> lock(indicesMutex);

    // Function to execute asynchronously
    auto asyncConvertMDFToISO = [&](const std::string& selectedFile, unsigned int maxThreads) {
        convertMDFToISO(selectedFile);
    };

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
                        } else if (start > 0 && end > 0 && start <= fileList.size() && end <= fileList.size()) {  // Check if the range is valid
                            // Handle a valid range
                            int step = (start <= end) ? 1 : -1; // Determine the step based on the range direction
                            int selectedIndex;  // Declare selectedIndex outside of the loop

                            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                                selectedIndex = i - 1;
                                if (selectedIndex >= 0 && selectedIndex < fileList.size()) {
                                    if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                                        // Convert MDF to ISO asynchronously and store the future in the vector
                                        std::string selectedFile = fileList[selectedIndex];
                                        futures.push_back(std::async(std::launch::async, asyncConvertMDFToISO, selectedFile, maxThreads));
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
            } else if (start >= 1 && start <= fileList.size()) {
                // Process a single index
                int selectedIndex = start - 1;
                if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                    if (selectedIndex >= 0 && selectedIndex < fileList.size()) {
                        // Convert MDF to ISO asynchronously and store the future in the vector
                        std::string selectedFile = fileList[selectedIndex];
                        futures.push_back(std::async(std::launch::async, asyncConvertMDFToISO, selectedFile, maxThreads));
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

    // Print error messages
    for (const auto& errorMessage : errorMessages) {
        std::cout << errorMessage << std::endl;
    }
    std::cout << " " << std::endl;
}


// Function to convert an MDF file to ISO format using mdf2iso
void convertMDFToISO(const std::string& inputPath) {
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[1;91mThe specified input file \033[1;93m'" << inputPath << "'\033[1;91m does not exist.\033[1;0m" << std::endl;
        return;
    }

    // Check if the corresponding .iso file already exists
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
    if (fileExistsConversions(outputPath)) {
        std::cout << "\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" << inputPath << "'\033[1;93m. Skipped conversion.\033[1;0m" << std::endl;
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
            std::cout << "\033[1;91mThe selected file \033[1;93m'" << inputPath << "'\033[1;91m is already in ISO format, maybe rename it to .iso?. Skipped conversion.\033[1;0m" << std::endl;
        } else {
            std::cout << "\033[1mImage file converted to ISO: \033[1;92m'" << outputPath << "'\033[1;0m\033[1m.\033[1;0m" << std::endl;
        }
    } else {
        std::cout << "\033[1;91mConversion of \033[1;93m'" << inputPath << "'\033[1;91m failed.\033[1;0m" << std::endl;
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
