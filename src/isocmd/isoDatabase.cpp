// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"


// Database Variables

const std::string databaseDirectory = std::string(std::getenv("HOME")) + "/.local/share/isocmd/database/"; // Construct the full path to the cache directory
const std::string databaseFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_database.txt";
const std::string databaseFilename = "iso_commander_database.txt";
const uintmax_t maxDatabaseSize = 1 * 1024 * 1024; // 1MB

// Global mutex to protect counter cout
std::mutex couNtMutex;


// Function to remove non-existent paths from database and cache
void removeNonExistentPathsFromDatabase() {
    // Check if the database file exists
    if (!fs::exists(databaseFilePath)) {
        globalIsoFileList.clear();
        return;
    }
    
    // Open the cache file for reading
    int fd = open(databaseFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        return;
    }
    
    // Lock the file to prevent concurrent access
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }
    
    // Get the file size
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
    size_t fileSize = sb.st_size;
    
    // Memory map the file
    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
    
    // Read the file into a vector of strings
    std::vector<std::string> cache;
    char* start = mappedFile;
    char* end = mappedFile + fileSize;
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        cache.emplace_back(start, lineEnd);
        start = lineEnd + 1;
    }
    
    // Unmap and close the file
    munmap(mappedFile, fileSize);
    flock(fd, LOCK_UN);
    close(fd);
    
    // Create a bitset to track which paths exist (more efficient than vector<bool>)
    std::vector<bool> pathExists(cache.size(), false);
    std::atomic<size_t> existingPathCount{0};
    
    // Use a thread pool to check paths in parallel
    ThreadPool pool(maxThreads);
    
    // Determine batch size
    const size_t batchSize = std::max(cache.size() / maxThreads + 1, static_cast<size_t>(2));
    
    // Create a vector to hold futures
    std::vector<std::future<void>> futures;
    
    // Process paths in batches using the thread pool
    for (size_t i = 0; i < cache.size(); i += batchSize) {
        size_t batchEnd = std::min(i + batchSize, cache.size());
        futures.emplace_back(pool.enqueue([&cache, &pathExists, &existingPathCount, i, batchEnd]() {
            for (size_t j = i; j < batchEnd; ++j) {
                if (fs::exists(cache[j])) {
                    pathExists[j] = true;
                    existingPathCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }));
    }
    
    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.get();  // use get() instead of wait() to propagate exceptions
    }
    
    // Check if any paths were removed
    if (existingPathCount == cache.size()) {
        // No changes needed, return without modifying the file
        return;
    }
    
    // Create vector of retained paths
    std::vector<std::string> retainedPaths;
    retainedPaths.reserve(existingPathCount);
    for (size_t i = 0; i < cache.size(); ++i) {
        if (pathExists[i]) {
            retainedPaths.push_back(std::move(cache[i]));
        }
    }
    
    // Open the file for writing, create if necessary, truncate it
    int writeFd = open(databaseFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (writeFd == -1) {
        return;
    }
    
    // Acquire exclusive lock
    if (flock(writeFd, LOCK_EX) == -1) {
        close(writeFd);
        return;
    }
    
    // Write retainedPaths
    for (const auto& path : retainedPaths) {
        std::string line = path + '\n';
        write(writeFd, line.c_str(), line.size());
    }
    
    // Release lock and close
    flock(writeFd, LOCK_UN);
    close(writeFd);
}


// Count Database entries for stats
int countNonEmptyLines(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Unable to open file: " << filePath << std::endl;
        return -1;
    }

    int nonEmptyLineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        // Check if the line is not empty (ignoring whitespace)
        if (!line.empty() && line.find_first_not_of(" \t\n\r\f\v") != std::string::npos) {
            ++nonEmptyLineCount;
        }
    }

    file.close();
    return nonEmptyLineCount;
}


// Set default home dir
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}


// Function to auto-import ISO files in cache without blocking the UI
void backgroundDatabaseImport(std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    std::vector<std::string> paths;
    int localMaxDepth = -1;
    bool localPromptFlag = false;

    // Read paths from file
    {
        std::ifstream file(historyFilePath);
        if (!file.is_open()) {
            isImportRunning.store(false);
            return;
        }
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string path;
            while (std::getline(iss, path, ';')) {
                if (!path.empty() && path[0] == '/') {
                    if (path.back() != '/') {
                        path += '/';
                    }
                    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
                        paths.push_back(path);
                    }
                }
            }
        }
    }
    
    // Exclude root path '/' only when there are other paths
    if (paths.size() > 1) {
        auto it = std::find(paths.begin(), paths.end(), "/");
        if (it != paths.end()) {
            paths.erase(it);
        }
    }
    
    // Sort paths by length
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
        return a.size() < b.size();
    });
    
    // Filter out subdirectories
    std::vector<std::string> finalPaths;
    for (const auto& path : paths) {
        bool isSubdir = false;
        for (const auto& existingPath : finalPaths) {
            if (path.size() >= existingPath.size() &&
                path.compare(0, existingPath.size(), existingPath) == 0 &&
                (existingPath.back() == '/' || path[existingPath.size()] == '/')) {
                isSubdir = true;
                break;
            }
        }
        if (!isSubdir) {
            finalPaths.push_back(path);
        }
    }
    
    // Set up data structures for processing
    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> uniqueErrorMessages;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;
    
    // Create a thread pool based on the available hardware threads.
    size_t numThreads = (maxThreads == 0 ? 4 : std::min(maxThreads * 2, static_cast<unsigned int>(MAX_HISTORY_LINES)));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    
    // Enqueue tasks for each valid directory
    for (const auto& path : finalPaths) {
        if (isValidDirectory(path)) {
            futures.emplace_back(pool.enqueue([&, path]() {
                traverse(path, allIsoFiles, uniqueErrorMessages,
                         totalFiles, processMutex, traverseErrorMutex,
                         localMaxDepth, localPromptFlag);
            }));
        }
    }
    
    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
    
    saveToDatabase(allIsoFiles, newISOFound);
    isImportRunning.store(false);
}


// Function to load ISO database from file
void loadFromDatabase(std::vector<std::string>& isoFiles) {
    // Open the database file in read-only mode
    int fd = open(databaseFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        return; // File doesn't exist or cannot be opened, exit the function
    }

    // Acquire a shared lock using flock to allow multiple readers but no writers
    if (flock(fd, LOCK_SH) == -1) {
        close(fd); // Release the file descriptor if locking fails
        return;
    }

    // Get file statistics to check its size and other attributes
    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1 || fileStat.st_size == 0) {
        // If fstat fails or the file is empty, release the lock and close the file
        flock(fd, LOCK_UN);
        close(fd);
        isoFiles.clear(); // Clear the output vector
        return;
    }

    // Store the file size for later use
    const auto fileSize = fileStat.st_size;

    // Map the file into memory for efficient reading
    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        // If mmap fails, release the lock and close the file
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    // Vector to store the loaded ISO file paths
    std::vector<std::string> loadedFiles;

    // Iterate through the mapped file to read lines (ISO file paths)
    char* start = mappedFile;
    char* end = mappedFile + fileSize;
    while (start < end) {
        // Find the end of the current line
        char* lineEnd = std::find(start, end, '\n');
        // Extract the line as a string
        std::string line(start, lineEnd);
        start = lineEnd + 1; // Move to the start of the next line

        // If the line is not empty, add it to the loadedFiles vector
        if (!line.empty()) {
            loadedFiles.push_back(std::move(line));
        }
    }

    // Unmap the memory and release the file lock
    munmap(mappedFile, fileSize);
    flock(fd, LOCK_UN);
    close(fd);

    // Swap the loaded files with the input vector to update it
    isoFiles.swap(loadedFiles);
}


// Function to save ISO cache to database
bool saveToDatabase(const std::vector<std::string>& isoFiles, std::atomic<bool>& newISOFound) {
    // Construct the full path to the database file
    std::filesystem::path cachePath = databaseDirectory;
    cachePath /= databaseFilename;

    // Create the database directory if it doesn't exist
    if (!std::filesystem::exists(databaseDirectory) && !std::filesystem::create_directories(databaseDirectory)) {
        return false; // Return false if directory creation fails
    }
    // Check if the database path is a valid directory
    if (!std::filesystem::is_directory(databaseDirectory)) {
        return false;
    }

    // Load the existing cache from the database
    std::vector<std::string> existingCache;
    loadFromDatabase(existingCache);

    // Convert the existing cache to a set for efficient lookup
    std::unordered_set<std::string> existingSet(existingCache.begin(), existingCache.end());
    
    // Vector to store new entries that are not already in the cache
    std::vector<std::string> newEntries;
    
    // Iterate through the input ISO files to find new entries
    for (const auto& iso : isoFiles) {
        if (existingSet.find(iso) == existingSet.end()) {
            // If the ISO file is not in the existing cache, add it to newEntries
            newEntries.push_back(iso);
            newISOFound.store(true); // Set the atomic variable to true when a new ISO is found
        }
    }
    
    // If no new entries are found, set newISOFound to false and return
    if (newEntries.empty()) {
        newISOFound.store(false);
        return false; // No new entries, don't modify cache
    }
    
    // Combine the existing cache with new entries, respecting the maximum size limit
    std::vector<std::string> combinedCache = existingCache;
    combinedCache.insert(combinedCache.end(), newEntries.begin(), newEntries.end());
    if (combinedCache.size() > maxDatabaseSize) {
        // If the combined cache exceeds the maximum size, remove the oldest entries
        combinedCache.erase(combinedCache.begin(), combinedCache.begin() + (combinedCache.size() - maxDatabaseSize));
    }
    
    // Open the database file for writing, creating it if it doesn't exist
    int fd = open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        return false; // Return false if the file cannot be opened
    }

    // Acquire an exclusive lock to prevent other processes from writing
    if (flock(fd, LOCK_EX) == -1) {
        close(fd); // Release the file descriptor if locking fails
        return false;
    }
    
    // Write the combined cache to the file
    bool success = true;
    for (const auto& entry : combinedCache) {
        std::string line = entry + "\n";
        if (write(fd, line.data(), line.size()) == -1) {
            success = false; // Set success to false if writing fails
            break;
        }
    }
    
    // Release the lock and close the file
    flock(fd, LOCK_UN);
    close(fd);

    // Return whether the operation was successful
    return success;
}


// Function to display on-disk and ram statistics
void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize, const std::unordered_map<std::string, std::string>& transformationCache, const std::vector<std::string>& globalIsoFileList) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C signal to prevent interruption
    disable_ctrl_d();               // Disable Ctrl+D to avoid unwanted program termination
    clearScrollBuffer();            // Clear any buffered data from the scroll buffer

    try {
        // Create the database file if it does not exist
        std::filesystem::path filePath(databaseFilePath);
        if (!std::filesystem::exists(filePath)) {
            std::ofstream createFile(databaseFilePath);
            createFile.close();  // Close the file after creation
        }
        
        // Create history file if it doesn't exist
        if (!std::filesystem::exists(historyFilePath)) {
            std::ofstream createFile(historyFilePath);
            createFile.close();  // Close the file after creation
        }

        // Create filter history file if it doesn't exist
        if (!std::filesystem::exists(filterHistoryFilePath)) {
            std::ofstream createFile(filterHistoryFilePath);
            createFile.close();  // Close the file after creation
        }

        // Display the statistics for the ISO database
        std::cout << "\n\033[1;94m=== ISO Database ===\033[0m\n";
        
        // Get the file size in bytes
        std::uintmax_t fileSizeInBytes = std::filesystem::file_size(filePath);
        std::uintmax_t cachesizeInBytes = maxDatabaseSize;
        
        // Convert file size and cache size to kilobytes
        double fileSizeInKB = fileSizeInBytes / 1024.0;
        double cachesizeInKb = cachesizeInBytes / 1024.0;
        
        // Calculate the usage percentage of the database file
        double usagePercentage = (fileSizeInBytes * 100.0) / cachesizeInBytes;
        
        // Display the capacity, file size, and usage percentage of the database
        std::cout << "\n\033[1;92mCapacity:\033[1;97m " << std::fixed << std::setprecision(0) << fileSizeInKB << "KB" 
                  << "/" << std::setprecision(0) << cachesizeInKb << "KB" 
                  << " (" << std::setprecision(1) << usagePercentage << "%)"
                  << " \n\033[1;92mEntries:\033[1;97m " << countNonEmptyLines(databaseFilePath) 
                  << "\n\033[1;92mLocation:\033[1;97m " << "'" << databaseFilePath << "'\033[0;1m\n";
       
        // Display the statistics for the history database
        std::cout  << "\n\033[1;94m=== History Database ===\033[0m\n"
                  << " \n\033[1;92mFolderPath Entries:\033[1;97m " << countNonEmptyLines(historyFilePath)<< "/" << MAX_HISTORY_LINES
                  << "\n\033[1;92mLocation:\033[1;97m " << "'" << historyFilePath << "'\033[0;1m"
                  << " \n\n\033[1;92mFilterTerm Entries:\033[1;97m " << countNonEmptyLines(filterHistoryFilePath) << "/" << MAX_HISTORY_PATTERN_LINES
                  << "\n\033[1;92mLocation:\033[1;97m " << "'" << filterHistoryFilePath << "'\033[0;1m" << std::endl;
        
        // Display the buffered entries in RAM
        std::cout << "\n\033[1;94m=== Buffered Entries ===\033[0m\n";
        
        // Show the total number of cached string entries in RAM
        std::cout << "\033[1;96m\nSTR → RAM:\033[1;97m " << transformationCache.size() + cachedParsesForUmount.size() << "\n";
        
        // Show the number of ISO files in RAM
        std::cout << "\n\033[1;92mISO → RAM:\033[1;97m " << globalIsoFileList.size() << "\n";
        
        // Show the number of BIN/IMG files in RAM
        std::cout << "\n\033[1;38;5;208mBIN/IMG → RAM:\033[1;97m " << binImgFilesCache.size() << "\n";
        
        // Show the number of MDF files in RAM
        std::cout << "\033[1;38;5;208mMDF → RAM:\033[1;97m " << mdfMdsFilesCache.size() << "\n";
        
        // Show the number of NRG files in RAM
        std::cout << "\033[1;38;5;208mNRG → RAM:\033[1;97m " << nrgFilesCache.size() << "\n";
    } catch (const std::filesystem::filesystem_error& e) {
        // Handle filesystem errors (e.g., unable to access a configuration file)
        std::cerr << "\n\033[1;91mError: Unable to access configuration file: \033[1;93m'"
                  << configPath << "'\033[1;91m.\033[0;1m\n";
    }

    // Prompt the user to press Enter to return
    std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // Wait for the user to press Enter
}


// Function to set the AutoUpdate switch in teh config file
void updateAutoUpdateConfig(const std::string& configPath, const std::string& inputSearch) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    // Create directory if it doesn't exist
    std::filesystem::path dirPath = std::filesystem::path(configPath).parent_path();
    if (!std::filesystem::exists(dirPath)) {
        if (!std::filesystem::create_directories(dirPath)) {
            std::cerr << "\n\033[1;91mFailed to create directory: \033[1;93m'" 
                    << dirPath.string() << "\033[1;91m'.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return; // Ensure we exit to avoid unnecessary operations after failure
        }
    }

    // Read the existing configuration (you need to implement this function or replace it)
    std::map<std::string, std::string> config = readConfig(configPath);

    // Update the auto_update setting based on the input
    config["auto_update"] = (inputSearch == "*auto_on") ? "on" : "off";

    // Ensure settings maintain order and are written back to the file
    std::vector<std::pair<std::string, std::string>> orderedDefaults = {
        {"auto_update", config["auto_update"]},       // Updated auto_update value
        {"pagination", config["pagination"]},         // Existing pagination value
        {"mount_list", config["mount_list"]},
        {"umount_list", config["umount_list"]},
        {"cp_mv_rm_list", config["cp_mv_rm_list"]},
        {"write_list", config["write_list"]},
        {"conversion_lists", config["conversion_lists"]}
    };

    // Write all settings back to file in the correct order
    std::ofstream outFile(configPath);
    if (outFile.is_open()) {
        for (const auto& [key, value] : orderedDefaults) {
            outFile << key << " = " << value << "\n";
        }
        outFile.close();

        // Display the appropriate message based on the action
        std::cout << "\n\033[0;1mAutomatic background updates have been "
                << (inputSearch == "*auto_on" ? "\033[1;92menabled" : "\033[1;91mdisabled")
                << "\033[0;1m.\033[0;1m\n";
    } else {
        std::cerr << "\n\033[1;91mError: Unable to access configuration file: \033[1;93m'"
                  << configPath << "'\033[1;91m.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function that can delete or show stats for ISO cache it is called from within refreshForDatabase
void databaseSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& filterHistory, std::atomic<bool>& newISOFound) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    
    std::string initialDir = "";
    
    const std::unordered_set<std::string> validInputs = {
        "*fl_m", "*cl_m", "*fl_u", "*cl_u", "*fl_fo", "*cl_fo", "*fl_w", "*cl_w", "*fl_c", "*cl_c"
    };
    
    if (inputSearch == "stats") {
        displayDatabaseStatistics(databaseFilePath, maxDatabaseSize, transformationCache, globalIsoFileList);
    } else if (inputSearch == "config") {
        displayConfigurationOptions(configPath);
    } else if (inputSearch == "!clr") {
        if (std::remove(databaseFilePath.c_str()) != 0) {
            std::cerr << "\n\001\033[1;91mError clearing ISO database: \001\033[1;93m'" 
                      << databaseFilePath << "\001'\033[1;91m. File missing or inaccessible." << std::endl;
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            // Clean transformationCache for .iso entries (case-insensitive)
            for (auto it = transformationCache.begin(); it != transformationCache.end();) {
                const std::string& key = it->first;
                if (key.size() >= 4) {
                    // Extract the last 4 characters (extension)
                    std::string ext = key.substr(key.size() - 4);
                    // Convert extension to lowercase using toLowerInPlace
                    toLowerInPlace(ext);
                    if (ext == ".iso") {
                        it = transformationCache.erase(it);
                        continue;
                    }
                }
                ++it;
            }
                    
            std::cout << "\n\001\033[1;92mISO database cleared successfully\001\033[1;92m." << std::endl;
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::vector<std::string>().swap(globalIsoFileList);
        }
    } else if (inputSearch == "!clr_paths" || inputSearch == "!clr_filter") {
        clearHistory(inputSearch);
    } else if (inputSearch == "*auto_on" || inputSearch == "*auto_off") {
        updateAutoUpdateConfig(configPath, inputSearch);
    } else if (inputSearch == "*flno_on" || inputSearch == "*flno_off") {
		updateFilenamesOnly(configPath, inputSearch);
	} else if (inputSearch.substr(0, 12) == "*pagination_") {
        updatePagination(inputSearch, configPath);
    } else if (isValidInput(inputSearch)) {
        setDisplayMode(inputSearch);
    }
    // Refresh the database after handling any command
    refreshForDatabase(initialDir, promptFlag, maxDepth, filterHistory, newISOFound);
}
