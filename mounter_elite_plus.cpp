#include "sanitization_extraction_readline.h"
#include "conversion_tools.h"

// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "iso_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB

std::mutex Mutex4High; // Mutex for high level functions
std::mutex Mutex4Med; // Mutex for middle level functions
std::mutex Mutex4Low; // Mutex for low level functions
std::mutex Mutex4ISO; // Mutex for handleIsoFile

bool gapPrinted = false; // for cache refresh for directory function
bool gapPrintedtraverse = false; // for traverse function

//	Function prototypes

//	bools

//Delete functions
bool fileExists(const std::string& filename);

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

//General functions
bool isAllZeros(const std::string& str);

//Delete functions
void select_and_delete_files_by_number();
void handleDeleteIsoFile(const std::string& iso, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);
void processDeleteInput(const char* input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet);

// Mount functions
void mountIsoFile(const std::string& isoFile, std::map<std::string, std::string>& mountedIsos);
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
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void removeNonExistentPathsFromCache();

// Unmount functions
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::string& isoDir);

// Art
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1();
void submenu2();
void print_ascii();

//	stds


// Cache functions
std::vector<std::string> vec_concat(const std::vector<std::string>& v1, const std::vector<std::string>& v2);
std::future<bool> iequals(std::string_view a, std::string_view b);
std::future<bool> FileExists(const std::string& path);
std::string getHomeDirectory();
std::vector<std::string> loadCache();


int main(int argc, char *argv[]) {
    bool exitProgram = false;
    std::string choice;
    
    if (argc == 2 && (std::string(argv[1]) == "--version"|| std::string(argv[1]) == "-v")) {
        printVersionNumber("2.5.2");
        return 0;
    }  

    while (!exitProgram) {
        std::system("clear");
        print_ascii();
        // Display the main menu options
        printMenu();

        // Prompt for the main menu choice
        char* input = readline("\033[1;94mChoose an option:\033[1;0m ");
        if (!input) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }

        std::string choice(input);
		if (choice == "1") {
        submenu1();
        
		} else {
			// Check if the input length is exactly 1
			if (choice.length() == 1){
            switch (choice[0]) {
            case '2':
                submenu2();
                break;
            case '3':
                manualRefreshCache();
                std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::system("clear");
                break;
            case '4':
                exitProgram = true; // Exit the program
                std::cout << " " << std::endl;
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


// Print the version number of the program
void printVersionNumber(const std::string& version) {
    
    std::cout << "\x1B[32mMounter-elite-plus v" << version << "\x1B[0m\n" << std::endl; // Output the version number in green color
}


void print_ascii() {
    // Display ASCII art

    const char* Color = "\x1B[1;38;5;214m";
    const char* resetColor = "\x1B[0m"; // Reset color to default
	                                                                                                                           
std::cout << Color << R"(   *         )               )                  (              (      (                                      
 (  `     ( /(            ( /(    *   )         )\ )           )\ )   )\ )    *   )                          
 )\))(    )\())      (    )\()) ` )  /(   (    (()/(     (    (()/(  (()/(  ` )  /(   (                      
((_)()\  ((_)\       )\  ((_)\   ( )(_))  )\    /(_))    )\    /(_))  /(_))  ( )(_))  )\        _       _    
(_()((_)   ((_)   _ ((_)  _((_) (_(_())  ((_)  (_))     ((_)  (_))   (_))   (_(_())  ((_)     _| |_   _| |_  
|  \/  |  / _ \  | | | | | \| | |_   _|  | __| | _ \    | __| | |    |_ _|  |_   _|  | __|   |_   _| |_   _| 
| |\/| | | (_) | | |_| | | .` |   | |    | _|  |   /    | _|  | |__   | |     | |    | _|      |_|     |_|   
|_|  |_|  \___/   \___/  |_|\_|   |_|    |___| |_|_\    |___| |____| |___|    |_|    |___|                                                                            

)" << resetColor;
}


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
        char* submenu_input = readline("\033[1;94mChoose a function, or press Enter to return:\033[1;0m ");

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
            case '4':
                // The user chose to go back to the main menu, so return from the function
                return;
        }	}
    }
}


void submenu2() {
	while (true) {
		std::system("clear");
		std::cout << "\033[1;32m+-------------------------+" << std::endl;
		std::cout << "\033[1;32m|↵ Convert2ISO             |" << std::endl;
		std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|1. CCD2ISO              |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << "\033[1;32m|2. MDF2ISO              |" << std::endl;
        std::cout << "\033[1;32m+-------------------------+" << std::endl;
        std::cout << " " << std::endl;
        char* submenu_input = readline("\033[1;94mChoose a function, or press Enter to return:\033[1;0m ");

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
             case '4':
				return;
			}
		}
	}
	
}


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
    std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/iso_cache.txt";
    std::vector<std::string> cache; // Vector to store paths read from the cache file

    // Attempt to open the cache file
    std::ifstream cacheFile(cacheFilePath);

    // Read paths from the cache file into the cache vector
    for (std::string line; std::getline(cacheFile, line);) {
        cache.push_back(line);
    }

    // Close the cache file
    cacheFile.close();

    // Calculate dynamic batch size based on the number of available processor cores
    const std::size_t maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    const size_t batchSize = (maxThreads > 1) ? std::max(cache.size() / maxThreads, static_cast<size_t>(2)) : cache.size();
    
    // Semaphore to limit the number of concurrent threads
    sem_t semaphore;
    sem_init(&semaphore, 0, maxThreads); // Initialize the semaphore with the number of threads allowed

    // Create a vector to hold futures for asynchronous tasks
    std::vector<std::future<std::vector<std::string>>> futures;

    // Process paths in dynamic batches
    for (size_t i = 0; i < cache.size(); i += batchSize) {
        // Acquire semaphore token
        sem_wait(&semaphore);
        
        auto begin = cache.begin() + i;
        auto end = std::min(begin + batchSize, cache.end());

        futures.push_back(std::async(std::launch::async, [&semaphore, begin, end]() {
            // Process batch
			std::future<std::vector<std::string>> futureResult = FileExistsAsync({begin, end});
			std::vector<std::string> result = futureResult.get();	
            
            // Release semaphore token
            sem_post(&semaphore);
            
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
    
    // Clean up semaphore
    sem_destroy(&semaphore);

    // Open the cache file for writing
    std::ofstream updatedCacheFile(cacheFilePath);

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
        std::cerr << "\033[1;91mInvalid cache directory.\033[1;0m" << std::endl;
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
            std::cerr << "\033[1;91mFailed to write to cache file.\033[1;0m" << std::endl;
            cacheFile.close();
            return false;  // Cache save failed
        }
    } else {
		std::cout << " " << std::endl;
        std::cerr << "\033[1;91mInsufficient read/write permissions.\033[1;0m" << std::endl;
        return false;  // Cache save failed
    }
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function to refresh the cache for a single directory
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles) {
    // Define a static variable to track whether the gap has been printed
    

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

    // Append the new entries to the shared vector
    allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());

    std::cout << "\033[1;92mProcessed directory path: '" << path << "'.\033[0m" << std::endl;
}


// Function for manual cache refresh
void manualRefreshCache() {
    std::system("clear");
    gapPrinted = false;

    // Prompt the user to enter directory paths for manual cache refresh
    std::string inputLine = readInputLine("\033[1;94mEnter the directory path(s) from which to populate the \033[1m\033[1;92mISO Cache\033[94m (if many, separate them with \033[1m\033[1;93m;\033[0m\033[1;94m), or press Enter to cancel:\n\033[0m");

    // Check if the user canceled the cache refresh
    if (inputLine.empty()) {
        std::cout << "\033[1;93mCache refresh canceled by user.\033[0m" << std::endl;
        std::cout << " " << std::endl;
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

    // Set up a thread pool with a maximum number of threads, fallback to two threads if hardware concurrency is not available or not positive
    const std::size_t maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

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
                invalidPaths.push_back("\033[1;91mInvalid directory path: '" + path + "'. Skipped from processing.\033[0m");
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
        std::cerr << "\033[1;91m" << e.what() << ".\033[1;0m" << std::endl;
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
        std::system("clear");
        std::cout << "\033[1;93mNo ISO(s) available for deletion.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
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
        std::cout << "\033[1;93m ! ISO DELETION IS IRREVERSIBLE PROCEED WITH CAUTION !\n\033[1;0m" << std::endl;

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
        char* input = readline("\033[1;94mChoose ISO(s) for \033[1;91mdeletion\033[1;94m (e.g., '1-3', '1 2', or press Enter to return):\033[1;0m ");
        std::system("clear");

        // Check if the user wants to return
        if (input[0] == '\0') {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        } else {
            // Process user input to select and delete specific ISO files
            processDeleteInput(input, isoFiles, deletedSet);
        }

        // Check if the ISO file list is empty
        if (isoFiles.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mNo ISO(s) available for deletion.\033[1;0m" << std::endl;
            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue..." << std::endl;
            std::cin.get();
            break;
        }

        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.get();
    }
}


// Function to check if a file exists
bool fileExists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}


// Function to handle the deletion of an ISO file
void handleDeleteIsoFile(const std::string& iso, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
    std::lock_guard<std::mutex> lowLock(Mutex4Low);
    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(iso);

    // Static variable to track whether the clear has been performed
    static bool clearScreenDone = false;

    // Check if the ISO file is in the cache
    auto it = std::find(isoFiles.begin(), isoFiles.end(), iso);
    if (it != isoFiles.end()) {
        // Escape the ISO file name for the shell command using shell_escape
        std::string escapedIso = shell_escape(iso);
        
        // Construct the sudo command
        std::string sudoCommand = "sudo -v";

        // Execute sudo to prompt for password
        int sudoResult = system(sudoCommand.c_str());
		
        // Clear the screen only if it hasn't been done yet
        if (sudoResult == 0) {
            // Clear the screen only if it hasn't been done yet
            if (!clearScreenDone) {
                std::system("clear");
                clearScreenDone = true;
            }

            // Check if the file exists before attempting to delete
            if (fileExists(iso)) {

                // Delete the ISO file from the filesystem
                std::string command = "sudo rm -f " + escapedIso + " > /dev/null 2>&1";
                int result = std::system(command.c_str());

                if (result == 0) {
                    // Get the index of the found ISO file (starting from 1)
                    int index = std::distance(isoFiles.begin(), it) + 1;

                    // Remove the deleted ISO file from the cache using the index
                    isoFiles.erase(isoFiles.begin() + index - 1);

                    // Add the ISO file to the set of deleted files
                    deletedSet.insert(iso);

                    std::cout << "\033[1;92mDeleted: \033[1;91m'" << isoDirectory << "/" << isoFilename << "'\033[1;92m.\033[1;0m" << std::endl;
                } else {
                    // Print error message in magenta and bold when rm command fails
                    std::cout << "\033[1;91mError deleting: \033[1;0m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[1;0m" << std::endl;
                }
            } else {
                std::cout << "\033[1;35mFile not found: \033[1;0m'" << isoDirectory << "/" << isoFilename << "'\033[1;95m.\033[1;0m" << std::endl;
            }
        } else {
            // Handle the case when sudo authentication fails
            std::cout << " " << std::endl;
            std::cout << "\033[1;91mFailed to authenticate with sudo.\033[1;0m" << std::endl;
        }
    } else {
        std::cout << "\033[1;93mFile not found in cache: \033[1;0m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m.\033[1;0m" << std::endl;
    }
}

// Function to check if a string consists only of zeros
bool isAllZeros(const std::string& str) {
    return str.find_first_not_of('0') == std::string::npos;
}


// Function to process user input for selecting and deleting specific ISO files
void processDeleteInput(const char* input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& deletedSet) {
    // Lock to ensure thread safety in a multi-threaded environment
    std::lock_guard<std::mutex> highLock(Mutex4High);
    
    // Detect and use the minimum of available threads and ISOs to ensure efficient parallelism; fallback is 2 threads
    unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    unsigned int numThreads = std::min(static_cast<unsigned int>(isoFiles.size()), static_cast<unsigned int>(maxThreads));
    
    // Create an input string stream to tokenize the user input
    std::istringstream iss(input);

    // Variables for tracking errors, processed indices, and valid indices
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::vector<int> processedIndices; // Vector to keep track of processed indices
    std::vector<int> validIndices;     // Vector to keep track of valid indices

    std::string token;
    std::vector<std::thread> threads; // Vector to store std::future objects for each task
    threads.reserve(maxThreads);      // Reserve space for maxThreads threads

    // Tokenize the input string
    while (iss >> token) {
		
		// Check if the token consists only of zeros and treat it as a non-existent index
        if (isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[1;0m");
            }
        }

        // Check if the token is '0' and treat it as a non-existent index
        if (token == "0") {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[1;0m");
            }
        }
		
        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
            continue;
        }

        // Process ranges specified with hyphens
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[1;0m");
                continue;
            }

            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m");
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
                    uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(i) + "' does not exist.\033[1;0m");
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
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[1;0m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
        }
    }

    // Display unique errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[1;0m" << std::endl;
        }
    }

    // Display additional information if there are invalid inputs and some valid indices
    if (invalidInput && !validIndices.empty()) {
        std::cout << " " << std::endl;
    }

    // Display selected deletions
    if (!processedIndices.empty()) {
        std::cout << "\033[1;94mThe following ISO(s) will be \033[1;91m*PERMANENTLY DELETED*\033[1;94m:\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        for (const auto& index : processedIndices) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
            std::cout << "\033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;0m" << std::endl;
        }
    }

    // Display a message if there are no valid selections for deletion
    if (!uniqueErrorMessages.empty() && processedIndices.empty()) {
        std::cout << " " << std::endl;
        std::cout << "\033[1;91mNo valid selection(s) for deletion.\033[1;0m" << std::endl;
    } else {
        // Prompt for confirmation before proceeding
        char confirmation;
        std::cout << " " << std::endl;
        std::cout << "\033[1;94mDo you want to proceed with the \033[1;91mdeletion\033[1;94m of the above? (y/n):\033[1;0m ";
        std::cin.get(confirmation);

        // Ignore any additional characters in the input buffer, including newline
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        // Check if the entered character is not 'Y' or 'y'
        if (!(confirmation == 'y' || confirmation == 'Y')) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;93mDeletion aborted by user.\033[1;0m" << std::endl;
            return;
        } else {
            // Start the timer after user confirmation
            auto start_time = std::chrono::high_resolution_clock::now();

            std::system("clear");

            // Use std::async to launch asynchronous tasks
            std::vector<std::future<void>> futures;
            futures.reserve(numThreads);
            
            // Calculate the total number of tasks to be launched
	int totalTasks = validIndices.size();

            // Launch deletion tasks for each selected index
for (const auto& index : processedIndices) {
    if (index >= 1 && static_cast<size_t>(index) <= isoFiles.size()) {
        // Check if the total number of tasks exceeds the double of available hardware threads
        if (totalTasks <= 2 * maxThreads) {
            // If so, launch tasks with the deferred policy
            futures.emplace_back(std::async(std::launch::deferred, handleDeleteIsoFile, isoFiles[index - 1], std::ref(isoFiles), std::ref(deletedSet)));
        } else {
            // Otherwise, launch tasks with the async policy
            futures.emplace_back(std::async(std::launch::async, handleDeleteIsoFile, isoFiles[index - 1], std::ref(isoFiles), std::ref(deletedSet)));
        }
    }
}

            // Wait for all asynchronous tasks to complete
            for (auto& future : futures) {
                future.wait();
            }

            // Stop the timer after completing all deletion tasks
            auto end_time = std::chrono::high_resolution_clock::now();

            // Calculate and print the elapsed time
            std::cout << " " << std::endl;
            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            // Print the time taken for the entire process in bold with one decimal place
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        }
    }
}


//	MOUNT STUFF

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
    auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);
    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
    // Static variable to track whether the clear has been performed
    static bool clearScreenDone = false;

    // Construct the sudo command
    std::string sudoCommand = "sudo -v";

    // Execute sudo to prompt for password
    int sudoResult = system(sudoCommand.c_str());

    if (sudoResult == 0) {
        // Clear the screen only if it hasn't been done yet
        if (!clearScreenDone) {
            std::system("clear");
            clearScreenDone = true;
        }

        // Asynchronously check and create the mount point directory
        auto future = std::async(std::launch::async, [&mountPoint]() {
            if (!std::filesystem::exists(mountPoint)) {
                std::filesystem::create_directory(mountPoint);
            }
        });

        // Wait for the asynchronous operation to complete
        future.wait();

        // Check if the mount point directory was created successfully
        if (std::filesystem::exists(mountPoint)) {
            try {
                // Construct the mount command and execute it
                std::string mountCommand = "sudo mount -o loop " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
                if (std::system(mountCommand.c_str()) != 0) {
                    throw std::runtime_error("Mount command failed");
                }

                // Remove the ".iso" extension from the mountisoFilename
                std::string mountisoFilenameWithoutExtension = isoFilename.substr(0, isoFilename.length() - 4);

                // Store the mount point in the map
                mountedIsos.emplace(isoFile, mountPoint);
                std::cout << "\033[1mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[1;0m "
                          << "\033[1mmounted at: \033[1;94m'" << mountisoDirectory << "/" << mountisoFilename << "'\033[1;0m\033[1m.\033[1;0m" << std::endl;
            } catch (const std::exception& e) {
                // Handle exceptions, log error, and cleanup
                std::cerr << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;0m\033[1;91m.\033[1;0m" << std::endl;
                fs::remove(mountPoint);
            }
        } else {
            // Handle failure to create the mount point directory
            std::cerr << "\033[1;91mFailed to create mount point directory: \033[1;93m" << mountPoint << "\033[1;0m" << std::endl;
        }
    } else {
        // Handle sudo command failure or user didn't provide the password
        std::cout << " " << std::endl;
        std::cerr << "\033[1;91mFailed to authenticate with sudo.\033[1;0m" << std::endl;
    }
}


// Function to mount ISO files concurrently using asynchronous tasks
void mountISOs(const std::vector<std::string>& isoFiles) {
    // Map to store mounted ISOs with their corresponding paths
    std::map<std::string, std::string> mountedIsos;

    // Determine the number of threads to use (minimum of available threads and ISOs)
    unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
    unsigned int numThreads = std::min(maxThreads, static_cast<unsigned int>(isoFiles.size()));
    
    // Vector to store futures for parallel mounting
    std::vector<std::future<void>> futures;

    // Iterate through the list of ISO files and spawn a future for each
    for (unsigned int i = 0; i < numThreads; ++i) {
         // Create a future for mounting the ISO file and pass the map by reference
        futures.push_back(std::async(std::launch::async, [&isoFiles, &mountedIsos, i]() {
            // Lock the mutex before accessing the shared map using Mutex4Med
            std::lock_guard<std::mutex> medLock(Mutex4Med);

            // Call the function that modifies the shared map
            mountIsoFile(isoFiles[i], mountedIsos);
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
        std::cout << "\033[1;93mISO Cache is empty. Please refresh it from the main Menu Options.\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.get();
        return;
    }

    // Check if there are any ISO files to mount
    if (isoFiles.empty()) {
        std::cout << "\033[1;93mNo .iso files in the cache. Please refresh the cache from the main menu.\033[1;0m" << std::endl;
        return;
    }

    // Set to track mounted ISO files
    std::unordered_set<std::string> mountedSet;

    // Main loop for selecting and mounting ISO files
    while (true) {
        std::system("clear");
        std::cout << "\033[1;93m ! IF EXPECTED ISO FILE(S) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\n\033[1;0m" << std::endl;

        // Remove non-existent paths from the cache after selection
        removeNonExistentPathsFromCache();

        // Load ISO files from cache
        isoFiles = loadCache();

        printIsoFileList(isoFiles);

        std::cout << " " << std::endl;

        // Prompt user for input
        char* input = readline("\033[1;94mChoose ISO(s) for \033[1;92mmount\033[1;94m (e.g., '1-3', '1 2', '00' mounts all, or press Enter to return):\033[1;0m ");
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
            // Determine the number of threads to use (minimum of available threads and ISOs)
            unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

            std::vector<std::future<void>> futures;
            auto isoIterator = isoFiles.begin();
            while (isoIterator != isoFiles.end()) {
                // Wait for available semaphore slots
                for (unsigned int i = 0; i < maxThreads; ++i) {
                    if (isoIterator == isoFiles.end()) {
                        break;
                    }
                    // Use std::async to launch each handleIsoFile task in a separate thread
                    futures.emplace_back(std::async(std::launch::async, handleIsoFile, *isoIterator++, std::ref(mountedSet)));
                }
                // Wait for all launched tasks to complete
                for (auto& future : futures) {
                    future.wait();
                }
                futures.clear(); // Clear futures vector for next iteration
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
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;
        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.get();

    }
}


void printIsoFileList(const std::vector<std::string>& isoFiles) {
    // Apply formatting once before the loop
    std::cout << std::right << std::setw(2);

    // ANSI escape codes for text formatting
    const std::string defaultColor = "\033[0m";
    const std::string bold = "\033[1m";
    const std::string magenta = "\033[95m";

    for (const auto& isoFile : isoFiles) {
        std::cout << std::setw(2) << &isoFile - &isoFiles[0] + 1 << ". ";

        // Extract directory and filename
        auto [directory, filename] = extractDirectoryAndFilename(isoFile);

        // Print the directory part in the default color
        std::cout << bold << directory << defaultColor;

        // Print the filename part in magenta and bold
        std::cout << bold << "/" << magenta << filename << defaultColor << std::endl;
    }
}


// Function to handle mounting of a specific ISO file asynchronously
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet) {
    try {
		// Lock the global mutex before accessing the shared set
        std::lock_guard<std::mutex> lock(Mutex4ISO);
        // Use std::async to execute the function asynchronously
        auto future = std::async(std::launch::async, [&iso, &mountedSet]() {
            // Check if the ISO file exists on disk
            if (fileExistsOnDisk(iso)) {

                // Attempt to insert the ISO file into the set; if it's a new entry, mount it
                auto insertResult = mountedSet.insert(iso);
                if (insertResult.second) {
                    // Mount the ISO file
                    mountISOs({iso});  // Pass a vector with a single string to mountISO
                } else {
                    // ISO file is already mounted, get the mount path
                    std::string mountedIso = *insertResult.first; // Get the existing entry from the set
                    printAlreadyMountedMessage(mountedIso);
                }
            } else {
                // Display an error message if the ISO file doesn't exist on disk
                displayErrorMessage(iso);
            }
        });

        // You can choose to wait for the asynchronous operation or not based on your requirements
        // future.wait();
    } catch (const std::exception& e) {
        // Handle exceptions thrown in the asynchronous task
        std::cerr << "\033[1;91m" << e.what() << " \033[1;91m.\033[1;0m" << std::endl;
    }
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
    std::lock_guard<std::mutex> highLock(Mutex4High);
    
    unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

    std::istringstream iss(input);
    bool invalidInput = false;
    std::unordered_set<std::string> uniqueErrorMessages; // Set to store unique error messages
    std::set<int> processedIndices; // Set to keep track of processed indices
    std::set<int> validIndices; // Set to keep track of valid indices
    std::unordered_set<std::string> processedIsoNames; // Set to keep track of processed ISO names

    std::string token;
    std::vector<std::future<void>> futures; // Vector to store std::future objects for each task
    std::vector<std::thread> threads; // Vector to store threads

    unsigned int runningThreads = 0;

    while (iss >> token) {
        // Check if token consists of only zeros or is not 00
        if (token != "00" && isAllZeros(token)) {
            if (!invalidInput) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '0' does not exist.\033[1;0m");
            }
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            // Check if there is more than one hyphen in the token
            if (token.find('-', dashPos + 1) != std::string::npos) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
                continue;
            }

            int start, end;

            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[1;0m");
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'. Ensure that numbers align with the list.\033[1;0m");
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                if (runningThreads >= maxThreads) {
                    for (auto& thread : threads) {
                        thread.join();
                        runningThreads--;
                    }
                    threads.clear();
                }
                if (static_cast<size_t>(i) <= isoFiles.size() && processedIndices.find(i) == processedIndices.end()) {
                    if (processedIsoNames.find(isoFiles[i - 1]) == processedIsoNames.end()) {
                        threads.emplace_back(std::thread([&isoFiles, &mountedSet, &runningThreads, &processedIsoNames, i]() {
                            handleIsoFile(isoFiles[i - 1], mountedSet);
                            runningThreads--;
                            processedIsoNames.insert(isoFiles[i - 1]); // Mark ISO name as processed
                        }));
                    }
                    processedIndices.insert(i); // Mark as processed
                    validIndices.insert(i); // Store the valid index
                    runningThreads++;
                } else if (static_cast<size_t>(i) > isoFiles.size()) {
                    invalidInput = true;
                    uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(i) + "' does not exist.\033[1;0m");
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (runningThreads >= maxThreads) {
                for (auto& thread : threads) {
                    thread.join();
                    runningThreads--;
                }
                threads.clear();
            }
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && processedIndices.find(num) == processedIndices.end()) {
                if (processedIsoNames.find(isoFiles[num - 1]) == processedIsoNames.end()) {
                    threads.emplace_back(std::thread([&isoFiles, &mountedSet, &runningThreads, &processedIsoNames, num]() {
                        handleIsoFile(isoFiles[num - 1], mountedSet);
                        runningThreads--;
                        processedIsoNames.insert(isoFiles[num - 1]); // Mark ISO name as processed
                    }));
                }
                processedIndices.insert(num); // Mark index as processed
                validIndices.insert(num); // Store the valid index
                runningThreads++;
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                invalidInput = true;
                uniqueErrorMessages.insert("\033[1;91mFile index '" + std::to_string(num) + "' does not exist.\033[1;0m");
            }
        } else {
            invalidInput = true;
            uniqueErrorMessages.insert("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
        }
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Display errors at the end
    if (invalidInput) {
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[1;0m" << std::endl;
        }
    }
}


void printAlreadyMountedMessage(const std::string& isoFile) {
    namespace fs = std::filesystem;
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string();
    std::string mountPoint = "/mnt/iso_" + isoFileName;
    auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);
    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
    
    // Remove the ".iso" extension from the mountisoFilename
	std::string mountisoFilenameWithoutExtension = isoFilename.substr(0, isoFilename.length() - 4);

    // Check if the mount point exists
    if (fs::exists(mountPoint)) {
        std::cout << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[1;93m is already mounted at: \033[1;94m'" << mountisoDirectory << "/" << mountisoFilename << "'\033[1;93m.\033[1;0m" << std::endl;
    } else {
        // ISO file is not actually mounted at the specified mount point
        std::cerr << "\033[1;91mFailed to mount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;0m\033[1;91m.\033[1;0m" << std::endl;
    }
}


// Function to display an error message when the ISO file does not exist on disk
void displayErrorMessage(const std::string& isoFile) {
namespace fs = std::filesystem;
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string();
    auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
    
    std::cout << "\033[1;35mISO: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;35m does not exist on disk.\033[1;0m" << std::endl;
}


// UMOUNT STUFF

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
        std::cerr << "\033[1;91mError opening the /mnt directory.\033[1;0m" << std::endl;
    }

    // Display a list of mounted ISOs with ISO names in bold and magenta text
    if (!isoDirs.empty()) {
        std::cout << "\033[1mList of mounted ISO(s):\033[1;0m" << std::endl; // White and bold
        std::cout << " " << std::endl;
        for (size_t i = 0; i < isoDirs.size(); ++i) {
            std::cout << i + 1 << ". \033[1m/mnt/iso_\033[1m\033[1;95m" << isoDirs[i] << "\033[1;0m" << std::endl; // Bold and magenta
        }
    } else {
        // Print a message if no ISOs are mounted
        std::cerr << "\033[1;91mNo mounted ISO(s) found.\033[1;0m" << std::endl;
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
    // Static variable to track whether the clear has been performed
    static bool clearScreenDone = false;

    // Use std::async to unmount and remove the directory asynchronously
    auto unmountFuture = std::async(std::launch::async, [&isoDir]() {
        // Construct the sudo command
        std::string sudoCommand = "sudo -v";

        // Execute sudo to prompt for password
        int sudoResult = system(sudoCommand.c_str());

        if (sudoResult == 0) {
            // Clear the screen only if it hasn't been done yet
            if (!clearScreenDone) {
                std::system("clear");
                clearScreenDone = true;
            }

            // Construct the unmount command with sudo, umount, and suppressing logs
            std::string command = "sudo umount -l " + shell_escape(isoDir) + " > /dev/null 2>&1";
            int result = system(command.c_str());
            
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            // Remove the ".iso" extension from the mountisoFilename
			std::string isoFilenameWithoutExtension = isoFilename.substr(0, isoFilename.length() - 4);
            
            
            if (isDirectoryEmpty(isoDir) && result != 0) {
                    // Construct the remove directory command with sudo, rmdir, and suppressing logs
                    command = "sudo rmdir " + shell_escape(isoDir) + " 2>/dev/null";
                    std::cout << "\033[1;92mRemoved empty directory: \033[1;91m'" << isoDirectory << "/" << isoFilename << "'\033[1;92m.\033[1;0m" << std::endl; // Print success message
				}
				// Check if the unmounting was successful
				else if (result == 0) {
                std::cout << "\033[1mUnmounted: \033[1;92m'" << isoDirectory << "/" << isoFilename << "'\033[1;0m." << std::endl; // Print success message

					// Check if the directory is empty before removing it
					if (isDirectoryEmpty(isoDir)) {
						// Construct the remove directory command with sudo, rmdir, and suppressing logs
						command = "sudo rmdir " + shell_escape(isoDir) + " 2>/dev/null";
						int removeDirResult = system(command.c_str());

						if (removeDirResult != 0) {
							std::cerr << "\033[1;91mFailed to remove directory: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;91m ...Please check it out manually.\033[1;0m" << std::endl;
						}
					}
				} else {
					// Print failure message
					std::cerr << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "'\033[1;91m ...Probably not an ISO mountpoint, check it out manually.\033[1;0m" << std::endl;
				}
			} else {
				// Print failure message
				std::cout << " " << std::endl;
				std::cerr << "\033[1;91mFailed to authenticate with sudo.\033[1;0m" << std::endl;
			}
		});

    // Wait for the asynchronous tasks to complete
   // unmountFuture.get();
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
    std::set<int> validIndices;

    bool invalidInput = false;

    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    while (true) {
        // Display the initial list of mounted ISOs
        listMountedISOs();

        std::vector<std::string> isoDirs;
        std::vector<std::string> errorMessages;  // Store error messages

        // Reset flags and sets for each iteration
        invalidInput = false;
        uniqueErrorMessages.clear();

        // Find and store directories with the name "iso_*" in /mnt using std::filesystem
        for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
            if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                isoDirs.push_back(entry.path().string());
            }
        }

        // Check if there are no mounted ISOs
        if (isoDirs.empty()) {
            std::cout << " " << std::endl;
            std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
            std::cin.get(); // Wait for the user to press Enter
            return;
        }

        // Check if there are mounted ISOs and add a newline
        if (!isoDirs.empty()) {
            std::cout << " " << std::endl;
        }

        // Prompt for unmounting input
        char* input = readline("\033[1;94mChoose ISO(s) for \033[1;92munmount\033[1;94m (e.g., '1-3', '1 2', '00' unmounts all, or press Enter to return):\033[1;0m ");
        std::system("clear");

        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input[0] == '\0') {
            break;  // Exit the loop
        }

        if (std::strcmp(input, "00") == 0) {
            // Detect and use the minimum of available threads and ISOs to ensure efficient parallelism fallback is two
            unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

            // Use the minimum of available threads and ISOs to ensure efficient parallelism
            unsigned int numThreads = std::min(static_cast<unsigned int>(isoDirs.size()), maxThreads);

            // Create a vector of threads to store the unmounting threads
            std::vector<std::thread> threads;

            for (const std::string& isoDir : isoDirs) {
                threads.emplace_back([&, isoDir = std::move(isoDir)]() {
                    std::lock_guard<std::mutex> highLock(Mutex4High); // Lock the critical section
                    unmountISO(isoDir);
                });

                // Limit the number of active threads to the available hardware threads
                if (threads.size() >= numThreads) {
                    // Join the threads to wait for them to finish
                    for (auto& thread : threads) {
                        thread.join();
                    }
                    threads.clear(); // Clear the vector for the next batch of threads
                }
            }

            // Join the remaining threads
            for (auto& thread : threads) {
                thread.join();
            }

            // Stop the timer after completing the unmounting process
            auto end_time = std::chrono::high_resolution_clock::now();

            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            // Print the time taken for the entire process in bold with one decimal place
            std::cout << " " << std::endl;
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;

            std::cout << " " << std::endl;
            std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
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
            // Check if token consists of only zeros or is not 00
            if (token != "00" && isAllZeros(token)) {
                if (!invalidInput) {
                    invalidInput = true;
                }
            }
            // Check if the token is a valid number
            if (std::regex_match(token, std::regex("^\\d+$"))) {
                // Individual number
                int number = std::stoi(token);
                if (isValidIndex(number, isoDirs.size())) {
                    // Check for duplicates
                    if (uniqueIndices.find(number) == uniqueIndices.end()) {
                        uniqueIndices.insert(number);
                        validIndices.insert(number);
                        unmountIndices.push_back(number);
                    }

                } else {
                    // Store the error message
                    errorMessages.push_back("\033[1;91mFile index '" + std::to_string(number) + "' does not exist.\033[1;0m");
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
                            validIndices.insert(startRange);
                            unmountIndices.push_back(startRange);
                        }
                    } else {
                        // Check if the error message for the invalid index is already stored
                        std::string errorMessage = "\033[1;91mFile index '" + std::to_string(startRange) + "' does not exist.\033[1;0m";

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
                                validIndices.insert(i);
                                unmountIndices.push_back(i);
                            }
                        }
                    } else {
                        // Store the error message for invalid range
                        errorMessages.push_back("\033[1;91mInvalid range: '" + token + "'. Ensure that numbers align with the list.\033[1;0m");
                    }
                }
            } else {
                // Store the error message for invalid input format
                errorMessages.push_back("\033[1;91mInvalid input: '" + token + "'.\033[1;0m");
            }
        }

        // Create a vector of threads to perform unmounting and directory removal concurrently
        std::vector<std::thread> threads;

        // Detect and use the minimum of available threads and ISOs to ensure efficient parallelism fallback is two
        unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;
        unsigned int numThreads = std::min(static_cast<unsigned int>(isoDirs.size()), maxThreads);

        for (int index : unmountIndices) {
            // Check if the index is within the valid range
            if (isValidIndex(index, isoDirs.size())) {
                const std::string& isoDir = isoDirs[index - 1];
                
                // Use a thread for each ISO to be unmounted
                threads.emplace_back([&, isoDir = std::move(isoDir)]() {
                    std::lock_guard<std::mutex> highLock(Mutex4High); // Lock the critical section
                    unmountISO(isoDir);
 
                });

                // Limit the number of active threads to the available hardware threads
                if (threads.size() >= numThreads) {
                    // Join the threads to wait for them to finish
                    for (auto& thread : threads) {
                        thread.join();
                    }
                    threads.clear(); // Clear the vector for the next batch of threads
                }
            }
        }

        // Join the remaining threads
        for (auto& thread : threads) {
            thread.join();
        }

        // Stop the timer after completing the unmounting process
        auto end_time = std::chrono::high_resolution_clock::now();
		
		if (invalidInput && !validIndices.empty()) {
		std::cout << " " << std::endl;	
	}
		
        // Print error messages
        for (const auto& errorMessage : errorMessages) {
            if (uniqueErrorMessages.find(errorMessage) == uniqueErrorMessages.end()) {
                // If not found, store the error message and print it
                uniqueErrorMessages.insert(errorMessage);
                std::cerr << "\033[1;93m" << errorMessage << "\033[1;0m" << std::endl;
            }
        }

        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << " " << std::endl;
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[1;0m" << std::endl;

        std::cout << " " << std::endl;
        std::cout << "\033[1;32mPress enter to continue...\033[1;0m";
        std::cin.get();
        std::system("clear");
    }
}
