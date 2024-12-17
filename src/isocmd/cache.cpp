// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Cache Variables

const std::string cacheDirectory = std::string(std::getenv("HOME")) + "/.cache"; // Construct the full path to the cache directory
const std::string cacheFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_cache.txt";
const std::string cacheFileName = "iso_commander_cache.txt";
const uintmax_t maxCacheSize = 10 * 1024 * 1024; // 10MB


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
bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered) {
    static std::filesystem::file_time_type lastModifiedTime;

    clearScrollBuffer();

    // Check if the cache file exists and has been modified
    bool needToReload = false;
    if (std::filesystem::exists(cacheFileName)) {
        std::filesystem::file_time_type currentModifiedTime = 
            std::filesystem::last_write_time(cacheFileName);

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

    if (needToReload) {
        removeNonExistentPathsFromCache();
        loadCache(globalIsoFileList);
        sortFilesCaseInsensitive(globalIsoFileList);
    }

    printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES");

    if (globalIsoFileList.empty()) {
        clearScrollBuffer();
        std::cout << "\n\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return false;
    }

    return true;
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


// Function that can delete or show stats for ISO cache it is called from within manualRefreshCache
void delCacheAndShowStats (std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& historyPattern) {
	if (inputSearch == "stats") {
		try {
			// Get the file size in bytes
			std::filesystem::path filePath(cacheFilePath);
			std::uintmax_t fileSizeInBytes = std::filesystem::file_size(filePath);
        
			// Convert to MB
			double fileSizeInMB = fileSizeInBytes / (1024.0 * 1024.0);
        
			std::cout << "\nSize: " << std::fixed << std::setprecision(1) << fileSizeInMB << "MB" << "/10MB." << " \nEntries: "<< countNonEmptyLines(cacheFilePath) << "\nLocation: " << "'" << cacheFilePath << "'\033[0;1m." <<std::endl;
		} catch (const std::filesystem::filesystem_error& e) {
			std::cerr << "\n\033[1;91mError: " << e.what() << std::endl;
		}
		
		std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		manualRefreshCache("", promptFlag, maxDepth, historyPattern);
		
	} else if (inputSearch == "clr") {
		if (std::remove(cacheFilePath.c_str()) != 0) {
			std::cerr << "\n\001\033[1;91mError deleting IsoCache: '\001\033[1;93m" << cacheFilePath << "\001\033[1;91m'. File missing or inaccessible." << std::endl;
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			manualRefreshCache("", promptFlag, maxDepth, historyPattern);
		} else {
			for (auto it = transformationCache.begin(); it != transformationCache.end();) {
				const std::string& key = it->first;
				if ((key.size() >= 4 && key.compare(key.size() - 4, 4, ".iso") == 0))
					{
						it = transformationCache.erase(it);  // erase and move to the next element
					} else {
						++it;  // move to the next element
					}
			}
			
			std::cout << "\n\001\033[1;92mIsoCache deleted successfully: '\001\033[0;1m" << cacheFilePath <<"\001\033[1;92m'." << std::endl;
			std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			manualRefreshCache("", promptFlag, maxDepth, historyPattern);
		}
	}
	return;
}


// Function for manual cache refresh
void manualRefreshCache(const std::string& initialDir, bool promptFlag, int maxDepth, bool historyPattern) {
    // Centralize input handling
    std::string input = initialDir;
    if (input.empty()) {
        if (promptFlag) {
            clearScrollBuffer();
        }
        
        loadHistory(historyPattern);
        maxDepth = -1;
        
        // Restore readline autocomplete and screen clear bindings
        rl_bind_key('\f', rl_clear_screen);
		rl_bind_key('\t', rl_complete);
        
        // Prompt the user to enter directory paths for manual cache refresh
		std::string prompt = "\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ to scan for \001\033[1;92m\002.iso\001\033[1;94m\002 files (>= 5MB) and import into \001\033[1;92m\002on-disk\001\033[1;94m\002 cache (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002),\001\033[1;93m\002 clr\001\033[1;94m\002 ↵ clear \001\033[1m\002\001\033[1;92m\002on-disk\001\033[1m\002\001\033[1;94m\002 cache, \001\033[1;95m\002stats\001\033[1;94m\002 ↵ cache\001\033[1m\002\001\033[1;94m\002 stats, ↵ return:\n\001\033[0;1m\002";
        char* rawSearchQuery = readline(prompt.c_str());
        
        std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
        input = searchQuery.get();
        
        
        if (input == "stats" || input == "clr") {
			delCacheAndShowStats(input, promptFlag, maxDepth, historyPattern);
			return;
		}
		
        if (!input.empty()) {
			add_history(searchQuery.get());
			std::cout << "\n";
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

    disableInput();
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
            }
            futures.clear();
            runningTasks = 0;
        }
    }

    // Wait for remaining tasks
    for (auto& future : futures) {
        future.wait();
    }
    
     // Flush and Restore input after processing
    flushStdin();
    restoreInput();

    // Post-processing
    if (promptFlag) {
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
                               uniqueErrorMessages, promptFlag, maxDepth, historyPattern, start_time);
    } else {
		// Save the combined cache to disk
		saveCache(allIsoFiles, maxCacheSize);
		promptFlag = true;
		maxDepth = -1;
	}
}


// Case-insensitive string comparison (optimized)
bool iequals(const std::string_view& a, const std::string_view& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}


// Function to traverse a directory and find ISO files
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag) {
    
    const size_t BATCH_SIZE = 100; // Adjust batch size as needed

    // Local batch collection for ISO files
    std::vector<std::string> localIsoFiles;

    // Local leftover storage for final merging
    std::vector<std::string> leftoverFiles;

    try {
        auto options = std::filesystem::directory_options::none;

        for (auto it = std::filesystem::recursive_directory_iterator(path, options); it != std::filesystem::recursive_directory_iterator(); ++it) {
            try {
                if (maxDepth >= 0 && it.depth() > maxDepth) {
                    it.disable_recursion_pending();
                    continue;
                }

                const auto& entry = *it;
				if (promptFlag){
					if (entry.is_regular_file()) {
							totalFiles++;
							std::cout << "\r\033[0;1mTotal files processed: " << totalFiles << std::flush;
					}
				}
					
                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto& filePath = entry.path();
                const auto extension = filePath.extension();

                if (!iequals(extension.string(), ".iso")) {
                    continue;
                }

                const auto fileSize = entry.file_size();
                if (fileSize < 5 * 1024 * 1024 || fileSize == 0) {
                    continue;
                }

                // Collect ISO files in a local batch
                localIsoFiles.push_back(filePath.string());

                // When batch reaches specified size, add to shared vector
                if (localIsoFiles.size() >= BATCH_SIZE) {
                    {
                        std::lock_guard<std::mutex> lock(traverseFilesMutex);
                        isoFiles.insert(isoFiles.end(), localIsoFiles.begin(), localIsoFiles.end());
                    }
                    localIsoFiles.clear();
                }
            } catch (const std::filesystem::filesystem_error& entryError) {
                std::string formattedError = std::string("\n\033[1;91mError processing path: ") +
                                              it->path().string() + " - " + entryError.what() + "\033[0;1m";
                if (promptFlag){
					std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
					uniqueErrorMessages.insert(formattedError);
				}
			}
        }
        
        if (totalFiles == 0 && promptFlag) {
			std::cout << "\r\033[0;1mTotal files processed: 0" << std::flush;
		}

        // Store any remaining local ISO files in leftover storage
        leftoverFiles = std::move(localIsoFiles);

    } catch (const std::filesystem::filesystem_error& e) {
        std::string formattedError = std::string("\n\033[1;91mError traversing directory: ") +
                                      path.string() + " - " + e.what() + "\033[0;1m";
        if (promptFlag){
			std::lock_guard<std::mutex> errorLock(traverseErrorsMutex);
			uniqueErrorMessages.insert(formattedError);
		}
    }

    // Merge leftovers into shared vector at the end
    if (!leftoverFiles.empty()) {
        std::lock_guard<std::mutex> lock(traverseFilesMutex);
        isoFiles.insert(isoFiles.end(), leftoverFiles.begin(), leftoverFiles.end());
    }
}
