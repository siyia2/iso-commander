#include "sanitization_readline.h"
#include "conversion_tools.h"

// Cache Variables \\

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "iso_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB

std::mutex Mutex4High; // Mutex for mount thread safety
std::mutex Mutex4Low; // Mutex for search thread safety


//	Function prototypes	\\

//	bools

// Mount functions
bool directoryExists(const std::string& path);
bool isNumeric(const std::string& str);

// Iso cache functions
bool isValidDirectory(const std::string& path);
bool ends_with_iso(const std::string& str);
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);

// Unmount functions
bool isDirectoryEmpty(const std::string& path);
bool isValidIndex(int index, size_t isoDirsSize);

//	voids

//Delete functions
bool isAllZeros(const std::string& str);
void select_and_delete_files_by_number();
void handleDeleteIsoFile(const std::string& iso, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);
void processDeleteInput(char* input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);

// Mount functions
void mountISOs(const std::vector<std::string>& isoFiles);
void select_and_mount_files_by_number();
void displayErrorMessage(const std::string& iso);
void printAlreadyMountedMessage(const std::string& isoFile) ;
void printIsoFileList(const std::vector<std::string>& isoFiles);
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet);
void processInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet);


// Iso cache functions
void manualRefreshCache();
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& Mutex4Low);
void removeNonExistentPathsFromCache();

// Unmount functions
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::string& isoDir);

// Art
void printMenu();
void print_ascii();

//	stds

// Unmount functions
std::future<void> asyncUnmountISO(const std::string& isoDir);

// Cache functions
std::vector<std::string> vec_concat(const std::vector<std::string>& v1, const std::vector<std::string>& v2);
std::future<bool> iequals(std::string_view a, std::string_view b);
std::future<bool> FileExists(const std::string& path);
std::vector<std::string> refreshCacheForDirectory(const std::string& path);
std::string getHomeDirectory();
std::vector<std::string> loadCache();


int main() {
    bool exitProgram = false;
    std::string choice;

    while (!exitProgram) {
        bool returnToMainMenu = false;
        std::system("clear");
        print_ascii();
        // Display the main menu options
        printMenu();

        // Prompt for the main menu choice
        char* input = readline("\033[1;94mEnter a choice:\033[0m ");
        if (!input) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }

        std::string choice(input);

        if (choice == "1") { 
            std::system("clear");
            select_and_mount_files_by_number();
            std::system("clear");
        } else {
			// Check if the input length is exactly 1
			if (choice.length() == 1){
            switch (choice[0]) {
            case '2':
                std::system("clear");
                unmountISOs();
                std::system("clear");
                break;
            case '3':
                std::system("clear");
                select_and_delete_files_by_number();
                std::system("clear");
                break;
            case '4':
                while (!returnToMainMenu) {
					std::system("clear");
					std::cout << "\033[1;32m+-------------------------+" << std::endl;
					std::cout << "\033[1;32m|↵ Convert2ISO             |" << std::endl;
					std::cout << "\033[1;32m+-------------------------+" << std::endl;
                    std::cout << "\033[1;32m|1. CCD2ISO              |" << std::endl;
                    std::cout << "\033[1;32m+-------------------------+" << std::endl;
                    std::cout << "\033[1;32m|2. MDF2ISO              |" << std::endl;
                    std::cout << "\033[1;32m+-------------------------+" << std::endl;
                    std::cout << " " << std::endl;
                    char* submenu_input = readline("\033[1;94mChoose a function, or press Enter to return:\033[0m ");

                    if (!submenu_input || std::strlen(submenu_input) == 0) {
					delete[] submenu_input;
					break; // Exit the submenu if input is empty or NULL
					}
					
                    std::string submenu_choice(submenu_input);
                    free(submenu_input);
                    // Check if the input length is exactly 1
					if (submenu_choice.empty() || submenu_choice.length() == 1){
                    switch (submenu_choice[0]) {		
                    case '1':
                        std::system("clear");
                        select_and_convert_files_to_iso();
                        break;
                    case '2':
                        std::system("clear");
                        select_and_convert_files_to_iso_mdf();
                        break;
                    default:
                        break;}
                    }
                }
                break;
            case '5':
                manualRefreshCache();
                std::cout << "Press Enter to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::system("clear");
                break;
            case '6':
				std::system("clear");
                listMountedISOs();
                std::cout << " " << std::endl;
                std::cout << "Press Enter to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::system("clear");
                break;
            case '7':
                exitProgram = true; // Exit the program
                std::cout << " " << std::endl;
                std::cout << "Exiting the program..." << std::endl;
                std::cout << " " << std::endl;
                break;
            default:
                break;
            }
        }
    }
}
    return 0;
}


// ... Function definitions ...

void print_ascii() {
    // Display ASCII art \\

    const char* greenColor = "\x1B[1;32m";
    const char* resetColor = "\x1B[0m"; // Reset color to default

    std::cout << greenColor << R"( _____            ___  _____  _____     ___  __   __   ___   _____  _____     __   __   ___  __   __  _   _  _____  _____  ____         ____
|  ___)    /\    (   )(_   _)|  ___)   (   )|  \ /  | / _ \ |  ___)|  ___)   |  \ /  | / _ \(_ \ / _)| \ | |(_   _)|  ___)|  _ \       (___ \     _      _   
| |_      /  \    | |   | |  | |_       | | |   v   || |_| || |    | |_      |   v   || | | | \ v /  |  \| |  | |  | |_   | |_) )  _  __ __) )  _| |_  _| |_ 
|  _)    / /\ \   | |   | |  |  _)      | | | |\_/| ||  _  || |    |  _)     | |\_/| || | | |  | |   |     |  | |  |  _)  |  __/  | |/ // __/  (_   _)(_   _)
| |___  / /  \ \  | |   | |  | |___     | | | |   | || | | || |    | |___    | |   | || |_| |  | |   | |\  |  | |  | |___ | |     | / /| |___    |_|    |_|  
|_____)/_/    \_\(___)  |_|  |_____)   (___)|_|   |_||_| |_||_|    |_____)   |_|   |_| \___/   |_|   |_| \_|  |_|  |_____)|_|     |__/ |_____)               
                                                                                                                                                                  )" << resetColor << '\n';

}


void printMenu() {
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|       Menu Options       |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+\033[0m" << std::endl;
    std::cout << "\033[1;32m|1. Mount                |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|2. Unmount              |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|3. Delete               |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|4. Convert2ISO          |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|5. Refresh ISO Cache    |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|6. List Mountpoints     |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|7. Exit Program         |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << std::endl;
}


//	CACHE STUFF \\


// Function to check if a file exists
std::future<bool> FileExists(const std::string& filePath) {
    return std::async(std::launch::async, [filePath]() {
        std::lock_guard<std::mutex> highLock(Mutex4High); // Ensure thread safety

        struct stat buffer;
        return (stat(filePath.c_str(), &buffer) == 0);
    });
}


// Function to remove non-existent paths from cache asynchronously
void removeNonExistentPathsFromCache() {
    // Define the path to the cache file
    std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/iso_cache.txt";
    std::vector<std::string> cache; // Vector to store paths read from the cache file

    // Attempt to open the cache file
    std::ifstream cacheFile(cacheFilePath);
    if (!cacheFile) {
        // Display an error message if the cache file is not found
        std::cerr << "\033[91mError: Unable to find cache file, will attempt to create it.\033[0m" << std::endl;
        return;
    }

    // Read paths from the cache file into the cache vector
    for (std::string line; std::getline(cacheFile, line);) {
        cache.push_back(line);
    }

    // Close the cache file
    cacheFile.close();

    // Create a vector to hold futures for asynchronous tasks
    std::vector<std::future<std::vector<std::string>>> futures;

    // Asynchronously check the existence of paths
    for (const auto& path : cache) {
        futures.push_back(std::async(std::launch::async, [path]() {
            std::vector<std::string> result;
            if (FileExists(path).get()) {
                result.push_back(path);
            }
            return result;
        }));
    }

    // Wait for all asynchronous tasks to complete and collect the results
    std::vector<std::string> retainedPaths;
    for (auto& future : futures) {
        std::vector<std::string> result = future.get();

        // Protect the critical section with a mutex
        std::lock_guard<std::mutex> highLock(Mutex4High);
        retainedPaths.insert(retainedPaths.end(), result.begin(), result.end());
    }

    // Open the cache file for writing
    std::ofstream updatedCacheFile(cacheFilePath);
    if (!updatedCacheFile) {
        // Display an error message if unable to open the cache file for writing
        std::cerr << "\033[91mError: Unable to open cache file for writing, check permissions.\033[0m" << std::endl;
        return;
    }

    // Write the retained paths to the updated cache file
    for (const std::string& path : retainedPaths) {
        updatedCacheFile << path << std::endl;
    }

    // Close the updated cache file
    updatedCacheFile.close();
}


// Helper function to concatenate vectors in a reduction clause
std::vector<std::string> vec_concat(const std::vector<std::string>& v1, const std::vector<std::string>& v2) {
    std::vector<std::string> result = v1;
    result.insert(result.end(), v2.begin(), v2.end());
    return result;
}


// Set default cache dir
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}


// Load cache
std::vector<std::string> loadCache() {
    std::vector<std::string> isoFiles;
    std::string cacheFilePath = getHomeDirectory() + "/.cache/iso_cache.txt";
    std::ifstream cacheFile(cacheFilePath);

    if (cacheFile.is_open()) {
        std::string line;
        while (std::getline(cacheFile, line)) {
            // Check if the line is not empty
            if (!line.empty()) {
                isoFiles.push_back(line);
            }
        }
        cacheFile.close();

        // Remove duplicates from the loaded cache
        std::sort(isoFiles.begin(), isoFiles.end());
        isoFiles.erase(std::unique(isoFiles.begin(), isoFiles.end()), isoFiles.end());
    }

    return isoFiles;
}


// Function to check if filepath exists
bool exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}


// Save cache
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize) {
    std::filesystem::path cachePath = cacheDirectory;
    cachePath /= cacheFileName;

    // Check if cache directory exists
    if (!exists(cacheDirectory) || !std::filesystem::is_directory(cacheDirectory)) {
		std::cout << " " << std::endl;
        std::cerr << "\033[91mInvalid cache directory.\033[0m" << std::endl;
        return false;  // Cache save failed
    }

    // Load the existing cache
    std::vector<std::string> existingCache = loadCache();

    // Combine new and existing entries and remove duplicates
    std::set<std::string> combinedCache(existingCache.begin(), existingCache.end());
    for (const std::string& iso : isoFiles) {
        combinedCache.insert(iso);
    }

    // Limit the cache size to the maximum allowed size
    while (combinedCache.size() > maxCacheSize) {
        combinedCache.erase(combinedCache.begin());
    }

    // Open the cache file in write mode (truncating it)
    std::ofstream cacheFile(cachePath, std::ios::out | std::ios::trunc);
    if (cacheFile.is_open()) {
        for (const std::string& iso : combinedCache) {
            cacheFile << iso << "\n";
        }

        // Check if writing to the file was successful
        if (cacheFile.good()) {
            cacheFile.close();
            return true;  // Cache save successful
        } else {
			std::cout << " " << std::endl;
            std::cerr << "\033[91mFailed to write to cache file.\033[0m" << std::endl;
            cacheFile.close();
            return false;  // Cache save failed
        }
    } else {
		std::cout << " " << std::endl;
        std::cerr << "\033[91mInsufficient read/write permissions.\033[0m" << std::endl;
        return false;  // Cache save failed
    }
}


// Function to refresh the cache for a single directory (now returning a vector of ISO files)
std::vector<std::string> refreshCacheForDirectory(const std::string& path) {
    std::cout << "\033[93mProcessing directory path: '" << path << "'.\033[0m" << std::endl;
    
    std::vector<std::string> newIsoFiles;

    // Use std::async to execute parallelTraverse asynchronously
    std::future<void> asyncResult = std::async(std::launch::async, [path, &newIsoFiles]() {
        parallelTraverse(path, newIsoFiles, Mutex4Low);
    });

    // You can perform other tasks here if needed

    // Wait for the asynchronous operation to complete
    asyncResult.wait();

    std::cout << "\033[92mProcessed directory path: '" << path << "'.\033[0m" << std::endl;

    // Return the new entries for this directory
    return newIsoFiles;
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function for manual cache refresh (now asynchronous)
void manualRefreshCache() {
    // Clear the console screen
    std::system("clear");

    // Prompt the user to enter directory paths for manual cache refresh
    std::string inputLine = readInputLine("\033[1;94mEnter the directory path(s) from which to populate the \033[1m\033[92mISO Cache\033[1;94m (if many, separate them with \033[1m\033[93m;\033[0m\033[1;94m), or press Enter to cancel:\n\033[0m");

    // Check if the user canceled the cache refresh
    if (inputLine.empty()) {
        std::cout << "\033[93mCache refresh canceled by user.\033[0m" << std::endl;
        std::cout << " " << std::endl;
        return;
    }
    
	// Lock the mutex before accessing shared resources
	std::lock_guard<std::mutex> highLock(Mutex4High);
	
	
    // Create an input string stream to parse directory paths
    std::istringstream iss(inputLine);
    std::string path;

    // Vector to store all ISO files from multiple directories
    std::vector<std::string> allIsoFiles;

    // Vector to store valid directory paths
    std::vector<std::string> validPaths;

    // Vector to store invalid paths
    std::vector<std::string> invalidPaths;

    // Set to store processed invalid paths
    std::set<std::string> processedInvalidPaths;

    // Vector of futures to store the results of asynchronous tasks
    std::vector<std::future<std::vector<std::string>>> futures;

    // Flags to determine whether cache write errors were encountered
    bool cacheWriteErrorEncountered = false;

    // Iterate through the entered directory paths and print invalid paths
    while (std::getline(iss, path, ';')) {
        // Check if the directory path is valid
        if (isValidDirectory(path)) {
            validPaths.push_back(path); // Store valid paths
        } else {
            // Check if the path has already been processed
            if (processedInvalidPaths.find(path) == processedInvalidPaths.end()) {
                // Print the error message and mark the path as processed
                invalidPaths.push_back("\033[91mInvalid directory path: '" + path + "'. Skipped from processing.\033[0m");
                processedInvalidPaths.insert(path);
            }
        }
    }

    // Check if any invalid paths were encountered and add a gap
    if (!invalidPaths.empty() || !validPaths.empty()) {
        std::cout << " " << std::endl;
    }

    // Print invalid paths
    for (const auto& invalidPath : invalidPaths) {
        std::cout << invalidPath << std::endl;
    }

    if (!invalidPaths.empty() && !validPaths.empty()) {
        std::cout << " " << std::endl;
    }

    // Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();

    // Create a thread for each valid directory to refresh the cache and pass the vector by reference
    std::istringstream iss2(inputLine); // Reset the string stream
    while (std::getline(iss2, path, ';')) {
        // Check if the directory path is valid
        if (!isValidDirectory(path)) {
            continue; // Skip invalid paths
        }

        // Launch an asynchronous task for each directory and store the future
        futures.push_back(std::async(std::launch::async, refreshCacheForDirectory, path));
    }

    // Wait for all asynchronous tasks to finish and retrieve results
    for (auto& future : futures) {
        auto result = future.get(); // Blocking call to get the result
        allIsoFiles.insert(allIsoFiles.end(), result.begin(), result.end());
    }

    // Save the combined cache to disk
    bool saveSuccess = saveCache(allIsoFiles, maxCacheSize);

    // Stop the timer after completing the cache refresh and removal of non-existent paths
    auto end_time = std::chrono::high_resolution_clock::now();

    // Calculate and print the elapsed time
    std::cout << " " << std::endl;
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();

    // Print the time taken for the entire process in bold with one decimal place
    std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

    // Inform the user about the cache refresh status
    if (saveSuccess) {
        std::cout << " " << std::endl;
        std::cout << "\033[92mCache refreshed successfully.\033[0m" << std::endl;
        std::cout << " " << std::endl;
    } else {
        std::cout << " " << std::endl;
        std::cout << "\033[91mCache refresh failed.\033[0m" << std::endl;
        std::cout << " " << std::endl;
    }
}


// Function to perform case-insensitive string comparison using std::string_view asynchronously
std::future<bool> iequals(std::string_view a, std::string_view b) {
    // Using std::async to perform the comparison asynchronously
    return std::async(std::launch::async, [a, b]() {
        // Check if the string views have different sizes, if so, they can't be equal
        if (a.size() != b.size()) {
            return false;
        }

        // Iterate through each character of the string views and compare them
        for (std::size_t i = 0; i < a.size(); ++i) {
            // Convert characters to lowercase using std::tolower and compare them
            if (std::tolower(a[i]) != std::tolower(b[i])) {
                // If characters are not equal, strings are not equal
                return false;
            }
        }

        // If all characters are equal, the strings are case-insensitively equal
        return true;
    });
}


// Function to parallel traverse a directory and find ISO files
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& Mutex4High) {
    try {
        // Get the maximum number of threads supported by the hardware
        const unsigned int maxThreads = std::thread::hardware_concurrency();

        // Vector to store futures for asynchronous tasks
        std::vector<std::future<void>> futures;

        // Iterate through the directory and its subdirectories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            // Check if the entry is a regular file
            if (entry.is_regular_file()) {
                // Get the path of the file
                const std::filesystem::path& filePath = entry.path();

                // Skip empty files or files with ".bin" extension
                if (std::filesystem::file_size(filePath) == 0 || iequals(filePath.stem().string(), ".bin").get()) {
                    continue;
                }

                // Get the file extension as a string
                std::string extensionStr = filePath.extension().string();

                // Convert the string to a string view
                std::string_view extension = extensionStr;

                // Perform case-insensitive comparison asynchronously
                std::future<bool> extensionComparisonFuture = iequals(extension, ".iso");

                // Check the result of the comparison
                if (extensionComparisonFuture.get()) {
                    // Use async to run the task of collecting ISO paths in parallel
                    futures.push_back(std::async(std::launch::async, [filePath, &isoFiles]() {
                        // Process the file content as needed
                        // For example, you can check for ISO file signatures, etc.

                        // Lock the mutex to update the shared vector
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        isoFiles.push_back(filePath.string());
                    }));
                }
            }
        }

        // Wait for all async tasks to complete
        for (auto& future : futures) {
            future.get();
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors and print an error message
        std::cerr << "\033[91m" << e.what() << "\033[0m" << std::endl;
    }
}

// DELETION STUFF \\

// Function to select and delete ISO files by number
void select_and_delete_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
        std::system("clear");
        std::cout << "\033[93mNo ISO(s) available for deletion.\033[0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
        return;
    }

    // Filter isoFiles to include only entries with ".iso" or ".ISO" extensions
    isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
        return !ends_with_iso(iso);
    }), isoFiles.end());


    // Set to track deleted ISO files
    std::unordered_set<std::string> deletedSet;

    // Main loop for selecting and deleting ISO files
    while (true) {
        std::system("clear");
        std::cout << "\033[93m ! ISO DELETION IS IRREVERSIBLE PROCEED WITH CAUTION !\n\033[0m" << std::endl;
        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Prompt user for input
        char* input = readline("\033[1;94mChoose ISO(s) for \033[91mdeletion\033[1;94m (e.g., '1-3', '1 2', or press Enter to return):\033[0m ");
        std::system("clear");
        
        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input[0] == '\0') {
			std::cout << "Press Enter to Return" << std::endl;
            break;
        }

        else {
            // Process user input to select and delete specific ISO files
            processDeleteInput(input, isoFiles, deletedSet);
        }
        
        // Check if the ISO file list is empty
		if (isoFiles.empty()) {
		std::cout << " " << std::endl;
        std::cout << "\033[93mNo ISO(s) available for deletion.\033[0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue..." << std::endl;
        std::cin.get();
        break;
		}
        
        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
		// Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
    }
}

// Function to handle the deletion of an ISO file
void handleDeleteIsoFile(const std::string& iso, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
	
	std::lock_guard<std::mutex> lowLock(Mutex4Low);
	
    // Check if the ISO file is in the cache
    auto it = std::find(isoFiles.begin(), isoFiles.end(), iso);
    if (it != isoFiles.end()) {
        // Escape the ISO file name for the shell command using shell_escape
        std::string escapedIso = shell_escape(iso);

        // Delete the ISO file from the filesystem
        std::string command = "sudo rm -f " + escapedIso;
        int result = std::system(command.c_str());

        if (result == 0) {
            // Get the index of the found ISO file (starting from 1)
            int index = std::distance(isoFiles.begin(), it) + 1;

            // Remove the deleted ISO file from the cache using the index
            isoFiles.erase(isoFiles.begin() + index - 1);

            // Add the ISO file to the set of deleted files
            deletedSet.insert(iso);

            std::cout << "\033[92mDeleted: \033[91m'" << iso << "'\033[92m." << std::endl;
        } else {
            std::cout << "\033[91mError deleting: \033[0m'" << iso << "'\033[91m." << std::endl;
        }
    } else {
        std::cout << "\033[93mFile not found: \033[0m'" << iso << "'\033[93m." << std::endl;
    }
}

// Function to check if a string consists only of zeros
bool isAllZeros(const std::string& str) {
    return str.find_first_not_of('0') == std::string::npos;
}

// Function to process user input for selecting and deleting specific ISO files
void processDeleteInput(char* input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
    std::istringstream iss(input);
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::set<int> processedIndices; // Set to keep track of processed indices

    std::string token;
    std::vector<std::future<void>> futures; // Vector to store std::future objects for each task

    while (iss >> token) {
        // Check if the token consists only of zeros and treat it as a non-existent index
        if (isAllZeros(token)) {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[91mFile index '" + token + "' is not a valid input.\033[0m");
            continue;
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[91mFile index '0' does not exist.\033[0m");
            }
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[91mInvalid input: '" + token + "'.\033[0m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m");
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0m");
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                if (static_cast<size_t>(i) <= isoFiles.size() && processedIndices.find(i) == processedIndices.end()) {
                    processedIndices.insert(i); // Mark as processed
                } else if (static_cast<size_t>(i) > isoFiles.size()) {
                    invalidInput = true;
                    uniqueErrorMessages.insert("\033[91mFile index '" + std::to_string(i) + "' does not exist.\033[0m");
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && processedIndices.find(num) == processedIndices.end()) {
                processedIndices.insert(num); // Mark index as processed
            } else if (num > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[91mFile index '" + std::to_string(num) + "' does not exist.\033[0m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[91mInvalid input: '" + token + "'.\033[0m");
        }
    }

    // Display unique errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[93m" << errorMsg << "\033[0m" << std::endl;
        }
    }

    // Display selected deletions
    if (!processedIndices.empty()) {
        std::cout << "\033[1;94mThe following ISO(s) will be \033[91m*PERMANENTLY DELETED*\033[1;94m :" << std::endl;
        std::cout << " " << std::endl;
        for (const auto& index : processedIndices) {
            std::cout << "\033[93m'" << isoFiles[index - 1] << "'\033[0m" << std::endl;
        }
	}
	if (!uniqueErrorMessages.empty() && processedIndices.empty()) {
		std::cout << " " << std::endl;
		std::cout << "\033[91mNo valid selection(s) for deletion.\033[0m" << std::endl;
	} else {
	// Prompt for confirmation before proceeding
	char confirmation;
	std::cout << " " << std::endl;
	std::cout << "\033[1;94mDo you want to proceed? (y/n):\033[0m ";
	std::cin.get(confirmation);
	
	// Ignore any additional characters in the input buffer, including newline
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

	// Check if the entered character is not 'Y' or 'y'
	if (!(confirmation == 'y' || confirmation == 'Y')) {
		std::cout << " " << std::endl;
		std::cout << "\033[93mDeletion aborted by user.\033[0m" << std::endl;
        return;
		
	} else {
		std::system("clear");
		// Launch deletion tasks for valid selections
		for (const auto& index : processedIndices) {
			if (index >= 1 && static_cast<size_t>(index) <= isoFiles.size()) {
				futures.emplace_back(std::async(std::launch::async, handleDeleteIsoFile, isoFiles[index - 1], std::ref(isoFiles), std::ref(deletedSet)));
			}
		}
	}
}
    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
}



//	MOUNT STUFF	\\

// Function to check if a directory exists
bool directoryExists(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function to mount selected ISO files called from mountISOs
void mountIsoFile(const std::string& isoFile, std::map<std::string, std::string>& mountedIsos) {
    namespace fs = std::filesystem;

    // Use the filesystem library to extract the ISO file name
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string(); // Remove the .iso extension

    // Use the modified ISO file name in the mount point with "iso_" prefix
    std::string mountPoint = "/mnt/iso_" + isoFileName;

    // Lock the global mutex for synchronization
    std::lock_guard<std::mutex> lowLock(Mutex4Low);

    // Check if the mount point directory doesn't exist, create it asynchronously
    if (!fs::exists(mountPoint)) {
        try {
            // Create the mount point directory
            fs::create_directory(mountPoint);

            // Construct the mount command and execute it
            std::string mountCommand = "sudo mount -o loop " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
            if (std::system(mountCommand.c_str()) != 0) {
                throw std::runtime_error("Mount command failed");
            }

            // Store the mount point in the map
            mountedIsos.emplace(isoFile, mountPoint);
            std::cout << "ISO file: \033[92m'" << isoFile << "'\033[0m mounted at: \033[1;94m'" << mountPoint << "'\033[0m." << std::endl;
        } catch (const std::exception& e) {
            // Handle exceptions, log error, and cleanup
            std::cerr << "\033[91mFailed to mount: \033[93m'" << isoFile << "'\033[0m\033[91m." << std::endl;
            fs::remove(mountPoint);
        }
    } else {
        // The mount point directory already exists, so the ISO is considered mounted
        mountedIsos.emplace(isoFile, mountPoint);
        std::cout << "\033[93mISO file: \033[92m'" << isoFile << "'\033[93m is already mounted at: \033[1;94m'" << mountPoint << "'\033[93m.\033[0m" << std::endl;
    }
}


// Function to mount ISO files concurrently using asynchronous tasks
void mountISOs(const std::vector<std::string>& isoFiles) {
    // Map to store mounted ISOs with their corresponding paths
    std::map<std::string, std::string> mountedIsos;

    // Mutex to synchronize access to the map
    std::mutex mapMutex;

    // Vector to store futures for parallel mounting
    std::vector<std::future<void>> futures;

    // Iterate through the list of ISO files and spawn a future for each
    for (const std::string& isoFile : isoFiles) {
        // Create a future for mounting the ISO file and pass the map and mutex by reference
        futures.push_back(std::async(std::launch::async, [isoFile, &mountedIsos, &mapMutex]() {
            // Lock the mutex before accessing the shared map
            std::lock_guard<std::mutex> lock(mapMutex);

            // Call the function that modifies the shared map
            mountIsoFile(isoFile, mountedIsos);
        }));
    }

    // Wait for all asynchronous tasks to complete
    for (auto& future : futures) {
        future.get();
    }
}


bool fileExistsOnDisk(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}


bool ends_with_iso(const std::string& str) {
    std::string lowercase = str;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), ::tolower);
    return (lowercase.size() >= 4) && (lowercase.compare(lowercase.size() - 4, 4, ".iso") == 0);
}


// Function to select and mount ISO files by number
void select_and_mount_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
        std::system("clear");
        std::cout << "\033[93mISO Cache is empty. Please refresh it from the main Menu Options.\033[0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
        return;
    }

    // Filter isoFiles to include only entries with ".iso" or ".ISO" extensions
    isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
        return !ends_with_iso(iso);
    }), isoFiles.end());

    // Check if there are any ISO files to mount
    if (isoFiles.empty()) {
        std::cout << "\033[93mNo .iso files in the cache. Please refresh the cache from the main menu.\033[0m" << std::endl;
        return;
    }

    // Set to track mounted ISO files
    std::unordered_set<std::string> mountedSet;

    // Main loop for selecting and mounting ISO files
    while (true) {
        std::system("clear");
        std::cout << "\033[93m ! IF EXPECTED ISO FILE(s) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\n\033[0m" << std::endl;
        printIsoFileList(isoFiles);
        
		std::cout << " " << std::endl;
		
        // Prompt user for input
        char* input = readline("\033[1;94mChoose ISO(s) for \033[92mmount\033[1;94m (e.g., '1-3', '1 2', '00' mounts all, or press Enter to return):\033[0m ");
        std::system("clear");

        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input[0] == '\0') {   
            std::cout << "Press Enter to Return" << std::endl;
            break;
        }

        // Check if the user wants to mount all ISO files
        if (std::strcmp(input, "00") == 0) {
            std::vector<std::future<void>> futures;
            for (const std::string& iso : isoFiles) {
                // Use std::async to launch each handleIsoFile task in a separate thread
                futures.emplace_back(std::async(std::launch::async, handleIsoFile, iso, std::ref(mountedSet)));
            }

            // Wait for all tasks to complete
            for (auto& future : futures) {
                future.wait();
            }
        } else {
            // Process user input to select and mount specific ISO files
            processInput(input, isoFiles, mountedSet);
        }

        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
        
    }
}


void printIsoFileList(const std::vector<std::string>& isoFiles) {
    // Apply formatting once before the loop
    std::cout << std::right;

    for (std::size_t i = 0; i < isoFiles.size(); ++i) {
        std::cout << std::setw(2) << i + 1 << ". ";

        // Extract directory and filename
        std::size_t lastSlashPos = isoFiles[i].find_last_of('/');
        std::string directory = isoFiles[i].substr(0, lastSlashPos + 1);
        std::string filename = isoFiles[i].substr(lastSlashPos + 1);

        // Print the directory part in the default color
        std::cout << "\033[1m" << directory << "\033[0m";

        // Print the filename part in magenta and bold
        std::cout << "\033[1m\033[95m" << filename << "\033[0m" << std::endl;
    }
}


// Function to handle mounting of a specific ISO file asynchronously
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet) {
	// Lock the mutex before accessing the shared vector
    std::lock_guard<std::mutex> highLock(Mutex4High);
    // Use std::async to execute the function asynchronously
    auto future = std::async(std::launch::async, [&iso, &mountedSet]() {
        // Check if the ISO file exists on disk
        if (fileExistsOnDisk(iso)) {
            // Attempt to insert the ISO file into the set; if it's a new entry, mount it
            if (mountedSet.insert(iso).second) {
                // Mount the ISO file
                mountISOs({iso});  // Pass a vector with a single string to mountISO
            } else {
                // Get the mount path if the ISO file is already mounted
                std::string result = iso;
                printAlreadyMountedMessage(iso);
            }
        } else {
            // Display an error message if the ISO file doesn't exist on disk
            displayErrorMessage(iso);
        }
    });

    // Wait for the asynchronous operation to complete
    future.wait();
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    // Use parallel execution policy for parallelization
    return std::all_of(std::execution::par, str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// Function to process the user input for ISO mounting using multithreading
void processInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet) {
    std::istringstream iss(input);
    bool invalidInput = false;
    std::vector<std::string> errorMessages; // Vector to store error messages
    std::set<int> processedIndices; // Set to keep track of processed indices

    std::string token;
    std::vector<std::future<void>> futures; // Vector to store std::future objects for each task

    while (iss >> token) {
		       // Check if the token is '0' and treat it as an invalid index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                errorMessages.push_back("\033[91mFile index '0' does not exist.\033[0m");
            }
		}
        
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                errorMessages.push_back("\033[91mInvalid input: '" + token + "'.\033[0m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                errorMessages.push_back("\033[91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m");
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                errorMessages.push_back("\033[91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0m");
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                if (static_cast<size_t>(i) <= isoFiles.size() && processedIndices.find(i) == processedIndices.end()) {
                    // Use std::async to launch each task in a separate thread
                    futures.emplace_back(std::async(std::launch::async, handleIsoFile, isoFiles[i - 1], std::ref(mountedSet)));
                    processedIndices.insert(i); // Mark  as processed
                } else if (static_cast<size_t>(i) > isoFiles.size()) {
                    invalidInput = true;
                    errorMessages.push_back("\033[91mFile index '" + std::to_string(i) + "' does not exist.\033[0m");
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && processedIndices.find(num) == processedIndices.end()) {
                // Use std::async to launch each task in a separate thread
                futures.emplace_back(std::async(std::launch::async, handleIsoFile, isoFiles[num - 1], std::ref(mountedSet)));
                processedIndices.insert(num); // Mark index as processed
            } else if (num > isoFiles.size()) {
                invalidInput = true;
                errorMessages.push_back("\033[91mFile index '" + std::to_string(num) + "' does not exist.\033[0m");
            }
        } else {
            invalidInput = true;
            errorMessages.push_back("\033[91mInvalid input: '" + token + "'.\033[0m");
        }
    }

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }

    // Display errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : errorMessages) {
            std::cerr << "\033[93m" << errorMsg << "\033[0m" << std::endl;
        }
    }
}


// Function to display already mounted at message when you are already in the selenct_and_mount()
void printAlreadyMountedMessage(const std::string& isoFile) {
    namespace fs = std::filesystem;
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string();
    std::string mountPoint = "/mnt/iso_" + isoFileName;

    std::cout << "\033[93mISO file: \033[92m'" << isoFile << "'\033[93m is already mounted at: \033[1;94m'" << mountPoint << "'\033[93m.\033[0m" << std::endl;
}


// Function to display an error message when the ISO file does not exist on disk
void displayErrorMessage(const std::string& iso) {
    std::cout << "\033[35mISO file '" << iso << "' does not exist on disk. Please return and re-enter the mount function, or refresh the cache from the main menu.\033[0m" << std::endl;
}


// UMOUNT STUFF	\\

// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    // Vector to store names of mounted ISOs
    std::vector<std::string> isoDirs;

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
        std::cerr << "\033[93mError opening the /mnt directory.\033[0m" << std::endl;
    }

    // Display a list of mounted ISOs with ISO names in bold and magenta text
    if (!isoDirs.empty()) {
        std::cout << "\033[37;1mList of mounted ISO(s):\033[0m" << std::endl; // White and bold
        std::cout << " " << std::endl;
        for (size_t i = 0; i < isoDirs.size(); ++i) {
            std::cout << i + 1 << ". \033[1m\033[95m" << isoDirs[i] << "\033[0m" << std::endl; // Bold and magenta
        }
    } else {
        // Print a message if no ISOs are mounted
        std::cerr << "\033[91mNo mounted ISO(s) found.\033[0m" << std::endl;
    }
}

// Check if the directory is empty before removing it
bool isEmptyDirectory(const std::string& path) {
    // Attempt to check if the directory is empty
    try {
        return std::filesystem::is_empty(path);
    } catch (const std::filesystem::filesystem_error& e) {
        if (e.code().value() == static_cast<int>(std::errc::permission_denied)) {
            // Permission denied error, treat it as an empty directory
            return true;
        } else {
            // Other filesystem error, print the error and return false
            std::cerr << "Error checking if the directory is empty: " << e.what() << std::endl;
            return false;
        }
    }
}

//function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    std::string checkEmptyCommand = "sudo find " + shell_escape(path) + " -mindepth 1 -maxdepth 1 -print -quit | grep -q .";
    int result = system(checkEmptyCommand.c_str());
    return result != 0; // If result is 0, directory is empty; otherwise, it's not empty
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::string& isoDir) {
    // Reusable string for commands
    std::string command;

    // Use std::async to unmount and remove the directory asynchronously
    auto unmountFuture = std::async(std::launch::async, [&isoDir, &command]() {
        // Construct the unmount command with sudo, umount, and suppressing logs
        command = "sudo umount -l " + shell_escape(isoDir) + " > /dev/null 2>&1";
        int result = system(command.c_str());

        // Check if the unmounting was successful
        if (result == 0) {
            std::cout << "Unmounted: \033[92m'" << isoDir << "'\033[0m." << std::endl; // Print success message

            // Check if the directory is empty before removing it
            if (isDirectoryEmpty(isoDir)) {
                // Construct the remove directory command with sudo, rmdir, and suppressing logs
                command = "sudo rmdir " + shell_escape(isoDir) + " 2>/dev/null";
                int removeDirResult = system(command.c_str());

                if (removeDirResult != 0) {
                    std::cerr << "\033[91mFailed to remove directory: \033[93m'" << isoDir << "'\033[91m ...Please check it out manually.\033[0m" << std::endl;
                }
            }
        } else {
            // Print failure message
            std::cerr << "\033[91mFailed to unmount: \033[93m'" << isoDir << "'\033[91m ...Probably not an ISO mountpoint, check it out manually.\033[0m" << std::endl;
        }
    });

    // Wait for the asynchronous tasks to complete
    unmountFuture.get();
}


// Function to perform asynchronous unmounting
std::future<void> asyncUnmountISO(const std::string& isoDir) {
    return std::async(std::launch::async, [](const std::string& isoDir) {
        std::lock_guard<std::mutex> lowLock(Mutex4Low); // Lock the critical section
        unmountISO(isoDir);
    }, isoDir);
}


// Function to check if a given index is within the valid range of available ISOs
bool isValidIndex(int index, size_t isoDirsSize) {
	
    return (index >= 1) && (static_cast<size_t>(index) <= isoDirsSize);
}


// Main function for unmounting ISOs
void unmountISOs() {
    // Set to store unique error messages
    std::set<std::string> uniqueErrorMessages;

    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    while (true) {
        // Display the initial list of mounted ISOs
        listMountedISOs();

        std::vector<std::string> isoDirs;
        std::vector<std::string> errorMessages;  // Store error messages

        // Find and store directories with the name "iso_*" in /mnt using std::filesystem
        for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
            if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                isoDirs.push_back(entry.path().string());
            }
        }

        // Check if there are no mounted ISOs
        if (isoDirs.empty()) {
            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue...";
            std::cin.get(); // Wait for the user to press Enter
            return;
        }

        // Check if there are mounted ISOs and add a newline
        if (!isoDirs.empty()) {
            std::cout << " " << std::endl;
        }

        // Prompt for unmounting input
        char* input = readline("\033[1;94mChoose ISO(s) for \033[92munmount\033[1;94m (e.g. '1-3', '1 2', '00' unmounts all, or press Enter to return):\033[0m ");
        std::system("clear");

        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input[0] == '\0') {
            break;  // Exit the loop
        }

        if (std::strcmp(input, "00") == 0) {
            // Unmount all ISOs asynchronously
            std::vector<std::future<void>> futures;
            for (const std::string& isoDir : isoDirs) {
                futures.push_back(asyncUnmountISO(isoDir));
            }

            // Wait for all asynchronous tasks to complete
            for (auto& future : futures) {
                future.get();
            }

            // Stop the timer after completing the unmounting process
            auto end_time = std::chrono::high_resolution_clock::now();

            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            // Print the time taken for the entire process in bold with one decimal place
            std::cout << " " << std::endl;
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue...";
            std::cin.get();
            std::system("clear");

            continue;  // Restart the loop
        }

        // Split the input into tokens
        std::istringstream iss(input);
        std::vector<int> unmountIndices;
        std::set<int> uniqueIndices;  // Use a set to store unique indices

        std::string token;
        while (iss >> token) {
            // Check if the token is a valid number
            if (std::regex_match(token, std::regex("^\\d+$"))) {
                // Individual number
                int number = std::stoi(token);
                if (isValidIndex(number, isoDirs.size())) {
                    // Check for duplicates
                    if (uniqueIndices.find(number) == uniqueIndices.end()) {
                        uniqueIndices.insert(number);
                        unmountIndices.push_back(number);
                    }

                } else {
                    // Store the error message
                    errorMessages.push_back("\033[91mFile index '" + std::to_string(number) + "' does not exist.\033[0m");
                }
            } else if (std::regex_match(token, std::regex("^(\\d+)-(\\d+)$"))) {
                // Range input (e.g., "1-3" or "3-1")
                std::smatch match;
                std::regex_match(token, match, std::regex("^(\\d+)-(\\d+)$"));
                int startRange = std::stoi(match[1]);
                int endRange = std::stoi(match[2]);

                // Check for valid range
                if (startRange == endRange) {
                    // Handle range with the same start and end index
                    if (isValidIndex(startRange, isoDirs.size())) {
                        // Check for duplicates
                        if (uniqueIndices.find(startRange) == uniqueIndices.end()) {
                            uniqueIndices.insert(startRange);
                            unmountIndices.push_back(startRange);
                        }
                    } else {
                        // Check if the error message for the invalid index is already stored
                        std::string errorMessage = "\033[91mFile index '" + std::to_string(startRange) + "' does not exist.\033[0m";

                        if (uniqueErrorMessages.find(errorMessage) == uniqueErrorMessages.end()) {
                            // If not, store the error message
                            uniqueErrorMessages.insert(errorMessage);
                            errorMessages.push_back(errorMessage);
                        }
                    }
                } else {
                    int step = (startRange < endRange) ? 1 : -1;

                    // Check if the range includes only valid indices
                    bool validRange = true;
                    for (int i = startRange; i != endRange + step; i += step) {
                        if (!isValidIndex(i, isoDirs.size())) {
                            validRange = false;
                            break;
                        }
                    }

                    if (validRange) {
                        for (int i = startRange; i != endRange + step; i += step) {
                            // Check for duplicates
                            if (uniqueIndices.find(i) == uniqueIndices.end()) {
                                uniqueIndices.insert(i);
                                unmountIndices.push_back(i);
                            }
                        }
                    } else {
                        // Store the error message for invalid range
                        errorMessages.push_back("\033[91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m");
                    }
                }
            } else {
                // Store the error message for invalid input format
                errorMessages.push_back("\033[91mInvalid input: '" + token + "'.\033[0m");
            }
        }

        // Determine the number of available CPU cores
        const unsigned int numCores = std::thread::hardware_concurrency();

        // Create a vector of threads to perform unmounting and directory removal concurrently
        std::vector<std::thread> threads;

        for (int index : unmountIndices) {
            // Check if the index is within the valid range
            if (isValidIndex(index, isoDirs.size())) {
                const std::string& isoDir = isoDirs[index - 1];

                // Use a thread for each ISO to be unmounted
                threads.emplace_back([&, isoDir]() {
                    std::lock_guard<std::mutex> highLock(Mutex4High); // Lock the critical section
                    unmountISO(isoDir);
                });
            }
        }

        // Join the threads to wait for them to finish
        for (auto& thread : threads) {
            thread.join();
        }

        // Stop the timer after completing the unmounting process
        auto end_time = std::chrono::high_resolution_clock::now();

        // Print error messages
        for (const auto& errorMessage : errorMessages) {
            if (uniqueErrorMessages.find(errorMessage) == uniqueErrorMessages.end()) {
                // If not found, store the error message and print it
                uniqueErrorMessages.insert(errorMessage);
                std::cerr << "\033[93m" << errorMessage << "\033[0m" << std::endl;
            }
        }

        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << " " << std::endl;
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
        std::system("clear");
    }
}
