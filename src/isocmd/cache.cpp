#include "../headers.h"

//	CACHE STUFF

// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFileName = "iso_commander_cache.txt";;
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB


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
    const std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_cache.txt";

    // Open the cache file for reading
    std::ifstream cacheFile(cacheFilePath, std::ios::in | std::ios::binary);
    if (!cacheFile.is_open()) {
        // Handle error if unable to open cache file
        return;
    }

    // Get the file size
    const auto fileSize = cacheFile.seekg(0, std::ios::end).tellg();
    cacheFile.seekg(0, std::ios::beg);

    // Open the file for memory mapping
    int fd = open(cacheFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        // Handle error if unable to open the file
        return;
    }

    // Memory map the file
    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        // Handle error if unable to map the file
        close(fd);
        return;
    }

    // Process the memory-mapped file
    std::vector<std::string> cache;
    char* start = mappedFile;
    char* end = mappedFile + fileSize;
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        cache.emplace_back(start, lineEnd);
        start = lineEnd + 1;
    }

    // Unmap the file
    munmap(mappedFile, fileSize);
    close(fd);

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
        // Handle error if unable to open cache file for writing
        return;
    }

    // Write the retained paths to the updated cache file
    for (const std::string& path : retainedPaths) {
        updatedCacheFile << path << '\n';
    }

    // RAII: Close the updated cache file automatically when it goes out of scope
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
    std::string cacheFilePath = getHomeDirectory() + "/.cache/iso_commander_cache.txt";

    // Check if the cache file exists
    struct stat fileStat;
    if (stat(cacheFilePath.c_str(), &fileStat) == -1) {
        // File doesn't exist, return an empty vector
        return isoFiles;
    }

    // Open the file for memory mapping
    int fd = open(cacheFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        // Handle error if unable to open the file
        return isoFiles;
    }

    // Get the file size
    const auto fileSize = fileStat.st_size;

    // Memory map the file
    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        // Handle error if unable to map the file
        close(fd);
        return isoFiles;
    }

    // Use a set to store unique lines
    std::set<std::string> uniqueIsoFiles;

    // Process the memory-mapped file
    char* start = mappedFile;
    char* end = mappedFile + fileSize;
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        if (!line.empty()) {
            uniqueIsoFiles.insert(std::move(line));
        }
        start = lineEnd + 1;
    }

    // Unmap the file
    munmap(mappedFile, fileSize);
    close(fd);

    // Convert the set to a vector
    isoFiles.assign(uniqueIsoFiles.begin(), uniqueIsoFiles.end());
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
		std::cout << "\n";
        std::cerr << "\033[1;91mInvalid cache directory.\033[0;1m\n";
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
			std::cout << "\n";
            std::cerr << "\033[1;91mFailed to write to cache file.\033[0;1m\n";
            cacheFile.close();
            return false;  // Cache save failed
        }
    } else {
		std::cout << "\n";
        std::cerr << "\033[1;91mFailed to open ISO cache file: \033[1;93m'"<< cacheDirectory + "/" + cacheFileName <<"'\033[1;91m. Check read/write permissions.\033[0;1m\n";
        return false;  // Cache save failed
    }
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function to refresh the cache for a single directory
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles, std::set<std::string>& uniqueErrorMessages) {
	if (promptFlag) {
		std::cout << "\033[1;93mProcessing directory path: '" << path << "'.\033[0m"<< std::endl;
	}

	std::vector<std::string> newIsoFiles;

	// Perform the cache refresh for the directory (e.g., using parallelTraverse)
	parallelTraverse(path, newIsoFiles, uniqueErrorMessages);

	// Use a separate mutex for read/write access to allIsoFiles
	std::mutex allIsoFilesMutex;

	{
		// Acquire lock for checking gapPrinted and potential printing
		std::lock_guard<std::mutex> lock(allIsoFilesMutex);
		if (!gapPrinted && promptFlag) {
		std::cout << "\n";
		gapPrinted = true; // Set the flag to true
		}
	}

	// Append new entries to allIsoFiles under lock protection
	{
		std::lock_guard<std::mutex> lock(allIsoFilesMutex);
		allIsoFiles.insert(allIsoFiles.end(), newIsoFiles.begin(), newIsoFiles.end());
	}

	if (promptFlag) {
		std::cout << "\033[1;92mProcessed directory path: '" << path << "'.\033[0m" << std::endl;
	}
}


// Function for manual cache refresh
void manualRefreshCache(const std::string& initialDir) {
    if (promptFlag){
    clearScrollBuffer();
    gapPrinted = false;
	}
    // Load history from file
    loadHistory();

    std::string inputLine;

    // Append the initial directory if provided
    if (!initialDir.empty()) {
        inputLine = initialDir;
    } else {
        // Prompt the user to enter directory paths for manual cache refresh
        inputLine = readInputLine("\033[1;94mDirectory path(s) ↵ to build/refresh the \033[1m\033[1;92mISO Cache\033[94m (multi-path separator: \033[1m\033[1;93m;\033[0m\033[1;94m), or ↵ to return:\n\033[0;1m");
    }

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
    // Vector to store ISO unique input errors
    std::set<std::string> uniqueErrorMessages;

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
                if (promptFlag){
					invalidPaths.push_back("\033[1;91mInvalid directory path: '" + path + "'. Skipped from processing.\033[0m");
					processedInvalidPaths.insert(path);
				}
            }
        }
    }

    // Check if any invalid paths were encountered and add a gap
    if ((!invalidPaths.empty() || !validPaths.empty()) && promptFlag) {
		std::lock_guard<std::mutex> lock(Mutex4High);
        std::cout << "\n";
    }

    // Print invalid paths
    for (const auto& invalidPath : invalidPaths) {
        std::cout << invalidPath << std::endl;
    }
    
    if (!invalidPaths.empty() && !validPaths.empty() && promptFlag) {
        std::cout << "\n";
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
        futures.emplace_back(std::async(std::launch::async, refreshCacheForDirectory, path, std::ref(allIsoFiles), std::ref(uniqueErrorMessages)));

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
            std::lock_guard<std::mutex> lock(Mutex4High);
            std::cout << "\n";
            gapPrinted = false;
        }
    }

    // Wait for the remaining tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
    
    for (const auto& error : uniqueErrorMessages) {
        std::cout << error;
    }
    
    if (!uniqueErrorMessages.empty()) {
		std::cout << "\n";
	}
    
    // Save the combined cache to disk
    bool saveSuccess = saveCache(allIsoFiles, maxCacheSize);

    // Stop the timer after completing the cache refresh and removal of non-existent paths
    auto end_time = std::chrono::high_resolution_clock::now();
    
    if (promptFlag) {

    // Calculate and print the elapsed time
    if (!validPaths.empty() || (!invalidPaths.empty() && validPaths.empty())) {
    std::cout << "\n";
	}
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();

    // Print the time taken for the entire process in bold with one decimal place
    std::cout << "\033[1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0m\n";

    // Inform the user about the cache refresh status
    if (saveSuccess && !validPaths.empty() && invalidPaths.empty() && uniqueErrorMessages.empty()) {
        std::cout << "\n";
        std::cout << "\033[1;92mCache refreshed successfully.\033[0m";
        std::cout << "\n";
    } 
    if (saveSuccess && !validPaths.empty() && (!invalidPaths.empty() || !uniqueErrorMessages.empty())) {
        std::cout << "\n";
        std::cout << "\033[1;93mCache refreshed with error(s).\033[0m";
        std::cout << "\n";
    }
    if (saveSuccess && validPaths.empty() && !invalidPaths.empty()) {
        std::cout << "\n";
        std::cout << "\033[1;91mCache refresh failed due to missing valid path(s).\033[0m";
        std::cout << "\n";
    } 
    if (!saveSuccess) {
        std::cout << "\n";
        std::cout << "\033[1;91mCache refresh failed.\033[0m";
        std::cout << "\n";
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}
	uniqueErrorMessages.clear();
	promptFlag = true;
}


//Function to perform case-insensitive string comparison using std::string_view asynchronously
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
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages) {
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
                    futures.push_back(std::async(std::launch::async, [filePath, &isoFiles]() {
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
        std::string formattedError = std::string("\n\033[1;91m") + e.what() + ".\033[0;1m";
        uniqueErrorMessages.insert(formattedError);
        
    }
}
