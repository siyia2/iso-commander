#include "../headers.h"

//	CACHE STUFF

// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_cache.txt";
const std::string cacheFileName = "iso_commander_cache.txt";
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB

int maxDepth = -1;


// Function to remove non-existent paths from cache
void removeNonExistentPathsFromCache() {
	
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
    std::ofstream updatedCacheFile(cacheFilePath, std::ios::out | std::ios::trunc);
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


// Set default cache dir
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}


// Load cache
void loadCache(std::vector<std::string>& isoFiles) {
    std::string cacheFilePath = getHomeDirectory() + "/.cache/iso_commander_cache.txt";
    
    // Check if the cache file exists
    struct stat fileStat;
    if (stat(cacheFilePath.c_str(), &fileStat) == -1) {
        // File doesn't exist, handle error or just return
        return;
    }
    
    // Check if the file is empty
    if (fileStat.st_size == 0) {
		isoFiles.clear();  // Clear the vector to ensure we don't retain old data
        return;  // File is empty, no need to process
    }

    // Open the file for memory mapping
    int fd = open(cacheFilePath.c_str(), O_RDONLY);
    if (fd == -1) {
        // Handle error if unable to open the file
        return;
    }

    // Get the file size
    const auto fileSize = fileStat.st_size;

    // Memory map the file
    char* mappedFile = static_cast<char*>(mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mappedFile == MAP_FAILED) {
        // Handle error if unable to map the file
        close(fd);
        return;
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

    // Convert the set to the vector reference
    isoFiles.assign(uniqueIsoFiles.begin(), uniqueIsoFiles.end());
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
    if (!std::filesystem::exists(cacheDirectory) || !std::filesystem::is_directory(cacheDirectory)) {
        return false;  // Cache save failed
    }

    // Load the existing cache into a local vector
    std::vector<std::string> existingCache;
    loadCache(existingCache);

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
            cacheFile.close();
            return false;  // Cache save failed
        }
    } else {
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
	traverse(path, newIsoFiles, uniqueErrorMessages);

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
	
	std::mutex cacheRefreshMutex;

	// Assuming promptFlag is defined elsewhere
	if (promptFlag) {
		clearScrollBuffer();
		gapPrinted = false;
	}

	std::string input;

	// Append the initial directory if provided
	if (!initialDir.empty()) {
		input = initialDir;
	} else {
		// Load history from file
		loadHistory();
		maxDepth = -1;
		// Prompt the user to enter directory paths for manual cache refresh
		std::string prompt = "\001\033[1;92m\002Folder path(s)\001\033[1;94m\002 ↵ to scan for \001\033[1;92m\002.iso\001\033[1;94m\002 files and import into \001\033[1;92m\002on-disk\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002),\001\033[1;93m\002 clr\001\033[1;94m\002 to clear \001\033[1m\002\001\033[1;92m\002on-disk\001\033[1m\002\001\033[1;94m\002 cache, or ↵ to return:\n\001\033[0;1m\002";
		// Prompt user for input
		char* rawSearchQuery = readline(prompt.c_str());

		// Use std::unique_ptr to manage memory for rawSearchQuery
		std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
		std::string inputSearch(searchQuery.get());
		
		if (inputSearch == "clr") {
			clearScrollBuffer();
			if (std::remove(cacheFilePath.c_str()) != 0) {
				std::cerr << "\n\001\033[1;91mError deleting file: '" << cacheFilePath << "'. File does not exist." << std::endl;
				std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
				std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
				manualRefreshCache("");
        } else {
			std::cout << "\n\001\033[1;92mFile deleted successfully: '" << cacheFilePath <<"'." << std::endl;
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			manualRefreshCache("");
        }
			
		} else if (!inputSearch.empty()) {
			input = inputSearch;
			add_history(searchQuery.get()); // Add to history
		}
	}

	// Check if the input line is empty or contains only spaces
	bool onlySpaces = std::all_of(input.begin(), input.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });

	if (input.empty() || onlySpaces) {
		return;
	}
	
	if (promptFlag) {
		// Save history
		saveHistory();
	}

    // Create an input string stream to parse directory paths
    std::istringstream iss(input);
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
		std::lock_guard<std::mutex> lock(cacheRefreshMutex);
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
    std::istringstream iss2(input); // Reset the string stream
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
            std::lock_guard<std::mutex> lock(cacheRefreshMutex);
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


// Case-insensitive string comparison (optimized)
bool iequals(const std::string_view& a, const std::string_view& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}


// Function to traverse a directory and find ISO files
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages) {
    try {
        // Set directory traversal options; initially none
        auto options = std::filesystem::directory_options::none;
        
        // If maxDepth is non-negative, include symlink directories in traversal
        if (maxDepth >= 0) {
            options |= std::filesystem::directory_options::follow_directory_symlink;
        }
        
        // Iterate through the directory recursively
        for (auto it = std::filesystem::recursive_directory_iterator(path, options); it != std::filesystem::recursive_directory_iterator(); ++it) {
            // If maxDepth is set and current depth exceeds it, skip further recursion
            if (maxDepth >= 0 && it.depth() > maxDepth) {
                it.disable_recursion_pending();
                continue;
            }
            
            const auto& entry = *it;
            // If the current entry is not a regular file, skip it
            if (!entry.is_regular_file()) {
                continue;
            }
            
            const auto& filePath = entry.path();
            const auto extension = filePath.extension();
            
            // Skip files that do not have a ".iso" extension
            if (!iequals(extension.string(), ".iso")) {
                continue;
            }
            
            const auto fileSize = entry.file_size();
            // Skip files smaller than 5 MB or with a size of 0
            if (fileSize < 5 * 1024 * 1024 || fileSize == 0) {
                continue;
            }
            
            // Add valid .iso file paths to the isoFiles vector
            isoFiles.push_back(filePath.string());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Catch any filesystem errors, format the error message, and add it to the set of unique error messages
        std::string formattedError = std::string("\n\033[1;91m") + e.what() + ".\033[0;1m";
        uniqueErrorMessages.insert(formattedError);
    }
}

