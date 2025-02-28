// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.local/share/isocmd/database/"; // Construct the full path to the cache directory
const std::string cacheFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_cache.txt";
const std::string cacheFileName = "iso_commander_cache.txt";
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB

// Global mutex to protect counter cout
std::mutex couNtMutex;

// Function to remove non-existent paths from cache
void removeNonExistentPathsFromCache() {
	
	if (!std::filesystem::exists(cacheFilePath)) {
        // If the file is missing, clear the ISO cache and return
        globalIsoFileList.clear();
        return;
    }

    // Open the cache file for reading
    int fd = open(cacheFilePath.c_str(), O_RDONLY);
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

    // Determine batch size
    const size_t maxThreads = std::thread::hardware_concurrency();
    const size_t batchSize = std::max(cache.size() / maxThreads + 1, static_cast<size_t>(2));

    // Create a vector to hold futures
    std::vector<std::future<std::vector<std::string>>> futures;

    // Process paths in batches
    for (size_t i = 0; i < cache.size(); i += batchSize) {
        auto begin = cache.begin() + i;
        auto end = std::min(begin + batchSize, cache.end());
            futures.push_back(std::async(std::launch::async, [begin, end]() {
            std::vector<std::string> result;
            for (auto it = begin; it != end; ++it) {
                if (std::filesystem::exists(*it)) {
                    result.push_back(*it);
                }
            }
            return result;
        }));
    }

    // Collect results
    std::vector<std::string> retainedPaths;
    for (auto& future : futures) {
        auto result = future.get();
        retainedPaths.insert(retainedPaths.end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }
    
    // Check if the retained paths are the same as the original cache
	if (cache.size() == retainedPaths.size() && 
		std::equal(cache.begin(), cache.end(), retainedPaths.begin())) {
		// No changes needed, return without modifying the file
		return;
	}

	// Only rewrite the file if there are changes
	std::ofstream updatedCacheFile(cacheFilePath, std::ios::out | std::ios::trunc);
	if (!updatedCacheFile.is_open()) {
		flock(fd, LOCK_UN);
		close(fd);
		return;
	}

    // Open the cache file for writing
    fd = open(cacheFilePath.c_str(), O_WRONLY);
    if (fd == -1) {
        return;
    }

    // Lock the file to prevent concurrent access
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }

    // Write the retained paths to the updated cache file
    if (!updatedCacheFile.is_open()) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    for (const std::string& path : retainedPaths) {
		if (std::filesystem::exists(path)) {
			updatedCacheFile << path << '\n';
		}
	}

    // RAII: Close the file and release the lock
    flock(fd, LOCK_UN);
    close(fd);
}


// Count ISOCache entries for stats
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


// Set default cache dir
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}


// Utility function to clear screen buffer and load IsoFiles from cache to a global vector only for the first time and only for if the cache has been modified.
bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType) {
	
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
	
    static std::filesystem::file_time_type lastModifiedTime;

    // Check if the cache file exists and has been modified
    bool needToReload = false;
    if (std::filesystem::exists(cacheFilePath)) {
        std::filesystem::file_time_type currentModifiedTime = 
            std::filesystem::last_write_time(cacheFilePath);

        if (lastModifiedTime == std::filesystem::file_time_type{}) {
            // First time checking, always load
            needToReload = true;
        } else if (currentModifiedTime > lastModifiedTime) {
            // Cache file has been modified since last load
            needToReload = true;
        }

        // Update last modified time
        lastModifiedTime = currentModifiedTime;
    } else {
        // Cache file doesn't exist, need to load
        needToReload = true;
    }

    // Common operations
    clearScrollBuffer();
    if (needToReload) {
        loadCache(globalIsoFileList);
    }
	{
		std::lock_guard<std::mutex> lock(updateListMutex);
		sortFilesCaseInsensitive(globalIsoFileList);
	}
    
    printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES", listSubType);

    if (globalIsoFileList.empty()) {
        std::cout << "\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return false;
    }

    return true;
}


// Function to auto-import ISO files in cache without blocking the UI
void backgroundCacheImport(int maxDepthParam, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    std::vector<std::string> paths;
    int localMaxDepth = maxDepthParam;
    bool localPromptFlag = false;
    const size_t maxThreadsX2 = (std::thread::hardware_concurrency() == 0 ? 4 : std::thread::hardware_concurrency()) * 2;

    // Local condition variable and mutex
    std::condition_variable cv;
    std::mutex threadMutex;
    std::atomic<size_t> activeThreads{0};

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

    // Sort and filter paths
    std::sort(paths.begin(), paths.end(),
        [](const std::string& a, const std::string& b) { return a.size() < b.size(); });

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

    // Process paths with thread limit
    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
    std::set<std::string> uniqueErrorMessages;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;

    std::vector<std::future<void>> futures;
    for (const auto& path : finalPaths) {
        if (isValidDirectory(path)) {
            // Wait until the number of active threads is less than maxThreads * 2
            std::unique_lock<std::mutex> lock(threadMutex);
            cv.wait(lock, [&]() { return activeThreads < maxThreadsX2; });

            // Increment the active thread count
            activeThreads++;

            // Launch the task asynchronously
            futures.push_back(std::async(std::launch::async, [&, path]() {
                traverse(path, allIsoFiles, uniqueErrorMessages,
                         totalFiles, processMutex, traverseErrorMutex,
                         localMaxDepth, localPromptFlag);

                // Decrement the active thread count when done
                {
                    std::unique_lock<std::mutex> lock(threadMutex);
                    activeThreads--;
                }

                // Notify the waiting thread that a thread has finished
                cv.notify_one();
            }));
        }
    }

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }

    saveCache(allIsoFiles, maxCacheSize, newISOFound);

    isImportRunning.store(false);
}


// Function to load ISO cache from file
void loadCache(std::vector<std::string>& isoFiles) {
    std::string cacheFilePath = getHomeDirectory() + "/.local/share/isocmd/database/iso_commander_cache.txt";

    int fd = open(cacheFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        return; // File doesn't exist or cannot be opened
    }

    // Acquire a shared lock using flock
    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }

    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1 || fileStat.st_size == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        isoFiles.clear();
        return;
    }

    const auto fileSize = fileStat.st_size;

    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    std::vector<std::string> loadedFiles;

    char* start = mappedFile;
    char* end = mappedFile + fileSize;
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        start = lineEnd + 1;

        if (!line.empty()) {
            loadedFiles.push_back(std::move(line));
        }
    }

    munmap(mappedFile, fileSize);
    flock(fd, LOCK_UN);
    close(fd);

    isoFiles.swap(loadedFiles);
}


// Function to save ISO cache to file
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize, std::atomic<bool>& newISOFound) {
    std::filesystem::path cachePath = cacheDirectory;
    cachePath /= cacheFileName;
    if (!std::filesystem::exists(cacheDirectory) && !std::filesystem::create_directories(cacheDirectory)) {
        return false;
    }
    if (!std::filesystem::is_directory(cacheDirectory)) {
        return false;
    }
    std::vector<std::string> existingCache;
    loadCache(existingCache);
    std::unordered_set<std::string> existingSet(existingCache.begin(), existingCache.end());
    
    // Only write if there are new entries
    std::vector<std::string> newEntries;
    for (const auto& iso : isoFiles) {
        if (existingSet.find(iso) == existingSet.end()) {
            newEntries.push_back(iso);
            newISOFound.store(true); // Set the atomic variable to true when a new ISO is found
        }
    }
    
    if (newEntries.empty()) {
        return true; // No new entries, don't modify cache
    }
    
    // Combine existing cache with new entries, respecting max size
    std::vector<std::string> combinedCache = existingCache;
    combinedCache.insert(combinedCache.end(), newEntries.begin(), newEntries.end());
    if (combinedCache.size() > maxCacheSize) {
        combinedCache.erase(combinedCache.begin(), combinedCache.begin() + (combinedCache.size() - maxCacheSize));
    }
    
    int fd = open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        return false;
    }
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return false;
    }
    
    bool success = true;
    for (const auto& entry : combinedCache) {
        std::string line = entry + "\n";
        if (write(fd, line.data(), line.size()) == -1) {
            success = false;
            break;
        }
    }
    
    flock(fd, LOCK_UN);
    close(fd);
    return success;
}


// Function to check if filepath exists
bool exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function that can delete or show stats for ISO cache it is called from within manualRefreshCache
void cacheAndMiscSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& historyPattern, std::atomic<bool>& newISOFound) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    const std::set<std::string> validInputs = {
        "*fl_m", "*cl_m", "*fl_u", "*cl_u", "*fl_fo", "*cl_fo", "*fl_w", "*cl_w", "*fl_c", "*cl_c"
    };
	std::string initialDir = "";
    if (inputSearch == "stats") {
        try {
            // Get the file size in bytes
            std::filesystem::path filePath(cacheFilePath);
            std::uintmax_t fileSizeInBytes = std::filesystem::file_size(filePath);
            std::uintmax_t cachesizeInBytes = maxCacheSize;

            // Convert to MB
            double fileSizeInMB = fileSizeInBytes / (1024.0 * 1024.0);
            double cachesizeInMb = cachesizeInBytes / (1024.0 * 1024.0);

            std::cout << "\nCapacity: " << std::fixed << std::setprecision(1) << fileSizeInMB << "MB" 
                      << "/" << std::setprecision(0) << cachesizeInMb << "MB" 
                      << " \nEntries: "<< countNonEmptyLines(cacheFilePath) 
                      << "\nLocation: " << "'" << cacheFilePath << "'\033[0;1m" << std::endl;
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "\n\033[1;91mError: " << e.what() << std::endl;
        }

        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);

    } else if (inputSearch == "!clr") {
        if (std::remove(cacheFilePath.c_str()) != 0) {
            std::cerr << "\n\001\033[1;91mError clearing IsoCache: \001\033[1;93m'" 
                      << cacheFilePath << "\001'\033[1;91m. File missing or inaccessible." << std::endl;
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
        } else {
            // Clean transformation cache for .iso entries
            for (auto it = transformationCache.begin(); it != transformationCache.end();) {
                const std::string& key = it->first;
                if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".iso") == 0) {
                    it = transformationCache.erase(it);
                } else {
                    ++it;
                }
            }

            std::cout << "\n\001\033[1;92mIsoCache cleared successfully\001\033[1;92m." << std::endl;
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
        }

    } else if (inputSearch == "!clr_paths" || inputSearch == "!clr_filter") {
        clearHistory(inputSearch);
        manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);

    } else if (inputSearch == "*auto_on" || inputSearch == "*auto_off") {
        // Create directory if it doesn't exist
        std::filesystem::path dirPath = std::filesystem::path(configPath).parent_path();
        if (!std::filesystem::exists(dirPath)) {
            if (!std::filesystem::create_directories(dirPath)) {
                std::cerr << "\n\033[1;91mFailed to create directory: \033[1;91m'\033[1;93m" 
                          << dirPath.string() << "\033[1;91m'.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
            }
        }

        std::map<std::string, std::string> config = readConfig(configPath);

        // Update the specific setting
        if (inputSearch == "*auto_on" || inputSearch == "*auto_off") {
            config["auto_update"] = (inputSearch == "*auto_on") ? "1" : "0";
        }

        // Write all settings back to file
        std::ofstream outFile(configPath);
        if (outFile.is_open()) {
            for (const auto& [key, value] : config) {
                outFile << key << " = " << value << "\n";
            }
            outFile.close();

            // Display appropriate message
            if (inputSearch == "*auto_on" || inputSearch == "*auto_off") {
                std::cout << "\n\033[0;1mAutomatic background updates have been "
                          << (inputSearch == "*auto_on" ? "\033[1;92menabled" : "\033[1;91mdisabled")
                          << "\033[0;1m.\033[0;1m\n";
            }
        } else {
            std::cerr << "\n\033[1;91mFailed to write configuration, unable to access: \033[1;91m'\033[1;93m" 
                      << configPath << "\033[1;91m'.\033[0;1m\n";
        }

        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);

    } else if (isValidInput(inputSearch)) {
        setDisplayMode(inputSearch);
        manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
    }
}


// Function for manual cache refresh
void manualRefreshCache(std::string& initialDir, bool promptFlag, int maxDepth, bool historyPattern, std::atomic<bool>& newISOFound) {
	
	enable_ctrl_d();
	// Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
        
    // Reset cancellation flag
    g_operationCancelled.store(false);
	
    // Centralize input handling
    std::string input = initialDir;
    if (input.empty()) {
        if (promptFlag) {
            clearScrollBuffer();
        }
        
        loadHistory(historyPattern);
        maxDepth = -1;
        
        const std::set<std::string> validInputs = {
			"*fl_m", "*cl_m", "*fl_u", "*cl_u", "*fl_fo", "*cl_fo", "*fl_w", "*cl_w", "*fl_c", "*cl_c"
		};
        
        // Restore readline autocomplete and screen clear bindings
        rl_bind_key('\f', rl_clear_screen);
		rl_bind_key('\t', rl_complete);
        bool isCpMv= false;
        // Prompt the user to enter directory paths for manual cache refresh
		std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ to scan for \001\033[1;92m\002.iso\001\033[1;94m\002 files and import them into \001\033[1;92m\002on-disk\001\033[1;94m\002 cache, ? ↵ for help, ↵ to return:\n\001\033[0;1m\002";
        char* rawSearchQuery = readline(prompt.c_str());
        

        // Handle EOF (Ctrl+D) scenario
        if (!rawSearchQuery) {
            input.clear();  // Explicitly clear input to trigger early exit
        } else {
            std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
            input = searchQuery.get();
			
			if (input == "?") {
				helpSearches(isCpMv);
				input = "";
				std::string dummyDir = "";
				manualRefreshCache(dummyDir, promptFlag, maxDepth, historyPattern, newISOFound);
			}        
			
            if (input == "stats" || input == "!clr" || input == "!clr_paths" || input == "!clr_filter" || input == "*auto_off" || input == "*auto_on" || isValidInput(input)) {
                cacheAndMiscSwitches(input, promptFlag, maxDepth, historyPattern, newISOFound);
                return;
            }
            
            if (!input.empty() && promptFlag) {
                add_history(searchQuery.get());
                std::cout << "\n";
            }
        }
    }

    // Early exit for empty or whitespace-only input
    if (std::all_of(input.begin(), input.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
        return;
    }

    // Combine path validation and processing
    std::vector<std::string> validPaths;
    std::set<std::string> invalidPaths;
    std::set<std::string> uniqueErrorMessages;
    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
	
	if (promptFlag) {
		disableInput();
	}
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Single-pass path processing with concurrent file traversal
    std::vector<std::future<void>> futures;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;

    std::istringstream iss(input);
    std::string path;
    std::size_t runningTasks = 0;
	
	
    
    while (std::getline(iss, path, ';')) {
        if (!isValidDirectory(path)) {
            if (promptFlag) {
                std::lock_guard<std::mutex> lock(processMutex);
                invalidPaths.insert(path);
            }
            continue;
        }

        validPaths.push_back(path);
        futures.emplace_back(std::async(std::launch::async, 
            [path, &allIsoFiles, &uniqueErrorMessages, &totalFiles, &processMutex, &traverseErrorMutex, &maxDepth, &promptFlag]() {
                traverse(path, allIsoFiles, uniqueErrorMessages, 
                         totalFiles, processMutex, traverseErrorMutex, maxDepth, promptFlag);
            }
        ));

        if (++runningTasks >= maxThreads) {
            for (auto& future : futures) {
                future.wait();
                if (g_operationCancelled.load()) break;
            }
            futures.clear();
            runningTasks = 0;
        }
    }

    // Wait for remaining tasks
    for (auto& future : futures) {
        future.wait();
        if (g_operationCancelled.load()) break;
    }
    
    // Post-processing
    if (promptFlag) {
		// Flush and Restore input after processing
		flushStdin();
		restoreInput();
			
        std::cout << "\r\033[0;1mTotal files processed: " << totalFiles;
        
        if (!invalidPaths.empty() || !validPaths.empty()) {
            std::cout << "\n";
        }

        if (validPaths.empty()) {
			input = "";
			clear_history();
            std::cout << "\033[1A\033[K";
        }
		if (!validPaths.empty() && !input.empty()) {
			saveHistory(historyPattern);
			clear_history();
		}
        verboseIsoCacheRefresh(allIsoFiles, totalFiles, validPaths, invalidPaths, 
                               uniqueErrorMessages, promptFlag, maxDepth, historyPattern, start_time, newISOFound);
    } else {
		if (!g_operationCancelled.load()) {
			// Save the combined cache to disk
			saveCache(allIsoFiles, maxCacheSize, newISOFound);
		}
		promptFlag = true;
		maxDepth = -1;
	}
}


// Function to traverse a directory and find ISO files
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag) {
    const size_t BATCH_SIZE = 100;
    std::vector<std::string> localIsoFiles;
    
    std::atomic<bool> g_CancelledMessageAdded{false};
    // Reset cancellation flag
    g_operationCancelled.store(false);


    auto iequals = [](const std::string_view& a, const std::string_view& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                         [](unsigned char a, unsigned char b) {
                             return std::tolower(a) == std::tolower(b);
                         });
    };

    try {
        auto options = std::filesystem::directory_options::none;
        for (auto it = std::filesystem::recursive_directory_iterator(path, options); 
			it != std::filesystem::recursive_directory_iterator(); ++it) {
			if (g_operationCancelled.load()) {
				if (!g_CancelledMessageAdded.exchange(true)) {
					std::lock_guard<std::mutex> lock(globalSetsMutex);
					uniqueErrorMessages.clear();
					uniqueErrorMessages.insert("\n\033[1;33mISO search interrupted by user.\033[0;1m");
				}
				break;
			}
                if (maxDepth >= 0 && it.depth() > maxDepth) {
                    it.disable_recursion_pending();
                    continue;
                }

                const auto& entry = *it;
                if (promptFlag && entry.is_regular_file()) {
                    totalFiles.fetch_add(1, std::memory_order_acq_rel);  // Simple increment of atomic counter
                    if (totalFiles % 100 == 0) { // Update display periodically
						std::lock_guard<std::mutex> lock(couNtMutex);
                        std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
                    }
                }

                if (!entry.is_regular_file()) continue;

                const auto& filePath = entry.path();
                if (!iequals(filePath.extension().string(), ".iso")) continue;

                localIsoFiles.push_back(filePath.string());

                if (localIsoFiles.size() >= BATCH_SIZE) {
                    std::lock_guard<std::mutex> lock(traverseFilesMutex);
                    isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
                    localIsoFiles.clear();
                }
			}

        // Merge leftovers
        if (!localIsoFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
        }

        // Merge errors
    } catch (const std::filesystem::filesystem_error& e) {
        std::string formattedError = "\n\033[1;91mError traversing directory: " + 
                                    path.string() + " - " + e.what() + "\033[0;1m";
        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
            uniqueErrorMessages.insert(formattedError);
        }
    }
}
