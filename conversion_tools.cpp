#include "sanitization_readline.h"
#include "conversion_tools.h"


std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here

std::mutex mdfFilesMutex; // Mutex to protect access to mdfImgFiles
std::mutex binImgFilesMutex; // Mutex to protect access to binImgFiles


// BIN/IMG CONVERSION FUNCTIONS	\\

// Function to list available files and prompt the user to choose a file for conversion
std::string chooseFileToConvert(const std::vector<std::string>& files) {
    // Display a header indicating the available .bin and .img files
    std::cout << "\033[32mFound the following .bin and .img files:\033[0m\n";

    // Iterate through the files and display them with their corresponding numbers
    for (size_t i = 0; i < files.size(); ++i) {
        std::cout << i + 1 << ": " << files[i] << "\n";
    }

    // Prompt the user to enter the number of the file they want to convert
    int choice;
    std::cout << "\033[94mEnter the number of the file you want to convert:\033[0m ";
    std::cin >> choice;

    // Check if the user's choice is within the valid range
    if (choice >= 1 && choice <= static_cast<int>(files.size())) {
        // Return the chosen file path
        return files[choice - 1];
    } else {
        // Print an error message for an invalid choice
        std::cout << "\033[31mInvalid choice. Please choose a valid file.\033[31m\n";
        return "";
    }
}


// Function to find binary image files in directories
std::vector<std::string> findBinImgFiles(std::vector<std::string>& directories,
                                        std::vector<std::string>& previousPaths,
                                        const std::function<void(const std::string&, const std::string&)>& callback) {
    // Use a set for efficient lookup and avoid duplicates
    std::set<std::string> combinedCache(binImgFilesCache.begin(), binImgFilesCache.end());
    std::set<std::string> processedFileNames;
    std::vector<std::string> fileNames;

    try {
        std::vector<std::future<void>> futures;
        std::mutex mutex;

        // Determine the maximum number of threads to use fallback is 2 threads
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

        // Iterate through each directory
        for (const auto& directory : directories) {
            // Skip directories that were processed in a previous call
            if (std::find(previousPaths.begin(), previousPaths.end(), directory) != previousPaths.end()) {
                continue;
            }

            // Iterate through each entry in the directory (recursively)
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                // Check if the entry is a regular file
                if (entry.is_regular_file()) {
                    // Get the file extension and convert it to lowercase
                    std::string ext = entry.path().extension();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                        return std::tolower(c);
                    });

                    // Check if the file has a valid extension and other conditions
                    if ((ext == ".bin" || ext == ".img") &&
                        (entry.path().filename().string().find("data") == std::string::npos) &&
                        (entry.path().filename().string() != "terrain.bin") &&
                        (entry.path().filename().string() != "blocklist.bin")) {

                        // Check if the file size is greater than or equal to 10,000,000 bytes
                        if (std::filesystem::file_size(entry) >= 10'000'000) {
                            // Automatically determine the batch size based on the number of available threads
                            const int batchSize = maxThreads * 2;

                            // Ensure the number of concurrent tasks does not exceed the maximum allowed
                            while (futures.size() >= maxThreads) {
                                auto it = std::find_if(futures.begin(), futures.end(),
                                    [](const std::future<void>& f) {
                                        return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                    });
                                if (it != futures.end()) {
                                    it->get();
                                    futures.erase(it);
                                }
                            }

                            // Launch an asynchronous task for processing the file
                            futures.push_back(std::async(std::launch::async, [entry, &fileNames, &mutex, &callback, &processedFileNames, &combinedCache] {
                                std::string fileName = entry.path().string();
                                std::string filePath = entry.path().parent_path().string();

                                // Lock to ensure thread safety when modifying shared data structures
                                std::lock_guard<std::mutex> lock(mutex);

                                // Check if the file has not been processed before
                                if (processedFileNames.find(fileName) == processedFileNames.end() &&
                                    combinedCache.find(fileName) == combinedCache.end()) {
                                    // Add the file to the result vector, invoke the callback, and update the processed set
                                    fileNames.push_back(fileName);
                                    callback(fileName, filePath);
                                    processedFileNames.insert(fileName);
                                }
                            }));

                            // Check if the batch size is reached and wait for completion if necessary
                            if (futures.size() >= batchSize) {
                                for (auto& future : futures) {
                                    future.get();
                                }
                                futures.clear();
                            }
                        }
                    }
                }
            }
        }

        // Wait for any remaining asynchronous tasks to complete
        for (auto& future : futures) {
            future.get();
        }

        // Update the list of processed directories
        previousPaths.insert(previousPaths.end(), directories.begin(), directories.end());

        // Update the global cache with the processed file names
        combinedCache.insert(fileNames.begin(), fileNames.end());
        binImgFilesCache.assign(combinedCache.begin(), combinedCache.end());

    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    // Return the updated global cache
    return binImgFilesCache;
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


// Function to convert a BIN file to ISO format
void convertBINToISO(const std::string& inputPath) {
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[31mThe specified input file '" << inputPath << "' does not exist.\033[0m" << std::endl;
        return;
    }

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Check if the output ISO file already exists
    if (std::ifstream(outputPath)) {
        std::cout << "\033[33mThe output ISO file '" << outputPath << "' already exists. Skipping conversion.\033[0m" << std::endl;
        return;  // Skip conversion if the file already exists
    }

    // Execute the conversion using ccd2iso, with shell-escaped paths
    std::string conversionCommand = "ccd2iso " + shell_escape(inputPath) + " " + shell_escape(outputPath);
    int conversionStatus = std::system(conversionCommand.c_str());

    // Check the result of the conversion
    if (conversionStatus == 0) {
        std::cout << "\033[32mImage file converted to ISO:\033[0m " << outputPath << std::endl;
    } else {
        std::cout << "\033[31mConversion of " << inputPath << " failed.\033[0m" << std::endl;

        // Delete the partially created ISO file
        if (std::remove(outputPath.c_str()) == 0) {
            std::cout << "\033[31mDeleted partially created ISO file:\033[0m " << outputPath << std::endl;
        } else {
            std::cerr << "\033[31mFailed to delete partially created ISO file:\033[0m " << outputPath << std::endl;
        }
    }
}


// Function to convert multiple BIN files to ISO format concurrently
void convertBINsToISOs(const std::vector<std::string>& inputPaths, int numThreads) {
    // Check if ccd2iso is installed on the system
    if (!isCcd2IsoInstalled()) {
        std::cout << "\033[31mccd2iso is not installed. Please install it before using this option.\033[0m" << std::endl;
        return;
    }

    // Create a thread pool with a limited number of threads
    std::vector<std::thread> threads;
    int numCores = std::min(numThreads, static_cast<int>(std::thread::hardware_concurrency()));

    for (const std::string& inputPath : inputPaths) {
        if (inputPath == "") {
            break; // Break the loop if an empty path is encountered
        } else {
            // Construct the shell-escaped input path
            std::string escapedInputPath = shell_escape(inputPath);

            // Create a new thread for each conversion
            threads.emplace_back(convertBINToISO, escapedInputPath);

            if (threads.size() >= numCores) {
                // Limit the number of concurrent threads to the number of available cores
                for (auto& thread : threads) {
                    thread.join();
                }
                threads.clear();
            }
        }
    }

    // Join any remaining threads
    for (auto& thread : threads) {
        thread.join();
    }
}


// Function to process a range of files and convert them to ISO format
void processFilesInRange(int start, int end) {
    // Get a list of BIN/IMG files
    std::vector<std::string> binImgFiles;

    // Determine the number of threads based on CPU cores
    int numThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

    // Select files within the specified range
    std::vector<std::string> selectedFiles;
    {
        std::lock_guard<std::mutex> lock(binImgFilesMutex); // Lock the mutex while accessing binImgFiles
        for (int i = start; i <= end; i++) {
            selectedFiles.push_back(binImgFiles[i - 1]);
        }
    } // Unlock the mutex when leaving the scope

    // Construct the shell-escaped file paths
    std::vector<std::string> escapedSelectedFiles;
    for (const std::string& file : selectedFiles) {
        escapedSelectedFiles.push_back(shell_escape(file));
    }

    // Call convertBINsToISOs with shell-escaped file paths
    convertBINsToISOs(escapedSelectedFiles, numThreads);
}


// Main function to select directories and convert BIN/IMG files to ISO format
void select_and_convert_files_to_iso() {
    // Initialize vectors to store BIN/IMG files and directory paths
    std::vector<std::string> binImgFiles;
    std::vector<std::string> directoryPaths;

    // Declare previousPaths as a static variable
    static std::vector<std::string> previousPaths;

    // Read input for directory paths (allow multiple paths separated by semicolons)
    std::string inputPaths = readInputLine("\033[94mEnter the directory path(s) (if many, separate them with \033[33m;\033[0m\033[94m) to search for .bin .img files, or press Enter to return:\n\033[0m");

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
	std::cout << " " << std::endl;
    // Flag to check if new files are found
	bool newFilesFound = false;

	// Call the findBinImgFiles function to populate the cache
	binImgFiles = findBinImgFiles(directoryPaths, previousPaths,
    [&binImgFiles, &newFilesFound](const std::string& fileName, const std::string& filePath) {
        newFilesFound = true;
    });

	// Print a message only if no new files are found
	if (!newFilesFound && !binImgFiles.empty()) {
    std::cout << "\033[31mNo new .bin .img files found to be combined in RAM cache.\033[0m" << std::endl;
    std::cout << "Press enter to continue...";
    std::cin.ignore();
	}

    if (binImgFiles.empty()) {
        std::cout << "\033[31mNo .bin or .img files over 10MB found in the specified directories and their subdirectories or cached in RAM.\n\033[0m";
        std::cout << "Press enter to continue...";
        std::cin.ignore();
        
    } else {
        while (true) {
            std::system("clear");
            // Print the list of BIN/IMG files
            printFileListBin(binImgFiles);
            std::cout << " " << std::endl;
            // Prompt user to choose a file or exit
            std::string input = readInputLine("\033[94mChoose BIN/IMG file(s) to convert (e.g., '1-3' '1 2', or press Enter to return):\033[0m ");

            // Break the loop if the user presses Enter
            if (input.empty()) {
                std::system("clear");
                break;
            }

            std::system("clear");
            // Process user input
            processInputBin(input, binImgFiles);
            std::cout << "Press enter to continue...";
            std::cin.ignore();
        }
    }
}


// Function to print the list of BIN/IMG files
void printFileListBin(const std::vector<std::string>& fileList) {
	std::cout << "Select file(s) to convert to \033[1m\033[32mISO(s)\033[0m:\n";
    for (int i = 0; i < fileList.size(); i++) {
        std::cout << i + 1 << ". " << fileList[i] << std::endl;
    }
}


// Function to process user input and convert selected BIN/IMG files to ISO format
void processInputBin(const std::string& input, const std::vector<std::string>& fileList) {
    // Tokenize the input string
    std::istringstream iss(input);
    std::string token;
    std::vector<std::thread> threads;

    // Set to track processed indices
    std::set<int> processedIndices;

    // Iterate over tokens
    while (iss >> token) {
        std::istringstream tokenStream(token);
        int start, end;
        char dash;

        // Attempt to parse the token as a number
        if (tokenStream >> start) {
            // Check for a range input (e.g., 1-5)
            if (tokenStream >> dash && dash == '-' && tokenStream >> end) {
                // Process a valid range input
                if (start >= 1 && start <= fileList.size() && end >= start && end <= fileList.size()) {
                    for (int i = start; i <= end; i++) {
                        int selectedIndex = i - 1;
                        // Check if the index has already been processed
                        if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                            std::string selectedFile = fileList[selectedIndex];
                            threads.emplace_back(convertBINToISO, selectedFile);
                            processedIndices.insert(selectedIndex);
                        }
                    }
                } else {
                    std::cout << "\033[31mInvalid range: " << start << "-" << end << ". Ensure the starting range is equal to or less than the end, and that numbers align with the list.\033[0m" << std::endl;
                }
            } else if (start >= 1 && start <= fileList.size()) {
                // Process a valid single number input
                int selectedIndex = start - 1;
                // Check if the index has already been processed
                if (processedIndices.find(selectedIndex) == processedIndices.end()) {
                    std::string selectedFile = fileList[selectedIndex];
                    threads.emplace_back(convertBINToISO, selectedFile);
                    processedIndices.insert(selectedIndex);
                }
            } else {
                // Handle invalid number input
                std::cout << "\033[31mFile index " << start << ", does not exist.\033[0m" << std::endl;
            }
        } else {
            // Handle invalid input format
            std::cout << "\033[31mInvalid input: " << token << ".\033[0m" << std::endl;
        }
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
}

// Function to find MDF files in directories
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths,
                                        const std::function<void(const std::string&, const std::string&)>& callback) {
    // Static variables to cache results for reuse
    static std::vector<std::string> mdfMdsFilesCache;
    static std::vector<std::string> cachedPaths;

    // If the cache is not empty and the input paths are the same as the cached paths, return the cached results
    if (!mdfMdsFilesCache.empty() && paths == cachedPaths) {
        return mdfMdsFilesCache;
    }

    // Vector to store file names that match the criteria
    std::vector<std::string> fileNames;

    try {
        // Mutex to ensure thread safety
        std::mutex mutex;
        // Determine the maximum number of threads to use based on hardware concurrency fallback is 2 threads
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

        // Iterate through input paths
        for (const auto& path : paths) {
            // Iterate through files in the given directory and its subdirectories
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    // Check if the file has a ".mdf" extension and is larger than or equal to 10,000,000 bytes
                    std::string ext = entry.path().extension();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                        return std::tolower(c);
                    });

                    if (ext == ".mdf" && std::filesystem::file_size(entry) >= 10'000'000) {
                        // Automatically determine the batch size based on the number of available threads
                        const int batchSize = maxThreads * 2;

                        // Use a lambda function to process the file asynchronously
                        auto processFile = [&fileNames, &mutex, &callback](const std::filesystem::directory_entry& fileEntry) {
                            std::string fileName = fileEntry.path().string();
                            std::string filePath = fileEntry.path().parent_path().string();  // Get the path of the directory

                            // Lock the mutex to ensure safe access to shared data (fileNames)
                            std::lock_guard<std::mutex> lock(mutex);
                            fileNames.push_back(fileName);

                            // Call the callback function to inform about the found file
                            callback(fileName, filePath);
                        };

                        // Process the file asynchronously
                        auto future = std::async(std::launch::async, processFile, entry);

                        // Wait for the asynchronous task to complete
                        future.get();
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors and print an error message
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    // Wait for any remaining asynchronous tasks to complete (may not be necessary)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Remove duplicates from fileNames by sorting and using unique erase idiom
    std::sort(fileNames.begin(), fileNames.end());
    fileNames.erase(std::unique(fileNames.begin(), fileNames.end()), fileNames.end());

    // Update the cache by appending fileNames to mdfMdsFilesCache
    mdfMdsFilesCache.insert(mdfMdsFilesCache.end(), fileNames.begin(), fileNames.end());

    // Update the cached paths by appending the new paths
    cachedPaths.insert(cachedPaths.end(), paths.begin(), paths.end());

    // Return the combined results
    return mdfMdsFilesCache;
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


// Function to convert an MDF file to ISO format using mdf2iso
void convertMDFToISO(const std::string& inputPath) {
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[31mThe specified input file '" << inputPath << "' does not exist.\033[0m" << std::endl;
        return;
    }

    // Check if the corresponding .iso file already exists
    std::string isoOutputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
    if (std::ifstream(isoOutputPath)) {
        std::cout << "\033[33mThe corresponding .iso file already exists for '" << inputPath << "'. Skipping conversion.\033[0m" << std::endl;
        return;
    }

    // Escape the inputPath before using it in shell commands
    std::string escapedInputPath = shell_escape(inputPath);

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Escape the outputPath before using it in shell commands
    std::string escapedOutputPath = shell_escape(outputPath);

    // Continue with the rest of the conversion logic...

    // Execute the conversion using mdf2iso
    std::string conversionCommand = "mdf2iso " + escapedInputPath + " " + escapedOutputPath;
	
    // Capture the output of the mdf2iso command
    FILE* pipe = popen(conversionCommand.c_str(), "r");
    if (!pipe) {
        std::cout << "\033[31mFailed to execute conversion command\033[0m" << std::endl;
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
            std::cout << "\033[31mThe selected file '" << inputPath << "' is already in ISO format, maybe rename it to .iso?. Skipping conversion.\033[0m" << std::endl;
        } else {
            std::cout << "\033[32mImage file converted to ISO:\033[0m " << outputPath << std::endl;
        }
    } else {
        std::cout << "\033[31mConversion of " << inputPath << " failed.\033[0m" << std::endl;
    }
}

// Function to convert multiple MDF files to ISO format using mdf2iso
void convertMDFsToISOs(const std::vector<std::string>& inputPaths) {
    // Check if mdf2iso is installed
    if (!isMdf2IsoInstalled()) {
        std::cout << "\033[31mmdf2iso is not installed. Please install it before using this option.\033[0m";
        return;
    }

    // Determine the number of threads based on hardware concurrency, fallback is 2 threads
    int numThreads = std::thread::hardware_concurrency();
    if (numThreads <= 0) {
        // Fallback to a default number of threads if hardware concurrency is not available
        numThreads = 2;
    }

    // Create a thread pool with a limited number of threads
    std::vector<std::thread> threads;
    int numCores = std::min(numThreads, static_cast<int>(std::thread::hardware_concurrency()));

    for (const std::string& inputPath : inputPaths) {
        if (inputPath == "") {
            break; // Exit the loop
        } else {
            // No need to escape the file path, as we'll handle it in the convertMDFToISO function
            std::string escapedInputPath = inputPath;

            // Create a new thread for each conversion
            threads.emplace_back(convertMDFToISO, escapedInputPath);

            // Limit the number of concurrent threads to the number of available cores
            if (threads.size() >= numCores) {
                for (auto& thread : threads) {
                    thread.join();
                }
                threads.clear();
            }
        }
    }

    // Join any remaining threads
    for (auto& thread : threads) {
        thread.join();
    }
}

// Function to process a range of MDF files by converting them to ISO
void processMDFFilesInRange(int start, int end) {
    std::vector<std::string> mdfImgFiles; // Declare mdfImgFiles here
    int numThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2; // Determine the number of threads based on CPU cores
    std::vector<std::string> selectedFiles;

    // Construct a list of selected files based on the specified range
    {
        std::lock_guard<std::mutex> lock(mdfFilesMutex); // Lock the mutex while accessing mdfImgFiles
        for (int i = start; i <= end; i++) {
            std::string filePath = (mdfImgFiles[i - 1]);
            selectedFiles.push_back(filePath);
        }
    } // Unlock the mutex when leaving the scope

    // Split the selected files among threads
    int filesPerThread = selectedFiles.size() / numThreads;
    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        int startIdx = i * filesPerThread;
        int endIdx = (i == numThreads - 1) ? selectedFiles.size() : (i + 1) * filesPerThread;
        std::vector<std::string> filesSubset(selectedFiles.begin() + startIdx, selectedFiles.begin() + endIdx);

        // Create a thread for each subset of files
        threads.emplace_back([filesSubset]() {
            convertMDFsToISOs(filesSubset);
        });
    }

    // Wait for all threads to finish
    std::for_each(threads.begin(), threads.end(), [](std::thread& t) {
        t.join();
    });
}

// Function to interactively select and convert MDF files to ISO
void select_and_convert_files_to_iso_mdf() {
    // Read input for directory paths (allow multiple paths separated by semicolons)
    std::string inputPaths = readInputLine("\033[94mEnter the directory path(s) (if many, separate them with \033[33m;\033[0m\033[94m) to search for .mdf files, or press Enter to return:\n\033[0m");

    // Initialize vectors to store MDF/MDS files and directory paths
    std::vector<std::string> mdfMdsFiles;
    std::vector<std::string> directoryPaths;

    // Declare previousPaths as a static variable
    static std::vector<std::string> previousPaths;

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
	std::cout << " " << std::endl;
    // Flag to check if new .mdf files are found
	bool newMdfFilesFound = false;

	// Call the findMdsMdfFiles function to populate the cache
	mdfMdsFiles = findMdsMdfFiles(directoryPaths, [&mdfMdsFiles, &newMdfFilesFound](const std::string& fileName, const std::string& filePath) {
    newMdfFilesFound = true;
	});
	

    if (mdfMdsFiles.empty()) {
        std::cout << "\033[31mNo .mdf files over 10MB found in the specified directories and their subdirectories or cached in RAM.\n\033[0m";
        std::cout << "Press enter to continue...";
		std::cin.ignore();
        return;
    }
    // Continue selecting and converting files until the user decides to exit
    while (true) {
        std::system("clear");
        printFileListMdf(mdfMdsFiles);
		std::cout << " " << std::endl;
        // Prompt the user to enter file numbers or 'exit'
        std::string input = readInputLine("\033[94mChoose MDF file(s) to convert (e.g., '1-2' or '1 2', or press Enter to return):\033[0m ");

        if (input.empty()) {
            std::system("clear");
            break;
        }

        // Parse the user input to get selected file indices and capture errors
        std::pair<std::vector<int>, std::vector<std::string>> result = parseUserInput(input, mdfMdsFiles.size());
        std::vector<int> selectedFileIndices = result.first;
        std::vector<std::string> errorMessages = result.second;
        std::system("clear");

        if (!selectedFileIndices.empty()) {
            // Get the paths of the selected files based on user input
            std::vector<std::string> selectedFiles = getSelectedFiles(selectedFileIndices, mdfMdsFiles);

            // Convert the selected MDF files to ISO
            convertMDFsToISOs(selectedFiles);

            // Display errors if any
            for (const auto& errorMessage : errorMessages) {
                std::cerr << errorMessage << std::endl;
            }

            std::cout << "Press enter to continue...";
            std::cin.ignore();
        } else {
            // Display parsing errors
            for (const auto& errorMessage : errorMessages) {
                std::cerr << errorMessage << std::endl;
            }
            std::cout << "Press enter to continue...";
            std::cin.ignore();
        }
    }
}


// Function to print the list of MDF files with their corresponding indices
void printFileListMdf(const std::vector<std::string>& fileList) {
    std::cout << "Select file(s) to convert to \033[1m\033[32mISO(s)\033[0m:\n";
    for (int i = 0; i < fileList.size(); i++) {
        std::cout << i + 1 << ". " << fileList[i] << std::endl;
    }
}


// Function to parse user input and extract selected file indices and errors
std::pair<std::vector<int>, std::vector<std::string>> parseUserInput(const std::string& input, int maxIndex) {
    std::vector<int> selectedFileIndices;
    std::vector<std::string> errorMessages;
    std::istringstream iss(input);
    std::string token;

    // Set to track processed indices
    std::set<int> processedIndices;

    // Iterate through the tokens in the input string
    while (iss >> token) {
        if (token.find('-') != std::string::npos) {
            // Handle a range (e.g., "1-2")
            size_t dashPos = token.find('-');
            int startRange, endRange;

            try {
                startRange = std::stoi(token.substr(0, dashPos));
                endRange = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                errorMessages.push_back("\033[31mInvalid input " + token + ".\033[0m");
                continue;
            } catch (const std::out_of_range& e) {
                errorMessages.push_back("\033[31mInvalid input " + token + ".\033[0m");
                continue;
            }

            // Add each index within the specified range to the selected indices vector
            if (startRange <= endRange && startRange >= 1 && endRange <= maxIndex) {
                for (int i = startRange; i <= endRange; i++) {
                    int currentIndex = i - 1;

                    // Check if the index has already been processed
                    if (processedIndices.find(currentIndex) == processedIndices.end()) {
                        selectedFileIndices.push_back(currentIndex);
                        processedIndices.insert(currentIndex);
                    }
                }
            } else {
                errorMessages.push_back("\033[31mInvalid range: " + token + ". Ensure the starting range is equal to or less than the end, and that numbers align with the list.\033[0m");
            }
        } else {
            // Handle individual numbers (e.g., "1")
            int selectedFileIndex;

            try {
                selectedFileIndex = std::stoi(token);
            } catch (const std::invalid_argument& e) {
                errorMessages.push_back("\033[31mInvalid input: " + token + ".\033[0m");
                continue;
            } catch (const std::out_of_range& e) {
                errorMessages.push_back("\033[31mFile index " + token + ", does not exist.\033[0m");
                continue;
            }

            // Add the index to the selected indices vector if it is within the valid range
            if (selectedFileIndex >= 1 && selectedFileIndex <= maxIndex) {
                int currentIndex = selectedFileIndex - 1;

                // Check if the index has already been processed
                if (processedIndices.find(currentIndex) == processedIndices.end()) {
                    selectedFileIndices.push_back(currentIndex);
                    processedIndices.insert(currentIndex);
                }
            } else {
                errorMessages.push_back("\033[31mFile index " + token + ", does not exist.\033[0m");
            }
        }
    }

    return {selectedFileIndices, errorMessages};
}


// Function to retrieve selected files based on their indices
std::vector<std::string> getSelectedFiles(const std::vector<int>& selectedIndices, const std::vector<std::string>& fileList) {
    std::vector<std::string> selectedFiles;

    // Iterate through the selected indices and add corresponding files to the selected files vector
    for (int index : selectedIndices) {
        selectedFiles.push_back(fileList[index]);
    }

    return selectedFiles;
}
