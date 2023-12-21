#include "sanitization_readline.h"
#include "conversion_tools.h"

// Cache Varables \\

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "iso_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB


// MULTITHREADING STUFF
std::mutex mountMutex; // Mutex for thread safety
std::mutex mtx;

namespace fs = std::filesystem;

//	Function prototypes	\\

//	bools

bool directoryExists(const std::string& path);
bool iequals(std::string_view a, std::string_view b);
bool allSelectedFilesExistOnDisk(const std::vector<std::string>& selectedFiles);
bool fileExists(const std::string& path);
bool isNumeric(const std::string& str);

//	voids

void mountISO(const std::vector<std::string>& isoFiles);
void listAndMountISOs();
void unmountISO(const std::string& isoDir);
void listMountedISOs();
void unmountISOs();
void cleanAndUnmountAllISOs();
void listMode();
void select_and_mount_files_by_number();
void print_ascii();
void screen_clear();
void print_ascii();
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::mutex& mtx);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void manualRefreshCache();
void removeNonExistentPathsFromCacheWithOpenMP();
void displayErrorMessage(const std::string& iso);
void printAlreadyMountedMessage(const std::string& iso);
void printIsoFileList(const std::vector<std::string>& isoFiles);
void handleIsoFile(const std::string& iso, std::unordered_set<std::string>& mountedSet);
void processInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet);


std::string directoryPath;					// Declare directoryPath here

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
        free(input);

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
                    std::cout << "3. Back to Main Menu" << std::endl;
                    std::cout << " " << std::endl;
                    char* submenu_input = readline("\033[94mEnter a choice:\033[0m ");

                    if (!submenu_input) {
                        break; // Exit the submenu if readline returns NULL
                    }
					
                    std::string submenu_choice(submenu_input);
                    free(submenu_input);
                    // Check if the input length is exactly 1
					if (submenu_choice.length() == 1){
                    switch (submenu_choice[0]) {		
                    case '1':
                        std::system("clear");
                        select_and_convert_files_to_iso();
                        break;
                    case '2':
                        std::system("clear");
                        select_and_convert_files_to_iso_mdf();
                        break;
                    case '3':
                        returnToMainMenu = true;  // Set the flag to return to the main menu
                        break; // Go back to the main menu
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
        std::cerr << "\033[31mError: Unable to find cache file, will attempt to create it.\033[0m" << std::endl;
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
        std::cerr << "\033[31mError: Unable to open cache file for writing, check permissions.\033[0m" << std::endl;
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


// Save cache
void saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize) {
    std::filesystem::path cachePath = cacheDirectory;
    cachePath /= cacheFileName;

    // Load the existing cache
    std::vector<std::string> existingCache = loadCache();

    // Append new and unique entries to the existing cache
    for (const std::string& iso : isoFiles) {
        if (std::find(existingCache.begin(), existingCache.end(), iso) == existingCache.end()) {
            existingCache.push_back(iso);
        }
    }

    // Limit the cache size to the maximum allowed size
    while (existingCache.size() > maxCacheSize) {
        existingCache.erase(existingCache.begin());
    }

    // Open the cache file in write mode (truncating it)
    std::ofstream cacheFile(cachePath);
    if (cacheFile.is_open()) {
        for (const std::string& iso : existingCache) {
            cacheFile << iso << "\n";
        }
        cacheFile.close();
    } else {
        std::cerr << "Error: Could not open cache file for writing." << std::endl;
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
	std::cout << "\033[33mProcessing directory path: '" << path << "'\033[0m" << std::endl;
    std::vector<std::string> newIsoFiles;
    
    // Perform the cache refresh for the directory (e.g., using parallelTraverse)
    parallelTraverse(path, newIsoFiles, mtx);
    
    // Lock the mutex to protect the shared 'allIsoFiles' vector
    std::lock_guard<std::mutex> lock(mtx);
    
    // Append the new entries to the shared vector
    allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());
    
    std::cout << "\033[32mCache refreshed for directory: '" << path << "'\033[0m" << std::endl;
}


// Function for manual cache refresh
void manualRefreshCache() {
    std::system("clear");
    
    // Prompt the user to enter directory paths for manual cache refresh
    std::string inputLine = readInputLine("\033[94mEnter the directory path(s) from which to populate the cache (if many, separate them by \033[33m;\033[0m\033[94m), or press Enter to cancel:\n\033[0m");
    std::cout << " " << std::endl;

    // Start the timer
    auto start_time = std::chrono::high_resolution_clock::now();

    // Check if the user canceled the cache refresh
    if (inputLine.empty()) {
        std::cout << "\033[33mCache refresh canceled.\033[0m" << std::endl;
        return;
    }

    // Create an input string stream to parse directory paths
    std::istringstream iss(inputLine);
    std::string path;

    // Vector to store all ISO files from multiple directories
    std::vector<std::string> allIsoFiles;

    // Vector to store threads for parallel cache refreshing
    std::vector<std::thread> threads;

    // Iterate through the entered directory paths
    while (std::getline(iss, path, ';')) {
        // Create a thread for refreshing the cache for each directory and pass the vector by reference
        threads.emplace_back(std::thread(refreshCacheForDirectory, path, std::ref(allIsoFiles)));
    }

    // Wait for all threads to finish
    for (std::thread& t : threads) {
        t.join();
    }

    // Save the combined cache to disk
    saveCache(allIsoFiles, maxCacheSize);

    // Stop the timer after completing the cache refresh and removal of non-existent paths
    auto end_time = std::chrono::high_resolution_clock::now();

    // Calculate and print the elapsed time
    std::cout << " " << std::endl;
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    
    // Print the time taken for the entire process in bold with one decimal place
    std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m" << std::endl;

    // Inform the user that the cache has been successfully refreshed
    std::cout << "\033[94mCache refreshed successfully.\033[0m" << std::endl;
    std::cout << " " << std::endl;
}

//	MOUNT STUFF	\\

// Function to check if a directory exists at the specified path
bool directoryExists(const std::string& path) {
    // Use the std::filesystem::is_directory function to check if the path is a directory
    return std::filesystem::is_directory(path);
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
    if (mountedIsos.find(isoFile) != mountedIsos.end()) {
        std::cout << "\033[33mALREADY MOUNTED\e[0m: ISO file '" << isoFile << "' is already mounted at '" << mountedIsos[isoFile] << "'.\033[0m" << std::endl;
        return;
    }

    // Use the filesystem library to extract the ISO file name
    fs::path isoPath(isoFile);
    std::string isoFileName = isoPath.stem().string(); // Remove the .iso extension

    std::string mountPoint = "/mnt/iso_" + isoFileName; // Use the modified ISO file name in the mount point with "iso_" prefix

    // Check if the mount point directory doesn't exist, create it
    if (!directoryExists(mountPoint)) {
        // Create the mount point directory
        std::string mkdirCommand = "sudo mkdir " + shell_escape(mountPoint);
        if (system(mkdirCommand.c_str()) != 0) {
            std::perror("\033[31mFailed to create mount point directory\033[0m");
            return;
        }

        // Mount the ISO file to the mount point
        std::string mountCommand = "sudo mount -o loop " + shell_escape(isoFile) + " " + shell_escape(mountPoint) + " > /dev/null 2>&1";
        int mountResult = system(mountCommand.c_str());
			if (mountResult == 0) {
				std::cout << "\033[32mMounted at: " << mountPoint << "\033[0m" << std::endl;}
			if (mountResult != 0) {
				std::cerr << "\033[31mFailed to mount: " << isoFile << "\033[0m" <<std::endl;
           

            // Cleanup the mount point directory
            std::string cleanupCommand = "sudo rmdir " + shell_escape(mountPoint);
            if (system(cleanupCommand.c_str()) != 0) {
                std::perror("\033[31mFailed to clean up mount point directory\033[0m");
            }

            
            return;
        } else {
            //std::cout << "\033[32mISO file '" << isoFile << "' mounted at '" << mountPoint << "'.\033[0m" << std::endl;
             //Store the mount point in the map
            mountedIsos[isoFile] = mountPoint;
            
        }
    } else {
        // The mount point directory already exists, so the ISO is considered mounted
        std::cout << "\033[33mISO file '" << isoFile << "' is already mounted at '" << mountPoint << "'.\033[m" << std::endl;
        mountedIsos[isoFile] = mountPoint;
        
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


// Function to check if a string ends with ".iso" (case-insensitive)
bool ends_with_iso(const std::string& str) {
    // Convert the string to lowercase for a case-insensitive comparison
    std::string lowercase = str;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), ::tolower);

    // Check if the lowercase string ends with ".iso"
    return lowercase.size() >= 4 && lowercase.compare(lowercase.size() - 4, 4, ".iso") == 0;
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


// Function to select and mount ISO files by number
void select_and_mount_files_by_number() {
	// Remove non-existent paths from the cache
    removeNonExistentPathsFromCacheWithOpenMP();

    // Load ISO files from cache
    std::vector<std::string> isoFiles = loadCache();

    // Check if the cache is empty
    if (isoFiles.empty()) {
        std::system("clear");
        std::cout << "\033[33mCache is empty. Please refresh the cache from the main menu.\033[0m" << std::endl;
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
        std::cout << "\033[33mNo .iso files in the cache. Please refresh the cache from the main menu.\033[0m" << std::endl;
        return;
    }

    // Set to track mounted ISO files
    std::unordered_set<std::string> mountedSet;

    // Start the timer before the loop
    auto start_time = std::chrono::high_resolution_clock::now();

    // Main loop for selecting and mounting ISO files
    while (true) {
        std::system("clear");
        std::cout << "\033[33m! IF EXPECTED ISO FILE IS NOT ON THE LIST, REFRESH CACHE FROM MAIN MENU !\n\033[0m" << std::endl;
        printIsoFileList(isoFiles);

        // Prompt user for input
        std::string input;
        std::cout << "\033[94mChoose ISO(s) to mount (e.g., '1-3', '1 2', '00' mounts all, or press Enter to return):\033[0m ";
		std::getline(std::cin, input);
        std::system("clear");
		
		// Start the timer
		auto start_time = std::chrono::high_resolution_clock::now();
        
        // Check if the user wants to return
        if (input.empty()) {
            std::cout << "Press Enter to Return" << std::endl;
            break;
        }

        // Check if the user wants to mount all ISO files
        if (input == "00") {
            for (const std::string& iso : isoFiles) {
                handleIsoFile(iso, mountedSet);
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


// Function to print the list of ISO files with their corresponding numbers
void printIsoFileList(const std::vector<std::string>& isoFiles) {
    for (int i = 0; i < isoFiles.size(); i++) {
        std::cout << i + 1 << ". " << isoFiles[i] << std::endl;
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

void processInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedSet) {
    std::istringstream iss(input);
    bool invalidInput = false;

    std::string token;
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
                std::cerr << "\033[31mInvalid input format: " << token << "\033[0m" << std::endl;
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                invalidInput = true;
                std::cerr << "\033[31mInvalid range: " << token << ". Please enter a valid range.\033[0m" << std::endl;
                continue;
            }

            if (start > end || start < 1 || static_cast<size_t>(end) > isoFiles.size()) {
                invalidInput = true;
                std::cerr << "\033[31mInvalid range: " << start << "-" << end << ". Please enter a valid range.\033[0m" << std::endl;
                continue;
            }

            for (int i = start; i <= end; ++i) {
                if (static_cast<size_t>(i) <= isoFiles.size()) {
                    handleIsoFile(isoFiles[i - 1], mountedSet);
                } else {
                    invalidInput = true;
                    std::cerr << "\033[31mFile index: " << i << " , does not exist.\033[0m" << std::endl;
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                handleIsoFile(isoFiles[num - 1], mountedSet);
            } else {
                invalidInput = true;
                std::cerr << "\033[31mFile index: " << num << " , does not exist.\033[0m" << std::endl;
            }
        } else {
            invalidInput = true;
            std::cerr << "\033[31mInvalid input: " << token << ". Please enter a valid number or range.\033[0m" << std::endl;
        }
    }
}

// Function to print a message indicating that the ISO file is already mounted
void printAlreadyMountedMessage(const std::string& iso) {
    std::cout << "\033[33mISO file '" << iso << "' is already mounted.\033[0m" << std::endl;
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
    #pragma omp parallel for reduction(&&:equal)
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
        std::cerr << "Error: " << e.what() << std::endl;
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
        std::cerr << "Error opening the /mnt directory." << std::endl;
    }

    // Display a list of mounted ISOs with ISO names in bold and magenta text
    if (!isoDirs.empty()) {
        std::cout << "\033[37;1mList of mounted ISOs:\033[0m" << std::endl; // White and bold
        for (size_t i = 0; i < isoDirs.size(); ++i) {
            std::cout << i + 1 << ". \033[1m\033[35m" << isoDirs[i] << "\033[0m" << std::endl; // Bold and magenta
        }
    } else {
        // Print a message if no ISOs are mounted
        std::cerr << "\033[31mNO MOUNTED ISOS FOUND\n\033[0m" << std::endl;
    }
}


void unmountISO(const std::string& isoDir) {
    // Construct the unmount command with sudo, umount, and suppressing logs
    std::string unmountCommand = "sudo umount -l " + shell_escape(isoDir) + " > /dev/null 2>&1";

    // Execute the unmount command
    int result = system(unmountCommand.c_str());

    // Check if the unmounting was successful
    if (result == 0) {
        std::cout << "\033[32mUnmounted mount point: " << isoDir << "\033[0m" << std::endl; // Print success message

        // Check if the directory is empty before removing it
        if (std::filesystem::is_empty(isoDir)) {
            // Construct the remove directory command with sudo, rmdir, and suppressing logs
            std::string removeDirCommand = "sudo rmdir " + shell_escape(isoDir) + " 2>/dev/null";

            // Execute the remove directory command
            int removeDirResult = system(removeDirCommand.c_str());
        }
    } else {
        std::cerr << "\033[31mFailed to unmount: " << isoDir << " ...Probably not an ISO mountpoint, check it out manually.\033[0m" << std::endl; // Print failure message
    }
}


// Function to unmount ISOs based on user input
void unmountISOs() {
    listMountedISOs(); // Display the initial list of mounted ISOs

    // Path where ISO directories are expected to be mounted
    const std::string isoPath = "/mnt";

    while (true) {
        std::vector<std::string> isoDirs;

        // Find and store directories with the name "iso_*" in /mnt
        DIR* dir;
        struct dirent* entry;

        if ((dir = opendir(isoPath.c_str())) != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                // Check if the entry is a directory and has a name starting with "iso_"
                if (entry->d_type == DT_DIR && std::string(entry->d_name).find("iso_") == 0) {
                    isoDirs.push_back(isoPath + "/" + entry->d_name);
                }
            }
            closedir(dir);
        } else {
            // Print an error message if there is an issue opening the /mnt directory
            std::cerr << "\033[33mError opening the /mnt directory.\033[0m" << std::endl;
        }

        // Check if there are no mounted ISOs
        if (isoDirs.empty()) {
            std::cout << "\033[33mLIST IS EMPTY, NOTHING TO DO.\n\033[0m";
            std::cout << "Press Enter to continue...";
            std::cin.get(); // Wait for the user to press Enter
            return;
        }

        // Prompt for unmounting input
        std::cout << "\033[94mChoose ISO(s) to unmount (e.g. '1-3', '1 2', '00' unmounts all, or press Enter to return):\033[0m ";
        std::string input;
        std::getline(std::cin, input);
        std::system("clear");

        // Start the timer
        auto start_time = std::chrono::high_resolution_clock::now();

        // Check if the user wants to return
        if (input == "") {
            break;  // Exit the loop
        }

        if (input == "00") {
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
                if (number >= 1 && static_cast<size_t>(number) <= isoDirs.size()) {
                    // Check for duplicates
                    if (uniqueIndices.find(number) == uniqueIndices.end()) {
                        uniqueIndices.insert(number);
                        unmountIndices.push_back(number);
                    } 
                    
                } else {
                    // Print an error message for an invalid index
                    std::cerr << "\033[31mFile index: " << number << " , does not exist.\033[0m" << std::endl;
                    continue;  // Restart the loop
                }
            } else if (std::regex_match(token, std::regex("^(\\d+)-(\\d+)$"))) {
                // Range input (e.g., "1-3")
                std::smatch match;
                std::regex_match(token, match, std::regex("^(\\d+)-(\\d+)$"));
                int startRange = std::stoi(match[1]);
                int endRange = std::stoi(match[2]);
                if (startRange >= 1 && endRange >= startRange && static_cast<size_t>(endRange) <= isoDirs.size()) {
                    for (int i = startRange; i <= endRange; ++i) {
                        // Check for duplicates
                        if (uniqueIndices.find(i) == uniqueIndices.end()) {
                            uniqueIndices.insert(i);
                            unmountIndices.push_back(i);
                        } 
                    }
                } else {
                    // Print an error message for an invalid range
                    std::cerr << "\033[31mInvalid range: " << startRange << "-" << endRange << ". Please enter a valid range.\033[0m" << std::endl;
                }
            } else {
                // Print an error message for invalid input format
                std::cerr << "\033[31mInvalid input format: " << token << "\033[0m" << std::endl;
            }
         
        }
        
        // Determine the number of available CPU cores
        const unsigned int numCores = std::thread::hardware_concurrency();

        // Create a vector of threads to perform unmounting and directory removal concurrently
        std::vector<std::thread> threads;

        for (int index : unmountIndices) {
            const std::string& isoDir = isoDirs[index - 1];

            // Use a thread for each ISO to be unmounted
            threads.emplace_back([&]() {
                std::lock_guard<std::mutex> lock(mtx); // Lock the critical section
                unmountISO(isoDir);
            });
        }

        // Join the threads to wait for them to finish
        for (auto& thread : threads) {
            thread.join();
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
		
        listMountedISOs(); // Display the updated list of mounted ISOs after unmounting

      
    }
}
