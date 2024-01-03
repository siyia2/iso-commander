#include "sanitization_readline.h"
#include "conversion_tools.h"

// Cache Variables \\

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "iso_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB


// MULTITHREADING STUFF
std::mutex mountMutex; // Mutex for thread safety
std::mutex mtx;

namespace fs = std::filesystem;

//	Function prototypes	\\

//	bools

bool fileExists(const std::string& path);
bool directoryExists(const std::string& path);
bool allDirectoriesExistOnDisk(const std::vector<std::string>& directories);
bool allFilesExistAndAreIso(const std::vector<std::string>& files);
bool isValidDirectory(const std::string& path);
bool isDirectoryEmpty(const std::string& path);
bool iequals(std::string_view a, std::string_view b);
bool allSelectedFilesExistOnDisk(const std::vector<std::string>& selectedFiles);
bool fileExists(const std::string& path);
bool parallelFileExistsOnDisk(const std::vector<std::string>& filenames);
bool ends_with_iso(const std::string& str);
bool isNumeric(const std::string& str);
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);

//	voids

void listMountedISOs();
void unmountISOs();
void cleanAndUnmountAllISOs();
void select_and_mount_files_by_number();
void print_ascii();
void manualRefreshCache();
void mountISO(const std::vector<std::string>& isoFiles);
void unmountISO(const std::string& isoDir);
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& mtx);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void removeNonExistentPathsFromCacheWithOpenMP();
void displayErrorMessage(const std::string& iso);
void printAlreadyMountedMessage(const std::string& iso);
void printIsoFileList(const std::vector<std::string>& isoFiles);
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet);
void processInputMultithreaded(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet);
void processPath(const std::string& path, std::vector<std::string>& allIsoFiles);

//	stds

std::vector<std::string> vec_concat(const std::vector<std::string>& v1, const std::vector<std::string>& v2);
std::vector<bool> parallelIsDirectoryEmpty(const std::vector<std::string>& paths);
std::vector<bool> parallelIsNumeric(const std::vector<std::string>& strings);
std::vector<bool> parallelEndsWithIso(const std::vector<std::string>& strings);
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
        std::cout << "Menu Options:" << std::endl;
        std::cout << "1. Mount" << std::endl;
        std::cout << "2. Unmount" << std::endl;
        std::cout << "3. Conversion Tools" << std::endl;
        std::cout << "4. Refresh ISO Cache" << std::endl;
        std::cout << "5. List Mountpoints" << std::endl;
        std::cout << "6. Exit Program" << std::endl;

        // Prompt for the main menu choice
        //std::cin.clear();

        std::cout << " " << std::endl;
        char* input = readline("\033[94mEnter a choice:\033[0m ");
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
                while (!returnToMainMenu) {
					std::system("clear");
                    std::cout << "1. Convert to ISO (BIN2ISO)" << std::endl;
                    std::cout << "2. Convert to ISO (MDF2ISO)" << std::endl;
                    std::cout << " " << std::endl;
                    char* submenu_input = readline("\033[94mChoose a function, or press Enter to return:\033[0m ");

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
            case '4':
                manualRefreshCache();
                std::cout << "Press Enter to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::system("clear");
                break;
            case '5':
				std::cout << " " << std::endl;
                listMountedISOs();
                std::cout << " " << std::endl;
                std::cout << "Press Enter to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::system("clear");
                break;
            case '6':
                exitProgram = true; // Exit the program
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

    const char* greenColor = "\x1B[32m";
    const char* resetColor = "\x1B[0m"; // Reset color to default

    std::cout << greenColor << R"( _____            ___  _____  _____     ___  __   __   ___   _____  _____     __   __   ___  __   __  _   _  _____  _____  ____         ____
|  ___)    /\    (   )(_   _)|  ___)   (   )|  \ /  | / _ \ |  ___)|  ___)   |  \ /  | / _ \(_ \ / _)| \ | |(_   _)|  ___)|  _ \       (___ \     _      _   
| |_      /  \    | |   | |  | |_       | | |   v   || |_| || |    | |_      |   v   || | | | \ v /  |  \| |  | |  | |_   | |_) )  _  __ __) )  _| |_  _| |_ 
|  _)    / /\ \   | |   | |  |  _)      | | | |\_/| ||  _  || |    |  _)     | |\_/| || | | |  | |   |     |  | |  |  _)  |  __/  | |/ // __/  (_   _)(_   _)
| |___  / /  \ \  | |   | |  | |___     | | | |   | || | | || |    | |___    | |   | || |_| |  | |   | |\  |  | |  | |___ | |     | / /| |___    |_|    |_|  
|_____)/_/    \_\(___)  |_|  |_____)   (___)|_|   |_||_| |_||_|    |_____)   |_|   |_| \___/   |_|   |_| \_|  |_|  |_____)|_|     |__/ |_____)               
                                                                                                                                                                  )" << resetColor << '\n';

}

//	CACHE STUFF \\


// Function to check if a file or directory exists
bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}


// Function to remove non existent paths from cache
void removeNonExistentPathsFromCacheWithOpenMP() {
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

    // Determine the size of the cache file and reserve space in the cache vector
    cacheFile.seekg(0, std::ios::end);
    cache.reserve(cacheFile.tellg());
    cacheFile.seekg(0, std::ios::beg);

    // Read paths from the cache file into the cache vector
    for (std::string line; std::getline(cacheFile, line);) {
        cache.push_back(line);
    }

    // Close the cache file
    cacheFile.close();

    // Create a vector to hold private paths for each OpenMP thread
    std::vector<std::vector<std::string>> privatePaths(omp_get_max_threads());

    // Parallel loop to check the existence of paths and distribute them among threads
    #pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < cache.size(); i++) {
        if (fileExists(cache[i])) {
            // Add existing paths to the private vector of the current thread
            privatePaths[omp_get_thread_num()].push_back(cache[i]);
        }
    }

    // Combine private paths into a single shared vector
    std::vector<std::string> retainedPaths;
    retainedPaths.reserve(cache.size()); // Reserve space to avoid reallocations

    for (const auto& privatePath : privatePaths) {
        retainedPaths.insert(retainedPaths.end(), privatePath.begin(), privatePath.end());
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


// Function to check if a file or directory exists using OpenMP
bool exists(const std::filesystem::path& path) {
    bool result = false;

    #pragma omp parallel for
    for (int i = 0; i < omp_get_max_threads(); ++i) {
        // Each thread checks the existence independently
        if (std::filesystem::exists(path)) {
            #pragma omp atomic write
            result = true;
        }
    }

    return result;
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


// Check if all selected files are still present on disk
bool allSelectedFilesExistOnDisk(const std::vector<std::string>& selectedFiles) {
    bool allExist = true;

    #pragma omp parallel for reduction(&&:allExist) num_threads(omp_get_max_threads())
    for (int i = 0; i < selectedFiles.size(); ++i) {
        if (!std::filesystem::exists(selectedFiles[i])) {
            // If a file is not found, update the reduction variable
            allExist = false;
        }
    }

    return allExist;
}


// Function to refresh the cache for a single directory
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles) {
	std::cout << "\033[93mProcessing directory path: '" << path << "'.\033[0m" << std::endl;
    std::vector<std::string> newIsoFiles;
    
    // Perform the cache refresh for the directory (e.g., using parallelTraverse)
    parallelTraverse(path, newIsoFiles, mtx);
    
    // Lock the mutex to protect the shared 'allIsoFiles' vector
    std::lock_guard<std::mutex> lock(mtx);
    
    // Append the new entries to the shared vector
    allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());
    
    std::cout << "\033[92mProcessed directory path: '" << path << "'.\033[0m" << std::endl;
}


// Function to check if a directory is valid using OpenMP
bool isValidDirectory(const std::string& path) {
    bool result = false;

    #pragma omp parallel shared(result)
    {
        #pragma omp for
        for (int i = 0; i < omp_get_max_threads(); ++i) {
            if (std::filesystem::is_directory(path)) {
                #pragma omp critical
                {
                    result = true;
                }
            }
        }
    }

    return result;
}


// Function for manual cache refresh
void manualRefreshCache() {
    // Clear the console screen
    std::system("clear");

    // Prompt the user to enter directory paths for manual cache refresh
    std::string inputLine = readInputLine("\033[94mEnter the directory path(s) from which to populate the \033[1m\033[92mISO Cache\033[94m (if many, separate them with \033[1m\033[93m;\033[0m\033[94m), or press Enter to cancel:\n\033[0m");

    // Check if the user canceled the cache refresh
    if (inputLine.empty()) {
        std::cout << "\033[93mCache refresh canceled by user.\033[0m" << std::endl;
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

    // Vector to store threads for parallel cache refreshing
    std::vector<std::thread> threads;

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

        // Create a thread for refreshing the cache for each directory and pass the vector by reference
        threads.emplace_back(std::thread(refreshCacheForDirectory, path, std::ref(allIsoFiles)));
    }

    // Wait for all threads to finish
    for (std::thread& t : threads) {
        t.join();
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


//	MOUNT STUFF	\\

// Function to check if a directory exists at the specified path using OpenMP
bool directoryExists(const std::string& path) {
    bool result = false;

    #pragma omp parallel for
    for (int i = 0; i < omp_get_max_threads(); ++i) {
        // Each thread checks the existence independently
        if (std::filesystem::is_directory(path)) {
            #pragma omp atomic write
            result = true;
        }
    }

    return result;
}


// Function to check if all directories in a vector exist on disk
bool allDirectoriesExistOnDisk(const std::vector<std::string>& directories) {
    // Flag to track whether all directories exist
    bool allExist = true;

    // Use OpenMP to parallelize the loop for checking directory existence
    #pragma omp parallel for reduction(&&:allExist) num_threads(omp_get_max_threads())
    for (int i = 0; i < directories.size(); ++i) {
        // Check if the directory at the current index exists
        if (!directoryExists(directories[i])) {
            // If not, update the reduction variable
            allExist = false;
        }
    }

    // Return the final result indicating whether all directories exist
    return allExist;
}


void mountIsoFile(const std::string& isoFile, std::map<std::string, std::string>& mountedIsos) {
    // Check if the ISO file is already mounted
    std::lock_guard<std::mutex> lock(mountMutex); // Lock to protect access to mountedIsos

    // Use the filesystem library to extract the ISO file name
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string(); // Remove the .iso extension

    std::string mountPoint = "/mnt/iso_" + isoFileName; // Use the modified ISO file name in the mount point with "iso_" prefix

    // Check if the mount point directory doesn't exist, create it
    if (!directoryExists(mountPoint)) {
        // Create the mount point directory
        std::string mkdirCommand = "sudo mkdir " + shell_escape(mountPoint);
        if (system(mkdirCommand.c_str()) != 0) {
            std::cerr << "\033[91mFailed to create mount point directory\033[0m" << std::endl;
            return;
        }

        // Mount the ISO file to the mount point
        std::string mountCommand = "sudo mount -o loop " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
        int mountResult = system(mountCommand.c_str());
        if (mountResult != 0) {
            std::cerr << "\033[91mFailed to mount: " << isoFile << "\033[0m" << std::endl;

            // Cleanup the mount point directory
            std::string cleanupCommand = "sudo rmdir " + shell_escape(mountPoint);
            if (system(cleanupCommand.c_str()) != 0) {
                std::cerr << "\033[91mFailed to clean up mount point directory\033[0m" << std::endl;
            }

            return;
        } else {
            // Store the mount point in the map
            mountedIsos[isoFile] = mountPoint;
            std::cout << "\033[92mMounted at: " << mountPoint << "\033[0m" << std::endl;
        }
    } else {
        // The mount point directory already exists, so the ISO is considered mounted
        mountedIsos[isoFile] = mountPoint;
        std::cout << "\033[93mISO file '" << isoFile << "' is already mounted at '" << mountPoint << "'.\033[m" << std::endl;
    }
}


// Function to mount ISO files concurrently using threads
void mountISO(const std::vector<std::string>& isoFiles) {
    // Map to store mounted ISOs with their corresponding paths
    std::map<std::string, std::string> mountedIsos;

    // Vector to store threads for parallel mounting
    std::vector<std::thread> threads;

    // Iterate through the list of ISO files and spawn a thread for each
    for (const std::string& isoFile : isoFiles) {
        // Create a copy of the ISO file path for the thread to avoid race conditions
        std::string IsoFile = (isoFile);

        // Create a thread for mounting the ISO file and pass the map by reference
        threads.emplace_back(mountIsoFile, IsoFile, std::ref(mountedIsos));
    }

    // Join all threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }
}


// Function to check if a file exists on disk
bool fileExistsOnDisk(const std::string& filename) {
    // Use an ifstream to check the existence of the file
    std::ifstream file(filename);
    return file.good();
}


// Function to check file existence using multiple threads
bool parallelFileExistsOnDisk(const std::vector<std::string>& filenames) {
    bool exists = true;

    // Parallelize the file existence check using OpenMP
    #pragma omp parallel for reduction(&&:exists) num_threads(omp_get_max_threads())
    for (int i = 0; i < static_cast<int>(filenames.size()); ++i) {
        if (!fileExistsOnDisk(filenames[i])) {
            // If any file does not exist, set the exists flag to false
            #pragma omp atomic write
            exists = false;
        }
    }

    return exists;
}


// Function to check if a string ends with ".iso" (case-insensitive)
bool ends_with_iso(const std::string& str) {
    // Convert the string to lowercase for a case-insensitive comparison
    std::string lowercase = str;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), ::tolower);

    // Check if the lowercase string ends with ".iso"
    return lowercase.size() >= 4 && lowercase.compare(lowercase.size() - 4, 4, ".iso") == 0;
}


// Function to check if multiple strings end with ".iso" using multiple threads
std::vector<bool> parallelEndsWithIso(const std::vector<std::string>& strings) {
    std::vector<bool> results(strings.size(), false);

    // Parallelize the ends_with_iso check using OpenMP
    #pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < static_cast<int>(strings.size()); ++i) {
        results[i] = ends_with_iso(strings[i]);
    }

    return results;
}


// Function to check if all files in a vector exist on disk and have the ".iso" extension
bool allFilesExistAndAreIso(const std::vector<std::string>& files) {
    // Flag to track whether all files exist and have the ".iso" extension
    bool allExistAndIso = true;

    // Use OpenMP to parallelize the loop for checking file existence and extension
    #pragma omp parallel for reduction(&&:allExistAndIso) num_threads(omp_get_max_threads())
    for (int i = 0; i < files.size(); ++i) {
        // Check if the file exists on disk and has the ".iso" extension
        if (!fileExistsOnDisk(files[i]) || !ends_with_iso(files[i])) {
            // If not, update the reduction variable
            allExistAndIso = false;
        }
    }

    // Return the final result indicating whether all files meet the criteria
    return allExistAndIso;
}

int customNewline(int count, int key) {
    // Do nothing or perform your specific actions without adding to history
    return 0;  // Return 0 to indicate success
}

// Function to select and mount ISO files by number
void select_and_mount_files_by_number() {
    // Remove non-existent paths from the cache
    removeNonExistentPathsFromCacheWithOpenMP();

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
        std::cout << "\033[93m! IF EXPECTED ISO FILE(s) NOT ON THE LIST REFRESH ISO CACHE FROM THE MAIN MENU OPTIONS !\n\033[0m" << std::endl;
        printIsoFileList(isoFiles);
        
		std::cout << " " << std::endl;
		
        // Prompt user for input
        char* input = readline("\033[94mChoose ISO(s) to mount (e.g., '1-3', '1 2', '00' mounts all, or press Enter to return):\033[0m ");
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
            processInputMultithreaded(input, isoFiles, mountedSet);
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


// Function to print ISO file list with filename in green
void printIsoFileList(const std::vector<std::string>& isoFiles) {

    for (std::size_t i = 0; i < isoFiles.size(); ++i) {
        std::cout << std::setw(2) << std::right << i + 1 << ". ";

        // Print the directory part in the default color
        std::size_t lastSlashPos = isoFiles[i].find_last_of('/');
        if (lastSlashPos != std::string::npos) {
            std::cout << isoFiles[i].substr(0, lastSlashPos + 1);
        }

        // Print the filename part in magenta and bold
        std::cout << "\033[1m\033[95m" << isoFiles[i].substr(lastSlashPos + 1) << "\033[0m" << std::endl;
    }
}


// Function to handle mounting of a specific ISO file
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet) {
    // Check if the ISO file exists on disk
    if (fileExistsOnDisk(iso)) {
        // Attempt to insert the ISO file into the set; if it's a new entry, mount it
        if (mountedSet.insert(iso).second) {
            mountISO({iso});
        } else {
            // Print a message if the ISO file is already mounted
            printAlreadyMountedMessage(iso);
        }
    } else {
        // Display an error message if the ISO file doesn't exist on disk
        displayErrorMessage(iso);
    }
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    for (char c : str) {
        if (!std::isdigit(c)) {
            return false;
        }
    }
    return true;
}


// Function to check if multiple strings are numeric using multiple threads
std::vector<bool> parallelIsNumeric(const std::vector<std::string>& strings) {
    std::vector<bool> results(strings.size(), false);

    // Parallelize the isNumeric check using OpenMP
    #pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < static_cast<int>(strings.size()); ++i) {
        results[i] = isNumeric(strings[i]);
    }

    return results;
}


// Function to process the user input for ISO mounting using multithreading
void processInputMultithreaded(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet) {
    std::istringstream iss(input);
    bool invalidInput = false;
    std::vector<std::string> errorMessages; // Vector to store error messages

    std::string token;
    std::vector<std::future<void>> futures; // Vector to store std::future objects for each task

    while (iss >> token) {
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
                if (static_cast<size_t>(i) <= isoFiles.size()) {
                    // Use std::async to launch each task in a separate thread
                    futures.emplace_back(std::async(std::launch::async, handleIsoFile, isoFiles[i - 1], std::ref(mountedSet)));
                } else {
                    invalidInput = true;
                    errorMessages.push_back("\033[91mFile index '" + std::to_string(i) + "' does not exist.\033[0m");
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                // Use std::async to launch each task in a separate thread
                futures.emplace_back(std::async(std::launch::async, handleIsoFile, isoFiles[num - 1], std::ref(mountedSet)));
            } else {
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


// Function to print a message indicating that the ISO file is already mounted
void printAlreadyMountedMessage(const std::string& iso) {
    std::cout << "\033[93mISO file '" << iso << "' is already mounted.\033[0m" << std::endl;
}


// Function to display an error message when the ISO file does not exist on disk
void displayErrorMessage(const std::string& iso) {
    std::cout << "\033[35mISO file '" << iso << "' does not exist on disk. Please return and re-enter the mount function, or refresh the cache from the main menu.\033[0m" << std::endl;
}


// Function to perform case-insensitive string comparison
bool iequals(std::string_view a, std::string_view b) {
    // Check if the string lengths are equal
    if (a.size() != b.size()) {
        return false;
    }

    // Flag to track equality, initialized to true
    bool equal = true;

    // Use OpenMP to parallelize the loop for case-insensitive comparison
    #pragma omp parallel for reduction(&&:equal) num_threads(omp_get_max_threads())
    for (std::size_t i = 0; i < a.size(); ++i) {
        // Check if characters are not equal (case-insensitive)
        if (std::tolower(a[i]) != std::tolower(b[i])) {
            // Set the equal flag to false and exit the loop
            equal = false;
        }
    }

    return equal;
}


// Function to parallel traverse a directory and find ISO files
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& mtx) {
    try {
        // Vector to store futures for asynchronous tasks
        std::vector<std::future<void>> futures;

        // Iterate through the directory and its subdirectories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            // Check if the entry is a regular file
            if (entry.is_regular_file()) {
                // Get the path of the file
                const std::filesystem::path& filePath = entry.path();

                // Skip empty files or files with ".bin" extension
                if (std::filesystem::file_size(filePath) == 0 || iequals(filePath.stem().string(), ".bin")) {
                    continue;
                }

                // Get the file extension as a string and string view
                std::string extensionStr = filePath.extension().string();
                std::string_view extension = extensionStr;

                // Check if the file has a ".iso" extension
                if (iequals(extension, ".iso")) {
                    // Use async to run the task of collecting ISO paths in parallel
                    futures.push_back(std::async(std::launch::async, [filePath, &isoFiles, &mtx]() {
                        // Process the file content as needed
                        // For example, you can check for ISO file signatures, etc.

                        // Lock the mutex to update the shared vector
                        std::lock_guard<std::mutex> lock(mtx);
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


// Function to process a directory path, find ISO files in parallel, and update the shared vector
void processPath(const std::string& path, std::vector<std::string>& allIsoFiles) {
    // Inform about the directory path being processed
    std::cout << "Processing directory path: " << path << std::endl;

    // Vector to store ISO files found in the current directory
    std::vector<std::string> newIsoFiles;

    // Call parallelTraverse to asynchronously find ISO files in the directory
    parallelTraverse(path, newIsoFiles, mtx);

    // Lock the mutex to safely update the shared vector of all ISO files
    std::lock_guard<std::mutex> lock(mtx);
    
    // Merge the new ISO files into the shared vector
    allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());

    // Inform that the cache has been refreshed for the processed directory
    std::cout << "Cache refreshed for directory: " << path << std::endl;
}


// UMOUNT FUNCTIONS	\\

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
        for (size_t i = 0; i < isoDirs.size(); ++i) {
            std::cout << i + 1 << ". \033[1m\033[95m" << isoDirs[i] << "\033[0m" << std::endl; // Bold and magenta
        }
    } else {
        // Print a message if no ISOs are mounted
        std::cerr << "\033[91mNo mounted ISO(s) found.\033[0m" << std::endl;
    }
}


// Function to check if a directory is empty
bool isDirectoryEmpty(const std::string& path) {
    std::string checkEmptyCommand = "sudo find " + shell_escape(path) + " -mindepth 1 -maxdepth 1 -print -quit | grep -q .";
    int result = system(checkEmptyCommand.c_str());
    return result != 0; // If result is 0, directory is empty; otherwise, it's not empty
}

// Function to check if multiple directories are empty using multiple threads
std::vector<bool> parallelIsDirectoryEmpty(const std::vector<std::string>& paths) {
    std::vector<bool> results(paths.size(), false);

    // Parallelize the isDirectoryEmpty check using OpenMP
    #pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
        results[i] = isDirectoryEmpty(paths[i]);
    }

    return results;
}


void unmountISO(const std::string& isoDir) {
    // Construct the unmount command with sudo, umount, and suppressing logs
    std::string unmountCommand = "sudo umount -l " + shell_escape(isoDir) + " > /dev/null 2>&1";

    // Execute the unmount command
    int result = system(unmountCommand.c_str());

    // Check if the unmounting was successful
    if (result == 0) {
        std::cout << "\033[92mUnmounted: " << isoDir << "\033[0m" << std::endl; // Print success message

        // Check if the directory is empty before removing it
        if (isDirectoryEmpty(isoDir)) {
            // Construct the remove directory command with sudo, rmdir, and suppressing logs
            std::string removeDirCommand = "sudo rmdir " + shell_escape(isoDir) + " 2>/dev/null";

            // Execute the remove directory command
            int removeDirResult = system(removeDirCommand.c_str());

            if (removeDirResult != 0) {
                std::cerr << "\033[91mFailed to remove directory: " << isoDir << " ...Please check it out manually.\033[0m" << std::endl;
            }
        }
    } else {
        std::cerr << "\033[91mFailed to unmount: " << isoDir << " ...Probably not an ISO mountpoint, check it out manually.\033[0m" << std::endl; // Print failure message
    }
}


// Function to check if a given index is within the valid range of available ISOs
bool isValidIndex(int index, size_t isoDirsSize) {
    return (index >= 1) && (static_cast<size_t>(index) <= isoDirsSize);
}

// Function to unmount ISOs based on user input
void unmountISOs() {
    listMountedISOs(); // Display the initial list of mounted ISOs

    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    while (true) {
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
        char* input = readline("\033[94mChoose ISO(s) to unmount (e.g. '1-3', '1 2', '00' unmounts all, or press Enter to return):\033[0m ");
        std::system("clear");

        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input[0] == '\0') {
            break;  // Exit the loop
        }

        if (std::strcmp(input, "00") == 0) {
            // Unmount all ISOs
            for (const std::string& isoDir : isoDirs) {
                std::lock_guard<std::mutex> lock(mtx); // Lock the critical section
                unmountISO(isoDir);
            }
            // Stop the timer after completing the mounting process
            auto end_time = std::chrono::high_resolution_clock::now();

            auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
            // Print the time taken for the entire process in bold with one decimal place
            std::cout << " " << std::endl;
            std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

            std::cout << " " << std::endl;
            std::cout << "Press Enter to continue...";
            std::cin.get();
            std::system("clear");

            listMountedISOs(); // Display the updated list of mounted ISOs after unmounting all

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
                        // Store the error message for invalid index
                        errorMessages.push_back("\033[91mFile index '" + std::to_string(startRange) + "' does not exist.\033[0m");
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
                    std::lock_guard<std::mutex> lock(mtx); // Lock the critical section
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
            std::cerr << "\033[93m" << errorMessage << "\033[0m" << std::endl;
        }

        auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
        // Print the time taken for the entire process in bold with one decimal place
        std::cout << " " << std::endl;
        std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

        std::cout << " " << std::endl;
        std::cout << "Press Enter to continue...";
        std::cin.get();
        std::system("clear");

        listMountedISOs(); // Display the updated list of mounted ISOs after unmounting
    }
}
