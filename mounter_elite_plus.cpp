#include "headers.h"

 
// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "mounter_elite_plus_iso_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB

std::mutex Mutex4High; // Mutex for high level functions
std::mutex Mutex4Low; // Mutex for low level functions

// For cache directory creation
bool gapPrinted = false; // for cache refresh for directory function
bool gapPrintedtraverse = false; // for traverse function

// Vector to store ISO mounts
std::vector<std::string> mountedFiles;
// Vector to store skipped ISO mounts
std::vector<std::string> skippedMessages;
// Vector to store ISO mount errors
std::vector<std::string> errorMessages;
// Vector to store ISO unique input errors
std::unordered_set<std::string> uniqueErrorMessages;


// Vector to store ISO unmounts
std::vector<std::string> unmountedFiles;
// Vector to store ISO unmount errors
std::vector<std::string> unmountedErrors;

// Vector to store deleted ISOs
std::vector<std::string> deletedIsos;
// Vector to store errors for deleted ISOs
std::vector<std::string> deletedErrors;


// Main function
int main(int argc, char *argv[]) {
    bool exitProgram = false;
    std::string choice;

    if (argc == 2 && (std::string(argv[1]) == "--version"|| std::string(argv[1]) == "-v")) {
        printVersionNumber("2.8.3");
        return 0;
    }

    while (!exitProgram) {
        std::system("clear");
        print_ascii();
        // Display the main menu options
        printMenu();
        
        // Clear history
        clear_history();

        // Prompt for the main menu choice
        char* input = readline("\033[1;94mChoose an option:\033[0m\033[1m ");
        if (!input) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }

        std::string choice(input);
        if (choice == "1") {
            submenu1();
        } else {
            // Check if the input length is exactly 1
            if (choice.length() == 1) {
                switch (choice[0]) {
                    case '2':
                        submenu2();
                        break;
                    case '3':
                        manualRefreshCache();
                        std::system("clear");
                        break;
                    case '4':
						exitProgram = true; // Exit the program
                        std::system("clear");
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

// ART

// Print the version number of the program
void printVersionNumber(const std::string& version) {
    
    std::cout << "\x1B[32mMounter-elite-plus v" << version << "\x1B[0m\n" << std::endl; // Output the version number in green color
}


// Function to print ascii
void print_ascii() {
    // Display ASCII art

    const char* Color = "\x1B[1;38;5;214m";
    const char* resetColor = "\x1B[0m"; // Reset color to default
	                                                                                                                           
std::cout << Color << R"(   *                                                                  
 (  `                     )               (       )                   
 )\))(        (        ( /(  (  (     (   )\(  ( /(  (                
((_)()\  (   ))\  (    )\())))\ )(    )\ ((_)\ )\())))\     _     _   
(_()((_) )\ /((_) )\ )(_))//((_(()\  ((_) _((_(_))//((_)  _| |_ _| |_ 
|  \/  |((_(_))( _(_/ | |_(_))  ((_) | __| |(_| |_(_))   |_   _|_   _|
| |\/| / _ | || | ' \ |  _/ -_)| '_| | _|| || |  _/ -_)    |_|   |_|  
|_|  |_\___/\_,_|_||_| \__\___||_|   |___|_||_|\__\___|               
        
)" << resetColor;

}


// Function to print submenu1
void submenu1() {

    while (true) {
        std::system("clear");
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|↵ Manage ISO              |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|1. Mount                 |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|2. Unmount               |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|3. Delete                |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << " " << std::endl;
        char* submenu_input = readline("\033[1;94mChoose a function, or press Enter to return:\033[0m\033[1m ");

        if (!submenu_input || std::strlen(submenu_input) == 0) {
			delete[] submenu_input;
			break; // Exit the submenu if input is empty or NULL
		}
					
          std::string submenu_choice(submenu_input);
         // Check if the input length is exactly 1
        if (submenu_choice.empty() || submenu_choice.length() == 1){
        switch (submenu_choice[0]) {
            case '1':
				std::system("clear");
                select_and_mount_files_by_number();
                break;
            case '2':
				std::system("clear");
                unmountISOs();
                break;
            case '3':
				std::system("clear");
                select_and_delete_files_by_number();
                break;
			}
        }
    }
}


// Function to print submenu2
void submenu2() {
	while (true) {
		std::system("clear");
		std::cout << "\033[1;32m+-------------------------+" << std::endl;
		std::cout << "\033[1;32m|↵ Convert2ISO             |" << std::endl;
		std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|1. CCD2ISO               |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|2. MDF2ISO               |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << " " << std::endl;
        char* submenu_input = readline("\033[1;94mChoose a function, or press Enter to return:\033[0m\033[1m ");

        if (!submenu_input || std::strlen(submenu_input) == 0) {
			delete[] submenu_input;
			break; // Exit the submenu if input is empty or NULL
		}
					
          std::string submenu_choice(submenu_input);
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
			}
		}
	}
	
}


// Function to print menu
void printMenu() {
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|       Menu Options       |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|1. Manage ISO            | " << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|2. Convert2ISO           |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|3. Refresh ISO Cache     |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << "\033[1;32m|4. Exit Program          |" << std::endl;
    std::cout << "\033[1;32m+-------------------------+" << std::endl;
    std::cout << std::endl;
}


// GENERAL STUFF

void clearScrollBuffer() {
    std::cout << "\033[3J\033[H"; // ANSI escape codes for clearing scroll buffer
    std::cout.flush(); // Ensure the output is flushed
}


// Function to check if a string consists only of zeros
bool isAllZeros(const std::string& str) {
    return str.find_first_not_of('0') == std::string::npos;
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    // Use parallel execution policy for parallelization
    return std::all_of(std::execution::par, str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


//	CACHE STUFF

// Function to check if a file exists asynchronously
std::future<std::vector<std::string>> FileExistsAsync(const std::vector<std::string>& paths) {
    return std::async(std::launch::async, [paths]() {
        std::vector<std::string> result;
        for (const auto& path : paths) {
            if (std::filesystem::exists(path)) {
                result.push_back(path);
            }
        }
        return result;
    });
}


// Function to remove non-existent paths from cache asynchronously with basic thread control
void removeNonExistentPathsFromCache() {
    // Define the path to the cache file
    const std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/mounter_elite_plus_iso_cache.txt";

    // Open the cache file for reading
    std::ifstream cacheFile(cacheFilePath);
    if (!cacheFile.is_open()) {
     //   std::cerr << "Error: Unable to open cache file.\n";
        return;
    }

    // RAII: Use a vector to hold paths read from the cache file
    std::vector<std::string> cache;
    {
        // Read paths from the cache file into the cache vector
        for (std::string line; std::getline(cacheFile, line);) {
            cache.push_back(std::move(line));
        }
    } // Cache vector goes out of scope here, ensuring file is closed

    // Calculate dynamic batch size based on the number of available processor cores
    const std::size_t maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    const size_t batchSize = std::max(cache.size() / maxThreads + 1, static_cast<std::size_t>(2));

    // Create a vector to hold futures for asynchronous tasks
    std::vector<std::future<std::vector<std::string>>> futures;
    futures.reserve(cache.size() / batchSize + 1); // Reserve memory for futures

    // Process paths in dynamic batches
    for (size_t i = 0; i < cache.size(); i += batchSize) {
        auto begin = cache.begin() + i;
        auto end = std::min(begin + batchSize, cache.end());

        futures.push_back(std::async(std::launch::async, [begin, end]() {
            // Process batch
            std::future<std::vector<std::string>> futureResult = FileExistsAsync({begin, end});
            return futureResult.get();
        }));
    }

    // Wait for all asynchronous tasks to complete and collect the results
    std::vector<std::string> retainedPaths;
    retainedPaths.reserve(cache.size()); // Reserve memory for retained paths

    for (auto& future : futures) {
        auto result = future.get();

        // Protect the critical section with a mutex
        {
            std::lock_guard<std::mutex> highLock(Mutex4High);
            retainedPaths.insert(retainedPaths.end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
        }
    }

    // Open the cache file for writing
    std::ofstream updatedCacheFile(cacheFilePath);
    if (!updatedCacheFile.is_open()) {
      //  std::cerr << "Error: Unable to open cache file for writing.\n";
        return;
    }

    // Write the retained paths to the updated cache file
    for (const std::string& path : retainedPaths) {
        updatedCacheFile << path << '\n';
    }

    // RAII: Close the updated cache file automatically when it goes out of scope
} // updatedCacheFile goes out of scope here, ensuring file is closed


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
    std::string cacheFilePath = getHomeDirectory() + "/.cache/mounter_elite_plus_iso_cache.txt";
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
        std::cerr << "\033[1;91mInvalid cache directory.\033[0m\033[1m" << std::endl;
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
            std::cerr << "\033[1;91mFailed to write to cache file.\033[0m\033[1m" << std::endl;
            cacheFile.close();
            return false;  // Cache save failed
        }
    } else {
		std::cout << " " << std::endl;
        std::cerr << "\033[1;91mFailed to open ISO cache file: \033[1;93m'"<< cacheDirectory + "/" + cacheFileName <<"'\033[1;91m. Check read/write permissions.\033[0m\033[1m" << std::endl;
        return false;  // Cache save failed
    }
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function to refresh the cache for a single directory
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles) {
    
    std::cout << "\033[1;93mProcessing directory path: '" << path << "'.\033[0m" << std::endl;

    std::vector<std::string> newIsoFiles;

    // Perform the cache refresh for the directory (e.g., using parallelTraverse)
    parallelTraverse(path, newIsoFiles, Mutex4Low);

    // Check if the gap has been printed, if not, print it
    if (!gapPrinted) {
        std::cout << " " << std::endl;
        gapPrinted = true; // Set the flag to true to indicate that the gap has been printed
    }
	// Lock the mutex to protect the shared 'allIsoFiles' vector
    std::lock_guard<std::mutex> highlock(Mutex4High);
    // Append the new entries to the shared vector
    allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());

    std::cout << "\033[1;92mProcessed directory path(s): '" << path << "'.\033[0m" << std::endl;
}


// Function for manual cache refresh
void manualRefreshCache() {
    std::system("clear");
    gapPrinted = false;
    
    // Load history from file
    loadHistory();

    // Prompt the user to enter directory paths for manual cache refresh
    std::string inputLine = readInputLine("\033[1;94mDirectory path(s) ↵ from which to populate the \033[1m\033[1;92mISO Cache\033[94m (if many, separate them with \033[1m\033[1;93m;\033[0m\033[1;94m), or press ↵ to return:\n\033[0m\033[1m");
    
    if (!inputLine.empty()) {
		// Save history to file
		saveHistory();
	}

    // Check if the user canceled the cache refresh
    if (inputLine.empty()) {
        return;
    }

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
    
    // Set to store processed valid paths
    std::set<std::string> processedValidPaths;

    std::vector<std::future<void>> futures;

    // Iterate through the entered directory paths and print invalid paths
    while (std::getline(iss, path, ';')) {
        // Check if the directory path is valid
        if (isValidDirectory(path)) {
            validPaths.push_back(path); // Store valid paths
        } else {
            // Check if the path has already been processed
            if (processedInvalidPaths.find(path) == processedInvalidPaths.end()) {
                // Print the error message and mark the path as processed
                invalidPaths.push_back("\033[1;91mInvalid directory path(s): '" + path + "'. Skipped from processing.\033[0m");
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

    // Create a task for each valid directory to refresh the cache and pass the vector by reference
    std::istringstream iss2(inputLine); // Reset the string stream
    std::size_t runningTasks = 0;  // Track the number of running tasks

    while (std::getline(iss2, path, ';')) {
        // Check if the directory path is valid
        if (!isValidDirectory(path)) {
            continue; // Skip invalid paths
        }

        // Check if the path has already been processed
        if (processedValidPaths.find(path) != processedValidPaths.end()) {
            continue; // Skip already processed valid paths
        }

        // Add a task to the thread pool for refreshing the cache for each directory
        futures.emplace_back(std::async(std::launch::async, refreshCacheForDirectory, path, std::ref(allIsoFiles)));

        ++runningTasks;

        // Mark the path as processed
        processedValidPaths.insert(path);

        // Check if the number of running tasks has reached the maximum allowed
        if (runningTasks >= maxThreads) {
            // Wait for the tasks to complete
            for (auto& future : futures) {
                future.wait();
            }
            // Clear completed tasks from the vector
            futures.clear();
            runningTasks = 0;  // Reset the count of running tasks
        }
    }

    // Wait for the remaining tasks to complete
    for (auto& future : futures) {
        future.wait();
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
    if (saveSuccess && !validPaths.empty() && invalidPaths.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;92mCache refreshed successfully.\033[0m" << std::endl;
        std::cout << " " << std::endl;
    } 
    if (saveSuccess && !validPaths.empty() && !invalidPaths.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;93mCache refreshed with errors from invalid path(s).\033[0m" << std::endl;
        std::cout << " " << std::endl;
    }
    if (saveSuccess && validPaths.empty() && !invalidPaths.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mCache refresh failed due to missing valid path(s).\033[0m" << std::endl;
        std::cout << " " << std::endl;
    } 
    if (!saveSuccess) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mCache refresh failed.\033[0m" << std::endl;
        std::cout << " " << std::endl;
    }
    std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
    std::cin.get();
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


// Function to check if a string ends with ".iso" (case-insensitive)
bool ends_with_iso(const std::string& str) {
    // Convert the string to lowercase
    std::string lowercase = str;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), ::tolower);
    // Check if the string ends with ".iso" by comparing the last 4 characters
    return (lowercase.size() >= 4) && (lowercase.compare(lowercase.size() - 4, 4, ".iso") == 0);
}


// Function to parallel traverse a directory and find ISO files
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& Mutex4Low) {
    try {
        // Vector to store futures for asynchronous tasks
        std::vector<std::future<void>> futures;

        // Iterate over entries in the specified directory and its subdirectories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            // Check if the entry is a regular file
            if (entry.is_regular_file()) {
                const std::filesystem::path& filePath = entry.path();

                // Check file size and skip if less than 5MB or empty, or if it has a ".bin" extension
                const auto fileSize = std::filesystem::file_size(filePath);
                if (fileSize < 5 * 1024 * 1024 || fileSize == 0 || iequals(filePath.stem().string(), ".bin").get()) {
                    continue;
                }

                // Extract the file extension
                std::string_view extension = filePath.extension().string();

                // Check if the file has a ".iso" extension
                if (iequals(extension, ".iso").get()) {
                    // Asynchronously push the file path to the isoFiles vector while protecting access with a mutex
                    futures.push_back(std::async(std::launch::async, [filePath, &isoFiles, &Mutex4Low]() {
                        std::lock_guard<std::mutex> lowLock(Mutex4Low);
                        isoFiles.push_back(filePath.string());
                    }));
                }
            }
        }

        // Wait for all asynchronous tasks to complete
        for (auto& future : futures) {
            future.get();
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors, print a message, and introduce a 2-second delay
        std::cerr << "\033[1;91m" << e.what() << ".\033[0m\033[1m" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}


// DELETION STUFF


// Function to select and delete ISO files by number
void select_and_delete_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
		clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93mNo ISO(s) available for deletion.\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
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
		clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93m ! ISO DELETION IS IRREVERSIBLE PROCEED WITH CAUTION !\n\033[0m\033[1m" << std::endl;

        // Remove non-existent paths from the cache
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
        isoFiles = loadCache();

        // Filter isoFiles to include only entries with ".iso" or ".ISO" extensions
        isoFiles.erase(std::remove_if(isoFiles.begin(), isoFiles.end(), [](const std::string& iso) {
            return !ends_with_iso(iso);
        }), isoFiles.end());

        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Prompt user for input
        char* input = readline("\033[1;94mISO(s) ↵ for \033[1;91mdeletion\033[1;94m (e.g., '1-3', '1 5'), or press ↵ to return:\033[0m\033[1m ");
        std::system("clear");

        // Check if the user wants to return
        if (input[0] == '\0') {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        } else {
			clearScrollBuffer();
			std::system("clear");
            // Process user input to select and delete specific ISO files
            processDeleteInput(input, isoFiles, deletedSet);
        }

        // Check if the ISO file list is empty
        if (isoFiles.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mNo ISO(s) available for deletion.\033[0m\033[1m" << std::endl;
            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue..." << std::endl;
            std::cin.get();
            break;
        }

        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
    }
}


// Function to check if a file exists
bool fileExists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}


// Function to handle the deletion of ISO files in batches
void handleDeleteIsoFile(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& deletedSet) {
    // Lock the global mutex for synchronization
    std::lock_guard<std::mutex> lowLock(Mutex4Low);
    
    // Determine batch size based on the number of isoFiles
    size_t batchSize = 5; // Default batch size
    if (isoFiles.size() > 100) {
        batchSize = 10;
    }
    if (isoFiles.size() > 1000) {
        batchSize = 25;
    }
    if (isoFiles.size() > 10000) {
        batchSize = 50;
    }
    if (isoFiles.size() > 100000) {
        batchSize = 100;
    }
    
    // Track ISO files to delete in the current batch
    std::vector<std::string> isoFilesToDelete;

    // Process each ISO file
    for (const auto& iso : isoFiles) {
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

        // Check if the ISO file is in the cache
        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);

        if (it != isoFilesCopy.end()) {
            // Check if the file exists before attempting to delete
            if (fileExists(iso)) {
                // Add the ISO file to the deletion batch
                isoFilesToDelete.push_back(iso);

                // If the deletion batch reaches the batch size, or no more ISO files to process
                if (isoFilesToDelete.size() == batchSize || &iso == &isoFiles.back()) {
                    // Construct the delete command for the entire batch
                    std::string deleteCommand = "sudo rm -f ";
                    for (const auto& deleteIso : isoFilesToDelete) {
                        deleteCommand += shell_escape(deleteIso) + " ";
                    }
                    deleteCommand += "> /dev/null 2>&1";

                    // Execute the delete command
                    int result = std::system(deleteCommand.c_str());

                    // Process deletion results
                    if (result == 0) {
                        for (const auto& deletedIso : isoFilesToDelete) {
                            deletedSet.insert(deletedIso);
                            std::string deletedIsoInfo = "\033[1;92mDeleted: \033[1;91m'" + isoDirectory + "/" + isoFilename + "'\033[1;92m.\033[0m\033[1m";
                            deletedIsos.push_back(deletedIsoInfo);
                        }
                    } else {
                        for (const auto& deletedIso : isoFilesToDelete) {
                            auto [isoDir, isoFile] = extractDirectoryAndFilename(deletedIso);
                            std::string errorMessageInfo = "\033[1;91mError deleting: \033[0m\033[1m'" + isoDir + "/" + isoFile + "'\033[1;95m.\033[0m\033[1m";
							deletedErrors.push_back(errorMessageInfo);
                        }
                    }

                    // Clear the deletion batch for the next set
                    isoFilesToDelete.clear();
                }
            } else {
                std::cout << "\033[1;35mFile not found: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[0m\033[1m" << std::endl;
            }
        } else {
            std::cout << "\033[1;93mFile not found in cache: \033[0m\033[1m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m.\033[0m\033[1m" << std::endl;
        }
    }
}


// Function to process user input for selecting and deleting specific ISO files
void processDeleteInput(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
    
    // Detect and use the minimum of available threads and ISOs to ensure efficient parallelism; fallback is 2 threads
	unsigned int numThreads = std::min(static_cast<unsigned int>(isoFiles.size()), static_cast<unsigned int>(maxThreads));
	
    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    std::vector<int> validIndices;     // Vector to keep track of valid indices
    std::vector<std::vector<int>> batchedIndices; // Vector to store batches of indices

    std::string token;
    std::vector<std::thread> threads; // Vector to store std::future objects for each task
    threads.reserve(maxThreads);      // Reserve space for maxThreads threads

    // Tokenize the input string
    while (iss >> token) {
		
		// Check if the token consists only of zeros and treat it as a non-existent index
        if (isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0m\033[1m");
            }
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0m\033[1m");
            }
        }
		
        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
            continue;
        }

        // Process ranges specified with hyphens
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
				// Lock to ensure thread safety in a multi-threaded environment
				std::lock_guard<std::mutex> highLock(Mutex4High);
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }
            
			// Lock to ensure thread safety in a multi-threaded environment
            std::lock_guard<std::mutex> highLock(Mutex4High);

            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i); // Mark as processed
                    validIndices.push_back(i);
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    invalidInput = true;
                    uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(i) + "' does not exist.\033[0m\033[1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num); // Mark index as processed
                validIndices.push_back(num);
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[0m\033[1m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
        }
    }

    // Display unique errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0m\033[1m" << std::endl;
        }
    }

    // Display additional information if there are invalid inputs and some valid indices
    if (invalidInput && !validIndices.empty()) {
        std::cout << " " << std::endl;
    }

    // Batch the valid indices into groups of up to 5
    std::vector<int> batch;
    for (const auto& index : validIndices) {
        batch.push_back(index);
        if (batch.size() == 5) {
            batchedIndices.push_back(batch);
            batch.clear();
        }
    }
    if (!batch.empty()) {
        batchedIndices.push_back(batch);
    }

    // Display selected deletions
    if (!batchedIndices.empty()) {
        std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*PERMANENTLY DELETED*\033[1;94m:\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        for (const auto& batch : batchedIndices) {
            for (const auto& index : batch) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m" << std::endl;
            }
        }
    }

    // Display a message if there are no valid selections for deletion
    if (!uniqueErrorMessages.empty() && batchedIndices.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mNo valid selection(s) for deletion.\033[0m\033[1m" << std::endl;
    } else {
        // Prompt for confirmation before proceeding
        std::string confirmation;
        std::cout << " " << std::endl;
        std::cout << "\033[1;94mDo you want to proceed with the \033[1;91mdeletion\033[1;94m of the above? (y/n):\033[0m\033[1m ";
        std::getline(std::cin, confirmation);

        // Check if the entered character is not 'Y' or 'y'
        if (!(confirmation == "y" || confirmation == "Y")) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mDeletion aborted by user.\033[0m\033[1m" << std::endl;
            return;
        } else {
            // Start the timer after user confirmation
            auto start_time = std::chrono::high_resolution_clock::now();

            std::system("clear");
            // Create a thread pool with a limited number of threads
            ThreadPool pool(maxThreads);
            // Use std::async to launch asynchronous tasks
            std::vector<std::future<void>> futures;
            futures.reserve(numThreads);
            
            // Lock to ensure thread safety in a multi-threaded environment
            std::lock_guard<std::mutex> highLock(Mutex4High);
            
            // Launch deletion tasks for each batch of selected indices
            for (const auto& batch : batchedIndices) {
                std::vector<std::string> isoFilesInBatch;
                for (const auto& index : batch) {
                    isoFilesInBatch.push_back(isoFiles[index - 1]);
                }
                futures.emplace_back(pool.enqueue(handleDeleteIsoFile, isoFilesInBatch, std::ref(isoFiles), std::ref(deletedSet)));
            }

            // Wait for all asynchronous tasks to complete
            for (auto& future : futures) {
                future.wait();
            }
            
            clearScrollBuffer();
            std::system("clear");
            
            if (!deletedIsos.empty()) {
                std::cout << " " << std::endl;
            }
        
            // Print all deleted files
            for (const auto& deletedIso : deletedIsos) {
                std::cout << deletedIso << std::endl;
            }
            
            if (!deletedErrors.empty()) {
                std::cout << " " << std::endl;
            }
            
            for (const auto& deletedError : deletedErrors) {
				std::cout << deletedError << std::endl;
			}
            
            // Clear the vector after each iteration
            deletedIsos.clear();

            // Stop the timer after completing all deletion tasks
            auto end_time = std::chrono::high_resolution_clock::now();

            // Calculate and print the elapsed time
            std::cout << " " << std::endl;
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            // Print the time taken for the entire process in bold with one decimal place
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;
        }
    }
}


//	MOUNT STUFF


// Function to select and mount ISO files by number
void select_and_mount_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCache();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
		clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93mISO Cache is empty. Please refresh it from the main Menu Options.\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
        return;
    }

    // Check if there are any ISO files to mount
    if (isoFiles.empty()) {
        std::cout << "\033[1;93mNo .iso files in the cache. Please refresh the cache from the main menu.\033[0m\033[1m" << std::endl;
        return;
    }

    // Set to track mounted ISO files
    std::unordered_set<std::string> mountedSet;

    // Main loop for selecting and mounting ISO files
    while (true) {
		clearScrollBuffer();
        std::system("clear");
        std::cout << "\033[1;93m ! IF EXPECTED ISO FILE(S) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\n\033[0m\033[1m" << std::endl;

        // Remove non-existent paths from the cache after selection
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
        isoFiles = loadCache();

        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Prompt user for input
        char* input = readline("\033[1;94mISO(s) ↵ for \033[1;92mmount\033[1;94m (e.g., '1-3', '1 5', '00' for all), or press ↵ to return:\033[0m\033[1m ");
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
            // Create a ThreadPool with maxThreads
			ThreadPool pool(maxThreads);

			// Process all ISO files asynchronously
			for (size_t i = 0; i < isoFiles.size(); ++i) {
				// Enqueue the mounting task to the thread pool with associated index
				pool.enqueue([i, &isoFiles, &mountedSet]() {
				// Create a vector containing the single ISO file to mount
				std::vector<std::string> isoFilesToMount = { isoFiles[i] }; // Assuming isoFiles is 1-based indexed
				clearScrollBuffer();
				std::system("clear");
				// Call mountIsoFile with the vector of ISO files to mount and the mounted set
				mountIsoFile(isoFilesToMount, mountedSet);
				});
			}
        } else {
			clearScrollBuffer();
			std::system("clear");
            // Process user input to select and mount specific ISO files
            processAndMountIsoFiles(input, isoFiles, mountedSet);
        }
        
        if (!mountedFiles.empty()) {
			std::cout << " " << std::endl;
		}
		
		// Print all mounted files
		for (const auto& mountedFile : mountedFiles) {
			std::cout << mountedFile << std::endl;
		}
		
		if (!skippedMessages.empty()) {
			std::cout << " " << std::endl;
		}
			
		// Print all the stored skipped messages
		for (const auto& skippedMessage : skippedMessages) {
			std::cerr << skippedMessage;
		}
        
        if (!errorMessages.empty()) {
			std::cout << " " << std::endl;
		}
		// Print all the stored error messages
		for (const auto& errorMessage : errorMessages) {
			std::cerr << errorMessage;
		}
		
		if (!uniqueErrorMessages.empty()) {
			std::cout << " " << std::endl;
		}
		
		for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0m\033[1m" << std::endl;
        }
		
		// Clear the vectors after each iteration
		mountedFiles.clear();
		skippedMessages.clear();
		errorMessages.clear();
		uniqueErrorMessages.clear();

        // Stop the timer after completing the mounting process
        auto end_time = std::chrono::high_resolution_clock::now();

        // Calculate and print the elapsed time
        std::cout << " " << std::endl;
        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
    }
}


// Function to print ISO files with alternating colors for sequence numbers
void printIsoFileList(const std::vector<std::string>& isoFiles) {
    // ANSI escape codes for text formatting
    const std::string defaultColor = "\033[0m";
    const std::string bold = "\033[1m";
    const std::string red = "\033[31;1m";   // Red color
    const std::string green = "\033[32;1m"; // Green color
    const std::string magenta = "\033[95m";

    bool useRedColor = true; // Start with red color

    for (size_t i = 0; i < isoFiles.size(); ++i) {
        // Determine sequence number
        int sequenceNumber = i + 1;

        // Determine color based on alternating pattern
        std::string sequenceColor = (useRedColor) ? red : green;
        useRedColor = !useRedColor; // Toggle between red and green

        // Print sequence number with the determined color
        std::cout << sequenceColor << std::right << std::setw(2) << sequenceNumber <<". ";

        // Extract directory and filename
        auto [directory, filename] = extractDirectoryAndFilename(isoFiles[i]);

        // Print the directory part in the default color
        std::cout << defaultColor << bold << directory << defaultColor;

        // Print the filename part in bold
        std::cout << bold << "/" << magenta << filename << defaultColor << std::endl;
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet) {
    // Initialize input string stream with the provided input
    std::istringstream iss(input);
    
    // Flag to track if any invalid input is encountered
    bool invalidInput = false;
    
    // Set to store indices of processed tokens
    std::set<int> processedIndices;
    
    // Set to store valid indices encountered
    std::set<int> validIndices;
    
    // Set to store processed ranges
    std::set<std::pair<int, int>> processedRanges;

    // Create a ThreadPool with maxThreads
    ThreadPool pool(maxThreads);
    
    // Define mutexes for synchronization
    std::mutex MutexForProcessedIndices;
    std::mutex MutexForValidIndices;

    // Iterate through each token in the input stream
    std::string token;
    while (iss >> token) {
        // Check if token consists of only zeros or is not 00
        if (token != "00" && isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[0m\033[1m");
            }
            continue;
        }

        // Check for presence of dash in token
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            // Check for multiple dashes
            if (token.find('-', dashPos + 1) != std::string::npos || 
                (dashPos == 0 || dashPos == token.size() - 1 || !std::isdigit(token[dashPos - 1]) || !std::isdigit(token[dashPos + 1]))) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
                continue;
            }

            // Extract start and end indices from token
            int start, end;
            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
                continue;
            } catch (const std::out_of_range& e) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }

            // Check validity of range indices
            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[0m\033[1m");
                continue;
            }
            
            // Lock the global mutex for synchronization
            std::lock_guard<std::mutex> highLock(Mutex4High);
            // Check if the range has been processed before
            std::pair<int, int> range(start, end);
            if (processedRanges.find(range) == processedRanges.end()) {
                // Enqueue task for marking range as processed
                pool.enqueue([&]() {
                    processedRanges.insert(range);
                });

                // Determine step for iteration
                int step = (start <= end) ? 1 : -1;
                for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                    // Check if the index has been processed before
                    if (processedIndices.find(i) == processedIndices.end()) {
                        // Enqueue task for marking index as processed
                        pool.enqueue([&]() {
                            std::lock_guard<std::mutex> processedLock(MutexForProcessedIndices);
                            processedIndices.insert(i);
                        });

                        // Enqueue mounting task
                        pool.enqueue([&, i]() {
                            std::lock_guard<std::mutex> validLock(MutexForValidIndices);
                            if (validIndices.find(i) == validIndices.end()) { // Ensure not processed before
								validIndices.insert(i);
								std::vector<std::string> isoFilesToMount;
								isoFilesToMount.push_back(isoFiles[i - 1]); // Assuming isoFiles is 1-based indexed
								mountIsoFile(isoFilesToMount, mountedSet);
							}
                        });
                    }
                }
            }
        } else if (isNumeric(token)) {
            // Lock the global mutex for synchronization
            std::lock_guard<std::mutex> highLock(Mutex4High);

            // Handle single index token
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && processedIndices.find(num) == processedIndices.end()) {
                // Enqueue task for marking index as processed
                pool.enqueue([&]() {
                    // Lock the mutex for processedIndices
                    std::lock_guard<std::mutex> processedLock(MutexForProcessedIndices);
                    processedIndices.insert(num);
                });

                // Enqueue mounting task
                pool.enqueue([&, num]() {
                    // Lock the mutex for validIndices
                    std::lock_guard<std::mutex> validLock(MutexForValidIndices);
                    if (validIndices.find(num) == validIndices.end()) { // Ensure not processed before
						validIndices.insert(num);
						std::vector<std::string> isoFilesToMount;
						isoFilesToMount.push_back(isoFiles[num - 1]); // Assuming isoFiles is 0-based indexed
						mountIsoFile(isoFilesToMount, mountedSet);
					}
                });
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[0m\033[1m");
            }
        } else {
            // Handle invalid token
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
        }
    }
}


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::unordered_set<std::string>& mountedSet) {
    // Lock the global mutex for synchronization
    std::lock_guard<std::mutex> lowLock(Mutex4Low);
    
    // Determine batch size based on the number of FilesToMount
    size_t batchSize = 5; // Maximum ISO files per mount command
    if (isoFilesToMount.size() > 100) {
        batchSize = 10;
    }
    if (isoFilesToMount.size() > 1000) {
        batchSize = 25;
    }
    if (isoFilesToMount.size() > 10000) {
        batchSize = 50;
    }
    if (isoFilesToMount.size() > 100000) {
        batchSize = 100;
    }

    namespace fs = std::filesystem;

    std::vector<std::string> batchIsoFiles;

    for (const auto& isoFile : isoFilesToMount) {
        // Use the filesystem library to extract the ISO file name
        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string(); // Remove the .iso extension

        // Use the modified ISO file name in the mount point with "iso_" prefix
        std::string mountPoint = "/mnt/iso_" + isoFileName;

        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);

        // Check if the mount point is already mounted
        if (isAlreadyMounted(mountPoint)) {
            // If already mounted, print a message and return
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m already mounted at: \033[1;94m'" << mountisoDirectory << "/" << mountisoFilename << "'\033[1;93m.\033[0m\033[1m" << std::endl;

            // Create the unordered set after populating skippedMessages
            std::unordered_set<std::string> skippedSet(skippedMessages.begin(), skippedMessages.end());

            // Check for duplicates
            if (skippedSet.find(skippedMessage.str()) == skippedSet.end()) {
                // Error message not found, add it to the vector
                skippedMessages.push_back(skippedMessage.str());
            }

            continue; // Skip mounting this ISO file
        }

        // Add ISO file to the batch for mounting
        batchIsoFiles.push_back(isoFile);

        // If batch is full or reached end of isoFilesToMount, mount the batch
        if (batchIsoFiles.size() == batchSize || &isoFile == &isoFilesToMount.back()) {
            std::stringstream isoPaths;
            for (const auto& path : batchIsoFiles) {
                isoPaths << shell_escape(path) << " ";
            }

            // Construct the sudo command and execute it
            std::string sudoCommand = "sudo -v";
            int sudoResult = system(sudoCommand.c_str());

            if (sudoResult == 0) {
                // Asynchronously check and create the mount point directory
                auto future = std::async(std::launch::async, [&mountPoint]() {
                    if (!fs::exists(mountPoint)) {
                        fs::create_directory(mountPoint);
                    }
                });

                // Wait for the asynchronous operation to complete
                future.wait();

                // Check if the mount point directory was created successfully
                if (fs::exists(mountPoint)) {
                    try {
                        // Construct the mount command and execute it
                        std::string mountCommand = "sudo mount -o loop " + isoPaths.str() + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
                        if (std::system(mountCommand.c_str()) != 0) {
                            throw std::runtime_error("Mount command failed");
                        }

                        // Insert all mount points into the set
                        mountedSet.insert(mountPoint);

                        // Store the mounted file information in the vector
                        std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m\033[1m"
                                                      + "\033[1m mounted at: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename + "'\033[0m\033[1m\033[1m.\033[0m\033[1m";
                        mountedFiles.push_back(mountedFileInfo);

                    } catch (const std::exception& e) {
                        // Handle exceptions and cleanup
                        std::stringstream errorMessage;
                        errorMessage << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[0m\033[1m\033[1;91m.\033[0m\033[1m" << std::endl;
                        fs::remove(mountPoint);

                        std::unordered_set<std::string> errorSet(errorMessages.begin(), errorMessages.end());
                        if (errorSet.find(errorMessage.str()) == errorSet.end()) {
                            // Error message not found, add it to the vector
                            errorMessages.push_back(errorMessage.str());
                        }
                    }
                } else {
                    // Handle failure to create the mount point directory
                    std::cerr << "\033[1;91mFailed to create mount point directory: \033[1;93m" << mountPoint << "\033[0m\033[1m" << std::endl;
                }
            } else {
                // Handle sudo command failure or user didn't provide the password
                std::cerr << "\033[1;91mFailed to authenticate with sudo.\033[0m\033[1m" << std::endl;
            }

            // Clear batch for the next set of ISOs
            batchIsoFiles.clear();
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


// UMOUNT STUFF


// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    // Vector to store names of mounted ISOs
    static std::mutex mtx;
    std::vector<std::string> isoDirs;

    // Lock mutex for accessing shared resource
    std::lock_guard<std::mutex> lock(mtx);

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
        std::cerr << "\033[1;91mError opening the /mnt directory.\033[0m\033[1m" << std::endl;
        return;
    }

    // Display a list of mounted ISOs with ISO names in bold and alternating colors
    if (!isoDirs.empty()) {
        std::cout << "\033[1mList of mounted ISO(s):\033[0m\033[1m" << std::endl; // White and bold
        std::cout << " " << std::endl;

        bool useRedColor = true; // Start with red color for sequence numbers

        for (size_t i = 0; i < isoDirs.size(); ++i) {
            // Determine color based on alternating pattern
            std::string sequenceColor = (useRedColor) ? "\033[31;1m" : "\033[32;1m";
            useRedColor = !useRedColor; // Toggle between red and green

            // Print sequence number with the determined color
            std::cout << sequenceColor << std::setw(2) << i + 1 << ". ";

            // Print ISO directory path in bold and magenta
            std::cout << "\033[0m\033[1m/mnt/iso_\033[1m\033[95m" << isoDirs[i] << "\033[0m\033[1m" << std::endl;
        }
    } else {
        // Print a message if no ISOs are mounted
        std::cerr << "\033[1;91mNo mounted ISO(s) found.\033[0m\033[1m" << std::endl;
    }
}


//function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    std::string checkEmptyCommand = "sudo find " + shell_escape(path) + " -mindepth 1 -maxdepth 1 -print -quit | grep -q .";
    int result = system(checkEmptyCommand.c_str());
    return result != 0; // If result is 0, directory is empty; otherwise, it's not empty
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs) {
    // Determine batch size based on the number of isoDirs
    size_t batchSize = 5;
    if (isoDirs.size() > 100) {
        batchSize = 10;
    }
    if (isoDirs.size() > 1000) {
        batchSize = 25;
    }
    if (isoDirs.size() > 10000) {
        batchSize = 50;
    }
    if (isoDirs.size() > 100000) {
        batchSize = 100;
    }

    // Use std::async to unmount and remove the directories asynchronously
    auto unmountFuture = std::async(std::launch::async, [&isoDirs, batchSize]() {
        // Construct the sudo command
        std::string sudoCommand = "sudo -v";
        int sudoResult = system(sudoCommand.c_str());

        if (sudoResult == 0) {
            // Construct the unmount command with sudo
            std::string unmountCommand = "sudo umount ";
            for (const auto& isoDir : isoDirs) {
                unmountCommand += shell_escape(isoDir) + " ";
            }
            unmountCommand += "> /dev/null 2>&1";
            int removeDirResult __attribute__((unused)) = system(unmountCommand.c_str());

            // Check and remove empty directories
            std::vector<std::string> emptyDirs;
            for (const auto& isoDir : isoDirs) {
                if (isDirectoryEmpty(isoDir)) {
                    emptyDirs.push_back(isoDir);
                } else {
                    // Handle non-empty directory error
                    std::stringstream errorMessage;
                    errorMessage << "\033[1;93mAre you sure \033[1;91m'" << isoDir << "'\033[1;93m is a mountpoint? Directory not empty, cannot be removed.\033[0m\033[1m" << std::endl;

                    if (std::find(unmountedErrors.begin(), unmountedErrors.end(), errorMessage.str()) == unmountedErrors.end()) {
                        unmountedErrors.push_back(errorMessage.str());
                    }
                }
            }

            // Remove empty directories in batches
            while (!emptyDirs.empty()) {
                std::string removeDirCommand = "sudo rmdir ";
                for (size_t i = 0; i < std::min(batchSize, emptyDirs.size()); ++i) {
                    removeDirCommand += shell_escape(emptyDirs[i]) + " ";
                }
                removeDirCommand += "2>/dev/null";

                int removeDirResult = system(removeDirCommand.c_str());

                for (size_t i = 0; i < std::min(batchSize, emptyDirs.size()); ++i) {
                    if (removeDirResult == 0) {
                        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(emptyDirs[i]);
                        std::string unmountedFileInfo = "\033[1mUnmounted: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m\033[1m.";
                        unmountedFiles.push_back(unmountedFileInfo);
                    } else {
                        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(emptyDirs[i]);
                        std::stringstream errorMessage;
                        errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;91m ...Please check it out manually.\033[0m\033[1m" << std::endl;

                        if (std::find(unmountedErrors.begin(), unmountedErrors.end(), errorMessage.str()) == unmountedErrors.end()) {
                            unmountedErrors.push_back(errorMessage.str());
                        }
                    }
                }
                emptyDirs.erase(emptyDirs.begin(), emptyDirs.begin() + std::min(batchSize, emptyDirs.size()));
            }
        } else {
            std::cerr << "\033[1;91mFailed to authenticate with sudo.\033[0m\033[1m" << std::endl;
        }
    });
}


// Function to check if a given index is within the valid range of available ISOs
bool isValidIndex(int index, size_t isoDirsSize) {
    // Use size_t for the comparison to avoid signed/unsigned comparison warnings
    return (index >= 1) && (static_cast<size_t>(index) <= isoDirsSize);
}


// Main function for unmounting ISOs
void unmountISOs() {
    // Set to store unique error messages
    std::set<std::string> uniqueErrorMessages;
    // Set to store valid indices selected for unmounting
    std::set<int> validIndices;

    // Flag to check for invalid input
    bool invalidInput = false;
    
    // Mutexes for synchronization
    std::mutex isoDirsMutex;
    std::mutex errorMessagesMutex;
    std::mutex uniqueErrorMessagesMutex;

    // Path where ISOs are mounted
    const std::string isoPath = "/mnt";

    while (true) {
        listMountedISOs();

        // Vectors to store ISO directories and error messages
        std::vector<std::string> isoDirs;
        std::vector<std::string> errorMessages;
        
        // Reset flags and clear containers
        invalidInput = false;
        uniqueErrorMessages.clear();

        {
            std::lock_guard<std::mutex> isoDirsLock(isoDirsMutex);
            
            // Iterate through the ISO path to find mounted ISOs
            for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
                if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                    isoDirs.push_back(entry.path().string());
                }
            }
        }

        // If no ISOs are mounted, prompt user to continue
        if (isoDirs.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
            std::cin.get();
            return;
        }

        // Display separator if ISOs are mounted
        if (!isoDirs.empty()) {
            std::cout << " " << std::endl;
        }

        // Prompt user to choose ISOs for unmounting
        char* input = readline("\033[1;94mISO(s) ↵ for \033[1;92munmount\033[1;94m (e.g., '1-3', '1 5', '00' for all), or press ↵ to return:\033[0m\033[1m ");
        std::system("clear");

        auto start_time = std::chrono::high_resolution_clock::now();

        // Break loop if user presses Enter
        if (input[0] == '\0') {
            break;
        }

        // Unmount all ISOs if '00' is entered
        if (std::strcmp(input, "00") == 0) {
            std::vector<std::thread> threads;
            // Create a thread pool with a limited number of threads
            ThreadPool pool(maxThreads);
            std::vector<std::future<void>> futures;

            std::lock_guard<std::mutex> isoDirsLock(isoDirsMutex);

            // Enqueue unmounting tasks for all mounted ISOs
            for (const std::string& isoDir : isoDirs) {
                futures.emplace_back(pool.enqueue([isoDir]() {
				std::lock_guard<std::mutex> highLock(Mutex4High);
				unmountISO(std::vector<std::string>{isoDir}); // Pass a vector containing isoDir
				}));
            }

            // Wait for all tasks to finish
            for (auto& future : futures) {
                future.wait();
            }
            
            if (invalidInput && !validIndices.empty()) {
				std::cout << " " << std::endl;
			}
        
			if (!unmountedFiles.empty()) {
				std::cout << " " << std::endl; // Print a blank line before unmounted files
			}
			// Print all unmounted files
			for (const auto& unmountedFile : unmountedFiles) {
				std::cout << unmountedFile << std::endl;
			}

			if (!unmountedErrors.empty()) {
				std::cout << " " << std::endl; // Print a blank line before deleted folders
			}
			// Print all unmounted files
		for (const auto& unmountedError : unmountedErrors) {
			std::cout << unmountedError << std::endl;
		}
			// Clear vectors
			unmountedFiles.clear();
			unmountedErrors.clear();

            auto end_time = std::chrono::high_resolution_clock::now();

            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            std::cout << " " << std::endl;
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;

            std::cout << " " << std::endl;
            std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
            std::cin.get();
            std::system("clear");

            continue;
        }

        // Parse user input to extract indices for unmounting
        std::istringstream iss(input);
        std::vector<int> unmountIndices;
        std::set<int> uniqueIndices;

        std::string token;
        while (iss >> token) {
            if (token != "00" && isAllZeros(token)) {
                if (!invalidInput) {
                    invalidInput = true;
                }
            }

            // Check if token represents a range or a single index
			bool isRange = (std::count(token.begin(), token.end(), '-') == 1 && token.find_first_not_of('-') != std::string::npos && token.find_last_not_of('-') != std::string::npos && token.find('-') > 0 && token.find('-') < token.length() - 1);
			bool isValidToken = std::all_of(token.begin(), token.end(), [](char c) { return std::isdigit(c) || c == '-'; });

			if (isValidToken) {
				if (isRange) {
					std::istringstream rangeStream(token);
					int startRange, endRange;
					char delimiter;
					rangeStream >> startRange >> delimiter >> endRange;

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
					validIndices.insert(i);
					unmountIndices.push_back(i);
					}
				}
        } else {
            errorMessages.push_back("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[0m\033[1m");
            invalidInput = true;
			}
		} else {
			// Check if the token is just a single number with a hyphen
			if (token.front() == '-' || token.back() == '-') {
				errorMessages.push_back("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
				invalidInput = true;
		} else {
			int number = std::stoi(token);
			if (isValidIndex(number, isoDirs.size())) {
				if (uniqueIndices.find(number) == uniqueIndices.end()) {
					uniqueIndices.insert(number);
					validIndices.insert(number);
					unmountIndices.push_back(number);
						}
				} else {
					errorMessages.push_back("\033[1;91mFile index '" + std::to_string(number) + "' does not exist.\033[0m\033[1m");
					invalidInput = true;
					}
				}
			}
		} else {
			errorMessages.push_back("\033[1;91mInvalid input: '" + token + "'.\033[0m\033[1m");
			invalidInput = true;
			}
		}

        std::vector<std::thread> threads;
        // Create a thread pool with a limited number of threads
        ThreadPool pool(maxThreads);
        std::vector<std::future<void>> futures;

        std::lock_guard<std::mutex> isoDirsLock(isoDirsMutex);
		// Enqueue unmounting tasks for all mounted ISOs
        for (int index : unmountIndices) {
            if (isValidIndex(index, isoDirs.size())) {
                const std::string& isoDir = isoDirs[index - 1];

                futures.emplace_back(pool.enqueue([isoDir]() {
				std::lock_guard<std::mutex> highLock(Mutex4High);
				unmountISO(std::vector<std::string>{isoDir}); // Pass a vector containing isoDir
				}));
            }
        }

        for (auto& future : futures) {
            future.wait();
        }
        
        if (!unmountedFiles.empty()) {
			std::cout << " " << std::endl; // Print a blank line before unmounted files
		}
		// Print all unmounted files
		for (const auto& unmountedFile : unmountedFiles) {
			std::cout << unmountedFile << std::endl;
		}

		if (!unmountedErrors.empty()) {
			std::cout << " " << std::endl; // Print a blank line before deleted folders
		}
		// Print all unmounted files
		for (const auto& unmountedError : unmountedErrors) {
			std::cout << unmountedError << std::endl;
		}

		// Clear vectors
		
		// Clear vectors
		unmountedFiles.clear();
		unmountedErrors.clear();
		
        // Lock access to error messages
        std::lock_guard<std::mutex> errorMessagesLock(errorMessagesMutex);
        
        if (invalidInput) {
            std::cout << " " << std::endl;
        }

        // Print error messages
        for (const auto& errorMessage : errorMessages) {
            if (uniqueErrorMessages.find(errorMessage) == uniqueErrorMessages.end()) {
                // If not found, store the error message and print it
                uniqueErrorMessages.insert(errorMessage);
                std::cerr << "\033[1;93m" << errorMessage << "\033[0m\033[1m" << std::endl;
            }
        }

        // Stop the timer after completing the unmounting process
        auto end_time = std::chrono::high_resolution_clock::now();


        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << " " << std::endl;
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\033[1m" << std::endl;

        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[0m\033[1m";
        std::cin.get();
        std::system("clear");
    }
}
