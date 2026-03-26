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


// Persistent reusable local theadPool for removeNonExistentPathsFromDatabase
static ThreadPool& getIOThreadPool(size_t* count = nullptr) {
    static const unsigned cap = std::min(maxThreads, 16u);
    static ThreadPool pool(cap);
    // No atexit needed — ~ThreadPool() calls waitAllTasksCompleted() then joins all threads cleanly
    if (count) *count = cap;
    return pool;
}


// Function to remove non-existent paths from database and cache
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList) {
    // Static mutex to serialize file access across threads within the same process
    static std::mutex fileMutex;
    std::lock_guard<std::mutex> fileLock(fileMutex);
    
    // Attempt to open the file directly instead of using access() + open(),
    // which would introduce a TOCTOU race between the existence check and the open.
    int fd = open(databaseFilePath.c_str(), O_RDWR, 0644);
    if (fd == -1) {
        if (errno == ENOENT) {
            std::lock_guard<std::mutex> lock(updateListMutex);
            globalIsoFileList.clear();
        }
        return;
    }
 
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }
 
    // Read all lines via a dup'd descriptor
    int dupFd = dup(fd);
    if (dupFd == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
 
    FILE* file = fdopen(dupFd, "r");
    if (!file) {
        close(dupFd);
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
 
    std::vector<std::string> cache;
    char* linePtr = nullptr;
    size_t len = 0;
 
    while (getline(&linePtr, &len, file) != -1) {
        std::string line(linePtr);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) cache.push_back(std::move(line));
    }
 
    free(linePtr);
    fclose(file); // also closes dupFd
 
    if (cache.empty()) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
 
    // Parallel existence check
    std::vector<bool> pathExists(cache.size(), false);
    std::atomic<size_t> existingCount{0};

    size_t numThread;
    ThreadPool& pool = getIOThreadPool(&numThread);
    const size_t chunkSize = (cache.size() + numThread - 1) / numThread;
 
    std::vector<std::future<void>> futures;
    futures.reserve(numThread);
 
    for (size_t i = 0; i < numThread; ++i) {
        const size_t start = i * chunkSize;
        const size_t end = std::min(cache.size(), start + chunkSize);
        if (start >= end) break;
 
        futures.emplace_back(pool.enqueue([&cache, &pathExists, &existingCount, start, end] {
            for (size_t j = start; j < end; ++j) {
                if (access(cache[j].c_str(), F_OK) == 0) {
                    pathExists[j] = true;
                    existingCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }));
    }
 
    for (auto& f : futures) f.get();
 
    // Early exit if nothing changed
    if (existingCount == cache.size()) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
 
    // Build retained list
    std::vector<std::string> retained;
    retained.reserve(existingCount);
    for (size_t i = 0; i < cache.size(); ++i) {
        if (pathExists[i])
            retained.push_back(std::move(cache[i]));
    }
 
    // Write back to file
    if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    bool writeSuccess = true;
    for (const auto& path : retained) {
        std::string line = path + '\n';
        if (write(fd, line.c_str(), line.size()) == -1) {
            writeSuccess = false;
            break;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);

    // Sync in-memory list to match pruned database only if write succeeded
    if (writeSuccess) {
        std::lock_guard<std::mutex> lock(updateListMutex);
        globalIsoFileList = std::move(retained);
    }
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


// Function to generalize paths from history and push them into backgroundDatabaseImport for traverse to work with
std::vector<std::string> hierarchicalPathReduction(const std::vector<std::string>& paths) {
    std::map<std::string, std::vector<std::string>> pathGroups;
    std::vector<std::string> allPaths;
    
    // Split semicolon-delimited paths and normalize
    for (const auto& pathEntry : paths) {
        std::istringstream iss(pathEntry);
        std::string path;
        while (std::getline(iss, path, ';')) {
            if (!path.empty() && path[0] == '/') {
                if (path.back() != '/') path += '/';
                allPaths.push_back(path);
            }
        }
    }
    
    // Group paths by first 3 directory levels
    for (const auto& path : allPaths) {
        size_t slashCount = 0, pos = 0;
        
        // Find position after 3rd slash
        for (size_t i = 1; i < path.length() && slashCount < 3; ++i) {
            if (path[i] == '/') {
                pos = i;
                slashCount++;
            }
        }
        
        std::string key = (slashCount >= 3) ? path.substr(0, pos + 1) : path;
        pathGroups[key].push_back(path);
    }
    
    // Create final paths: use prefix if multiple paths, original if single
    std::vector<std::string> finalPaths;
    for (const auto& [prefix, groupPaths] : pathGroups) {
        finalPaths.push_back((groupPaths.size() > 1) ? prefix : groupPaths[0]);
    }
    
    // Remove redundant parent paths (only top-level with ≤2 directory levels)
    std::sort(finalPaths.begin(), finalPaths.end());
    std::vector<std::string> result;
    
    for (const auto& path : finalPaths) {
        // Count directory levels
        int levels = std::count(path.begin() + 1, path.end(), '/');
        
        // Skip if it's a top-level path that's a parent of another path
        bool isRedundant = (levels <= 2) && 
            std::any_of(finalPaths.begin(), finalPaths.end(), 
                [&path](const std::string& other) {
                    return other != path && other.starts_with(path);
                });
        
        if (!isRedundant) {
            result.push_back(path);
        }
    }
    
    return result;
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
    
    // Apply path generalization
    std::vector<std::string> finalPaths = hierarchicalPathReduction(paths);
    
    // Early exit if paths are empty
    if (finalPaths.empty()) {
		isImportRunning.store(false);
		return;
	}
    
    
    // Set up data structures for processing
    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> uniqueErrorMessages;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;
    
    // Create a thread pool based on the available hardware threads.
    size_t numThreads = std::min(finalPaths.size(), static_cast<size_t>(maxThreads == 0 ? 4 : std::min(maxThreads * 2, (unsigned int)MAX_HISTORY_LINES)));
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
    
    // Cleanup after automatic update finishes
    removeNonExistentPathsFromDatabase(globalIsoFileList);
    
    saveToDatabase(allIsoFiles, newISOFound);
    isImportRunning.store(false);
}


// Function to load ISO database from file
void loadFromDatabase(std::vector<std::string>& outList) {
    // Static mutex to serialize file access across threads within the same process
    static std::mutex fileMutex;
    std::lock_guard<std::mutex> fileLock(fileMutex);
    
    int fd = open(databaseFilePath.c_str(), O_RDONLY);
    if (fd == -1) return;
    
    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }
    
    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1 || fileStat.st_size == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        outList.clear();   // caller holds updateListMutex — safe to write directly
        return;
    }
    
    // Simple read approach
    std::vector<char> buffer(fileStat.st_size);
    ssize_t bytesRead = read(fd, buffer.data(), fileStat.st_size);
    
    flock(fd, LOCK_UN);
    close(fd);
    
    if (bytesRead != fileStat.st_size) return;
    
    // Parse the buffer
    std::vector<std::string> loadedFiles;
    char* start = buffer.data();
    char* end = buffer.data() + bytesRead;
    
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            loadedFiles.push_back(std::move(line));
        }
        start = lineEnd + 1;
    }

    outList = std::move(loadedFiles);  // caller decides locking
}


// Function to save ISO cache to database
bool saveToDatabase(std::vector<std::string> globalIsoFileList, std::atomic<bool>& newISOFound) {
    // Parameter is passed by value — caller's copy is made at the call site before this
    // function runs, so globalIsoFileList is exclusively owned here with no mutex needed.

    // Construct the full path to the database file
    std::filesystem::path cachePath = databaseDirectory;
    cachePath /= databaseFilename;

    // Create the database directory if it doesn't exist
    if (!std::filesystem::exists(databaseDirectory) && !std::filesystem::create_directories(databaseDirectory)) {
        return false;
    }
    // Check if the database path is a valid directory
    if (!std::filesystem::is_directory(databaseDirectory)) {
        return false;
    }

    // Acquire the in-process file mutex before opening the file.
    // flock() is advisory and provides no mutual exclusion between threads in the
    // same process — both threads share the same lock. This mutex fills that gap.
    static std::mutex fileMutex;
    std::lock_guard<std::mutex> fileLock(fileMutex);

    // Open file and acquire lock FIRST
    int fd = open(cachePath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd == -1) return false;

    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return false;
    }

    // Load the existing cache from the same file descriptor
    std::vector<std::string> existingCache;

    struct stat fileStat;
    if (fstat(fd, &fileStat) == 0 && fileStat.st_size > 0) {
        // Read existing content
        std::vector<char> buffer(fileStat.st_size);
        ssize_t bytesRead = read(fd, buffer.data(), fileStat.st_size);

        if (bytesRead == fileStat.st_size) {
            // Parse the buffer
            char* start = buffer.data();
            char* end = buffer.data() + bytesRead;

            while (start < end) {
                char* lineEnd = std::find(start, end, '\n');
                std::string line(start, lineEnd);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    existingCache.push_back(std::move(line));
                }
                start = lineEnd + 1;
            }
        }
    }

    // Convert the existing cache to a set for efficient lookup
    std::unordered_set<std::string> existingSet(existingCache.begin(), existingCache.end());

    // Vector to store new entries that are not already in the cache
    std::vector<std::string> newEntries;

    // Use a local flag instead of writing directly to the shared atomic during the loop,
    // preventing concurrent calls from clobbering each other's result mid-execution.
    bool localNewISOFound = false;

    // Iterate through the ISO files to find new entries
    for (const auto& iso : globalIsoFileList) {
        if (existingSet.find(iso) == existingSet.end()) {
            newEntries.push_back(iso);
            localNewISOFound = true;
        }
    }

    // If no new entries are found, set newISOFound to false and return
    if (newEntries.empty()) {
        newISOFound.store(false);
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }

    // Combine the existing cache with new entries, respecting the maximum size limit
    std::vector<std::string> combinedCache = existingCache;
    combinedCache.insert(combinedCache.end(), newEntries.begin(), newEntries.end());
    if (combinedCache.size() > maxDatabaseSize) {
        combinedCache.erase(combinedCache.begin(), combinedCache.begin() + (combinedCache.size() - maxDatabaseSize));
    }

    // Truncate and write back to the same file descriptor
    if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }

    // Write the combined cache to the file
    bool success = true;
    for (const auto& entry : combinedCache) {
        std::string line = entry + "\n";
        if (write(fd, line.data(), line.size()) == -1) {
            success = false;
            break;
        }
    }

    // Release the lock and close the file
    flock(fd, LOCK_UN);
    close(fd);

    // Write the local result to the shared atomic only once, after all work is done
    newISOFound.store(localNewISOFound);

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
        {"filenames_only", config["filenames_only"]},         // Existing filenames_only value
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
