#include "sanitization_readline.h"
#include "conversion_tools.h"


std::vector<std::string> binImgFilesCache; // Memory cached binImgFiles here
std::vector<std::string> mdfMdsFilesCache; // Memory cached mdfImgFiles here


// BIN/IMG CONVERSION FUNCTIONS	\\

// Function to list and prompt the user to choose a file for conversion
std::string chooseFileToConvert(const std::vector<std::string>& files) {
    std::cout << "\033[32mFound the following .bin and .img files:\033[0m\n";
    for (size_t i = 0; i < files.size(); ++i) {
        std::cout << i + 1 << ": " << files[i] << "\n";
    }

    int choice;
    std::cout << "\033[94mEnter the number of the file you want to convert:\033[0m ";
    std::cin >> choice;

    if (choice >= 1 && choice <= static_cast<int>(files.size())) {
        return files[choice - 1];
    } else {
        std::cout << "\033[31mInvalid choice. Please choose a valid file.\033[31m\n";
        return "";
    }
}

std::vector<std::string> findBinImgFiles(const std::vector<std::string>& directories) {
    if (!binImgFilesCache.empty()) {
        return binImgFilesCache;
    }

    std::vector<std::string> fileNames;
    try {
        std::vector<std::future<void>> futures;
        std::mutex mutex;
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
        const int batchSize = 2; // Tweak the batch size as needed

        for (const auto& directory : directories) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                        return std::tolower(c);
                    });

                    if ((ext == ".bin" || ext == ".img") && (entry.path().filename().string().find("data") == std::string::npos) && (entry.path().filename().string() != "terrain.bin") && (entry.path().filename().string() != "blocklist.bin")) {
                        if (std::filesystem::file_size(entry) >= 10'000'000) {
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

                            futures.push_back(std::async(std::launch::async, [entry, &fileNames, &mutex] {
                                std::string fileName = entry.path().string();

                                std::lock_guard<std::mutex> lock(mutex);
                                fileNames.push_back(fileName);
                            }));

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

        for (auto& future : futures) {
            future.get();
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    binImgFilesCache = fileNames;
    return fileNames;
}


bool isCcd2IsoInstalled() {
    if (std::system("which ccd2iso > /dev/null 2>&1") == 0) {
        return true;
    } else {
        return false;
    }
}

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
    } else {
        // Execute the conversion using ccd2iso, with shell-escaped paths
        std::string conversionCommand = "ccd2iso " + shell_escape(inputPath) + " " + shell_escape(outputPath);
        int conversionStatus = std::system(conversionCommand.c_str());
        if (conversionStatus == 0) {
            std::cout << "\033[32mImage file converted to ISO:\033[0m " << outputPath << std::endl;
        } else {
            std::cout << "\033[31mConversion of " << inputPath << " failed.\033[0m" << std::endl;
        }
    }
}

void convertBINsToISOs(const std::vector<std::string>& inputPaths, int numThreads) {
    if (!isCcd2IsoInstalled()) {
        std::cout << "\033[31mccd2iso is not installed. Please install it before using this option.\033[0m" << std::endl;
        return;
    }

    // Create a thread pool with a limited number of threads
    std::vector<std::thread> threads;
    int numCores = std::min(numThreads, static_cast<int>(std::thread::hardware_concurrency()));

    for (const std::string& inputPath : inputPaths) {
        if (inputPath == "") {
            break; //
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

void processFilesInRange(int start, int end) {
	std::vector<std::string> binImgFiles;
    int numThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2; // Determine the number of threads based on CPU cores
    std::vector<std::string> selectedFiles;
    for (int i = start; i <= end; i++) {
        selectedFiles.push_back(binImgFiles[i - 1]);
    }

    // Construct the shell-escaped file paths
    std::vector<std::string> escapedSelectedFiles;
    for (const std::string& file : selectedFiles) {
        escapedSelectedFiles.push_back(shell_escape(file));
    }

    // Call convertBINsToISOs with shell-escaped file paths
    convertBINsToISOs(escapedSelectedFiles, numThreads);
}

void select_and_convert_files_to_iso() {
    std::vector<std::string> binImgFiles;
    std::vector<std::string> directoryPaths;

    // Read input for directory paths (allow multiple paths separated by spaces)
    std::string inputPaths = readInputLine("\033[94mEnter the directory paths (separated by spaces) to search for .bin .img files or simply press enter to return:\033[0m ");
    std::istringstream iss(inputPaths);
    std::copy(std::istream_iterator<std::string>(iss),
              std::istream_iterator<std::string>(),
              std::back_inserter(directoryPaths));

    if (directoryPaths.empty()) {
        std::cout << "Path input is empty. Exiting." << std::endl;
        return;
    }

    // Call the findBinImgFiles function to populate the cache
    binImgFiles = findBinImgFiles(directoryPaths);

    if (binImgFiles.empty()) {
        std::cout << "\033[33mNo .bin or .img files found in the specified directories and their subdirectories or all files are under 10MB.\n\033[0m";
    } else {
        printFileListBin(binImgFiles);

        std::string input;

        while (true) {
            std::cout << "\033[94mChoose a file to process (enter the number or range e.g., 1-5 or 1 or simply press Enter to return):\033[0m ";
            std::getline(std::cin, input);

            if (input.empty()) {
                std::cout << "Exiting..." << std::endl;
                break;
            }

            processInputBin(input, binImgFiles);
        }
    }
}

void printFileListBin(const std::vector<std::string>& fileList) {
    for (int i = 0; i < fileList.size(); i++) {
        std::cout << i + 1 << ". " << fileList[i] << std::endl;
    }
}

void processInputBin(const std::string& input, const std::vector<std::string>& fileList) {
    std::istringstream iss(input);
    std::string token;
    std::vector<std::thread> threads;

    while (iss >> token) {
        std::istringstream tokenStream(token);
        int start, end;
        char dash;

        if (tokenStream >> start) {
            if (tokenStream >> dash && dash == '-' && tokenStream >> end) {
                // Range input (e.g., 1-5)
                if (start >= 1 && start <= fileList.size() && end >= start && end <= fileList.size()) {
                    for (int i = start; i <= end; i++) {
                        int selectedIndex = i - 1;
                        std::string selectedFile = fileList[selectedIndex];
                        threads.emplace_back(convertBINToISO, selectedFile);
                    }
                } else {
                    std::cout << "\033[31mInvalid range. Please try again.\033[0m" << std::endl;
                }
            } else if (start >= 1 && start <= fileList.size()) {
                // Single number input
                int selectedIndex = start - 1;
                std::string selectedFile = fileList[selectedIndex];
                threads.emplace_back(convertBINToISO, selectedFile);
            } else {
                std::cout << "\033[31mInvalid number: " << start << ". Please try again.\033[0m" << std::endl;
            }
        } else {
            std::cout << "\033[31mInvalid input format: " << token << ". Please try again.\033[0m" << std::endl;
        }
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
}

// MDF/MDS CONVERSION FUNCTIONS	\\

std::vector<std::string> findMdsMdfFiles(const std::string& directory) {
    if (!mdfMdsFilesCache.empty()) {
        return mdfMdsFilesCache;
    }

    std::vector<std::string> fileNames;
    try {
        std::vector<std::future<void>> futures;
        std::mutex mutex;
        const int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
        const int batchSize = 2; // Tweak the batch size as needed

        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                    return std::tolower(c);
                });

                if (ext == ".mdf") {
                    if (std::filesystem::file_size(entry) >= 10'000'000) {
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

                        // Create a task to process the file
                        futures.push_back(std::async(std::launch::async, [entry, &fileNames, &mutex] {
                            std::string fileName = entry.path().string();

                            std::lock_guard<std::mutex> lock(mutex);
                            fileNames.push_back(fileName);
                        }));

                        // Check the batch size and acquire the lock to merge
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

        // Wait for any remaining tasks
        for (auto& future : futures) {
            future.get();
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    mdfMdsFilesCache = fileNames;
    return fileNames;
}

bool isMdf2IsoInstalled() {
    std::string command = "which " + shell_escape("mdf2iso");
    if (std::system((command + " > /dev/null 2>&1").c_str()) == 0) {
        return true;
    } else {
        return false;
    }
}

void convertMDFToISO(const std::string& inputPath) {
    // Check if the input file exists
    if (!std::ifstream(inputPath)) {
        std::cout << "\033[31mThe specified input file '" << inputPath << "' does not exist.\033[0m" << std::endl;
        return;
    }

    // Escape the inputPath before using it in shell commands
    std::string escapedInputPath = shell_escape(inputPath);

    // Define the output path for the ISO file with only the .iso extension
    std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";

    // Escape the outputPath before using it in shell commands
    std::string escapedOutputPath = shell_escape(outputPath);

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
            std::cout << "\033[31mThe selected file '" << inputPath << "' is already in ISO format. Skipping conversion.\033[0m" << std::endl;
        } else {
            std::cout << "\033[33mImage file converted to ISO:\033[0m " << outputPath << std::endl;
        }
    } else {
        std::cout << "\033[31mConversion of " << inputPath << " failed.\033[0m" << std::endl;
    }
}

void convertMDFsToISOs(const std::vector<std::string>& inputPaths, int numThreads) {
    if (!isMdf2IsoInstalled()) {
        std::cout << "\033[31mmdf2iso is not installed. Please install it before using this option.\033[0m";
        return;
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

void processMDFFilesInRange(int start, int end) {
	std::vector<std::string> mdfImgFiles;	// Declare mdfImgFiles here
    int numThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 8; // Determine the number of threads based on CPU cores
    std::vector<std::string> selectedFiles;
    for (int i = start; i <= end; i++) {
        std::string FilePath = (mdfImgFiles[i - 1]);
        selectedFiles.push_back(FilePath);
    }
    convertMDFsToISOs(selectedFiles, numThreads);
}

void select_and_convert_files_to_iso_mdf() {
    int numThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 8;
    std::string directoryPath = readInputLine("\033[94mEnter the directory path to search for .mdf files or simply press enter to return:\033[0m ");

    if (directoryPath.empty()) {
        std::cout << "\033[33mPath input is empty. Exiting.\033[0m" << std::endl;
        return;
    }

    // Call the findMdsMdfFiles function to populate the cache
    std::vector<std::string> mdfMdsFiles = findMdsMdfFiles(directoryPath);

    if (mdfMdsFiles.empty()) {
        std::cout << "\033[31mNo .mdf files found in the specified directory and its subdirectories or all files are under 10MB.\n\033[0m";
        return;
    }

    while (true) {
        printFileListMdf(mdfMdsFiles);

        std::string input = readInputLine("\033[94mEnter the numbers of the files to convert (e.g., '1-2' or '1 2', or simply press enter to return):\033[0m ");

        if (input.empty()) {
            std::cout << "Exiting..." << std::endl;
            break;
        }

        std::vector<int> selectedFileIndices = parseUserInput(input, mdfMdsFiles.size());

        if (!selectedFileIndices.empty()) {
            std::vector<std::string> selectedFiles = getSelectedFiles(selectedFileIndices, mdfMdsFiles);
            convertMDFsToISOs(selectedFiles, numThreads);
        } else {
            std::cout << "Invalid choice. Please enter valid file numbers or 'exit'." << std::endl;
        }
    }
}

void printFileListMdf(const std::vector<std::string>& fileList) {
    std::cout << "Select files to convert to ISO:\n";
    for (int i = 0; i < fileList.size(); i++) {
        std::cout << i + 1 << ". " << fileList[i] << std::endl;
    }
}

std::vector<int> parseUserInput(const std::string& input, int maxIndex) {
    std::vector<int> selectedFileIndices;
    std::istringstream iss(input);
    std::string token;

    while (iss >> token) {
        if (token.find('-') != std::string::npos) {
            // Handle a range (e.g., "1-2")
            size_t dashPos = token.find('-');
            int startRange = std::stoi(token.substr(0, dashPos));
            int endRange = std::stoi(token.substr(dashPos + 1));

            for (int i = startRange; i <= endRange; i++) {
                if (i >= 1 && i <= maxIndex) {
                    selectedFileIndices.push_back(i - 1);
                }
            }
        } else {
            // Handle individual numbers (e.g., "1")
            int selectedFileIndex = std::stoi(token);
            if (selectedFileIndex >= 1 && selectedFileIndex <= maxIndex) {
                selectedFileIndices.push_back(selectedFileIndex - 1);
            }
        }
    }

    return selectedFileIndices;
}

std::vector<std::string> getSelectedFiles(const std::vector<int>& selectedIndices, const std::vector<std::string>& fileList) {
    std::vector<std::string> selectedFiles;

    for (int index : selectedIndices) {
        selectedFiles.push_back(fileList[index]);
    }

    return selectedFiles;
}
