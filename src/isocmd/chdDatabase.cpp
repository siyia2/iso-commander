
#include "../headers.h"
#include "../threadpool.h"
#include "../themes.h"
#include "../readline.h"

// Local ISO Database mutex
namespace {
    std::mutex dbFileMutex;
}


void removeNonExistentChdPathsFromDatabase(std::vector<std::string>& globalChdFileList) {
    std::vector<std::string> retained;
    bool anyRemoved = false;
    
    // Construct the path specifically for the CHD database
    std::filesystem::path chdPath = databaseDirectory;
    chdPath /= databaseCHDFilename;

    {
        std::lock_guard<std::mutex> fileLock(dbFileMutex);

        int fd = open(chdPath.c_str(), O_RDWR, 0644);
        if (fd == -1) {
            if (errno == ENOENT) {
                // If database is missing, clear the runtime list
                std::lock_guard<std::mutex> lock(updateListMutex);
                globalChdFileList.clear();
            }
            return;
        }

        struct FdGuard {
            int fd;
            ~FdGuard() { if (fd != -1) { flock(fd, LOCK_UN); close(fd); } }
            void release() { fd = -1; }
        } fdGuard{fd};

        if (flock(fd, LOCK_EX) == -1) return;

        int dupFd = dup(fd);
        if (dupFd == -1) return;

        FILE* file = fdopen(dupFd, "r");
        if (!file) { close(dupFd); return; }

        // 1. Load CHD cache into memory
        std::vector<std::string> cache;
        char* linePtr = nullptr;
        size_t len    = 0;
        while (getline(&linePtr, &len, file) != -1) {
            std::string line(linePtr);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (!line.empty() && line.back() == '\r') line.pop_back(); 
            if (!line.empty()) cache.push_back(std::move(line));
        }
        free(linePtr);
        fclose(file);

        if (cache.empty()) return;

        // 2. Multithreaded validation of CHD file paths
        ThreadPool& pool = getStaticThreadPool();
        std::vector<int> pathExists(cache.size(), 0);
        std::atomic<size_t> existingCount{0};
        
        const size_t numThread = std::min({
            pool.threadCount(),
            static_cast<size_t>(CLEAN_THREAD_CAP),
            cache.size()
        });

        const size_t chunkSize = (cache.size() + numThread - 1) / numThread;
        std::vector<std::future<void>> futures;
        futures.reserve(numThread);

        for (size_t i = 0; i < numThread; ++i) {
            const size_t start = i * chunkSize;
            const size_t end   = std::min(cache.size(), start + chunkSize);
            if (start >= end) break;
            
            futures.emplace_back(
                pool.enqueue([&cache, &pathExists, &existingCount, start, end] {
                    for (size_t j = start; j < end; ++j) {
                        // CHD files are often on external storage; access() is fast for this
                        if (access(cache[j].c_str(), F_OK) == 0) {
                            pathExists[j] = 1;
                            existingCount.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }));
        }

        for (auto& f : futures) f.get();

        // 3. Exit if no stale entries were found
        if (existingCount == cache.size()) return;

        // 4. Prepare updated buffer
        const size_t surviving = existingCount.load();
        retained.reserve(surviving);
        size_t totalBufferSize = 0;
        for (size_t i = 0; i < cache.size(); ++i) {
            if (pathExists[i]) {
                totalBufferSize += cache[i].size() + 1;
                retained.push_back(std::move(cache[i]));
            }
        }
        anyRemoved = true;

        std::string buf;
        buf.reserve(totalBufferSize);
        for (const auto& path : retained) {
            buf += path;
            buf += '\n';
        }

        // 5. Atomic-style overwrite of the CHD text database
        if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) return;

        ssize_t written = ::write(fd, buf.data(), buf.size());
        if (written == -1 || static_cast<size_t>(written) != buf.size()) return;

        // Mark CHD list as dirty for the UI/Browser
        chdListDirty.store(true); 
        
        fdGuard.release();
        flock(fd, LOCK_UN);
        close(fd);
    }

    // 6. Final synchronization with the global CHD vector
    if (anyRemoved) {
        std::lock_guard<std::mutex> lock(updateListMutex);
        globalChdFileList = std::move(retained);
    }
}

void loadChdFromDatabase(std::vector<std::string>& outChdList) {
    // Construct the path for the CHD database
    std::filesystem::path chdPath = databaseDirectory;
    chdPath /= databaseCHDFilename;

    std::lock_guard<std::mutex> fileLock(dbFileMutex);
    
    // 1. Open specifically for the CHD database file
    int fd = open(chdPath.c_str(), O_RDONLY);
    if (fd == -1) return;
    
    // 2. Apply Shared Lock (allows multiple simultaneous readers)
    // This prevents reading while a write/cleanup is in progress
    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }
    
    // 3. Check file size and validity
    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1 || fileStat.st_size == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        outChdList.clear();
        return;
    }
    
    // 4. Read the entire file into a local buffer for fast parsing
    std::vector<char> buffer(fileStat.st_size);
    ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);
    
    // Release file resources as soon as the read is complete
    flock(fd, LOCK_UN);
    close(fd);
    
    if (bytesRead <= 0 || bytesRead > fileStat.st_size) return;
    
    // 5. Parse lines from the buffer
    std::vector<std::string> loadedChds;
    char* start = buffer.data();
    char* end = buffer.data() + bytesRead;
    
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        
        // Clean up potential Windows-style carriage returns (\r\n)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (!line.empty()) {
            loadedChds.push_back(std::move(line));
        }
        
        start = (lineEnd < end) ? lineEnd + 1 : end;
    }

    // 6. Move the local list into the output vector (O(1) operation)
    outChdList = std::move(loadedChds);
}

bool saveChdToDatabase(std::vector<std::string> globalChdFileList, std::atomic<bool>& newCHDFound) {
    std::filesystem::path cachePath = databaseDirectory;
    cachePath /= databaseCHDFilename; // Use a specific filename for CHDs

    // 1. Ensure Directory Exists
    if (!std::filesystem::exists(databaseDirectory) && !std::filesystem::create_directories(databaseDirectory)) {
        return false;
    }
    if (!std::filesystem::is_directory(databaseDirectory)) {
        return false;
    }

    // 2. Lock and Open File
    std::lock_guard<std::mutex> fileLock(dbFileMutex);

    int fd = open(cachePath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd == -1) return false;

    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return false;
    }

    // 3. Read Existing Cache into Vector
    std::vector<std::string> existingCache;
    struct stat fileStat;
    if (fstat(fd, &fileStat) == 0 && fileStat.st_size > 0) {
        std::vector<char> buffer(fileStat.st_size);
        ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);

        if (bytesRead > 0) {
            char* start = buffer.data();
            char* end = buffer.data() + bytesRead;

            while (start < end) {
                char* lineEnd = std::find(start, end, '\n');
                std::string line(start, lineEnd);
                
                // Handle CRLF if necessary
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    existingCache.push_back(std::move(line));
                }
                start = (lineEnd < end) ? lineEnd + 1 : end;
            }
        }
    }

    // 4. Identify New Entries using a Set for O(1) lookups
    std::unordered_set<std::string> existingSet(existingCache.begin(), existingCache.end());
    std::vector<std::string> newEntries;
    bool localNewCHDFound = false;

    for (const auto& chd : globalChdFileList) {
        if (existingSet.find(chd) == existingSet.end()) {
            newEntries.push_back(chd);
            localNewCHDFound = true;
        }
    }

    // 5. Early Exit if No Changes
    if (newEntries.empty()) {
        newCHDFound.store(false);
        flock(fd, LOCK_UN);
        close(fd);
        return false; 
    }

    // 6. Merge and Truncate to Max Size
    std::vector<std::string> combinedCache = std::move(existingCache);
    combinedCache.insert(combinedCache.end(), newEntries.begin(), newEntries.end());
    
    if (combinedCache.size() > maxDatabaseSize) {
        combinedCache.erase(combinedCache.begin(), 
                           combinedCache.begin() + (combinedCache.size() - maxDatabaseSize));
    }

    // 7. Write back to Disk
    if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }

    bool success = true;
    for (const auto& entry : combinedCache) {
        std::string line = entry + "\n";
        if (::write(fd, line.data(), line.size()) == -1) {
            success = false;
            break;
        }
    }

    // 8. Cleanup and Update Flags
    flock(fd, LOCK_UN);
    close(fd);

    newCHDFound.store(localNewCHDFound);
    if (success) chdListDirty.store(true); 

    return success;
}

int countDifferentChdEntries(const std::vector<std::string>& allChdFiles, const std::vector<std::string>& globalChdFileList) {
    // Using string_view for the set to avoid re-allocating strings already in memory
    std::unordered_set<std::string_view> globalSet;
    globalSet.reserve(globalChdFileList.size());

    // Populate the set with existing CHD entries
    for (const auto& file : globalChdFileList) {
        globalSet.insert(file);
    }

    int count = 0;
    // Check how many files in the newly scanned list are not in the current database
    for (const auto& file : allChdFiles) {
        if (globalSet.find(file) == globalSet.end()) {
            count++;
        }
    }

    return count;
}

void verboseChdForDatabase(std::vector<std::string>& allChdFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newCHDFound) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // Theme-based color mapping
    std::string_view errLabel    = isOriginal ? originalColors::red       : theme->secondary;
    std::string_view warnLabel   = isOriginal ? originalColors::yellow    : theme->warning;
    std::string_view okLabel     = isOriginal ? originalColors::green     : theme->accent;
    std::string_view importColor = isOriginal ? originalColors::magenta   : theme->highlight;
    std::string_view boldLabel   = isOriginal ? originalColors::boldAlt   : theme->muted;

    // Load current CHD list to compare entries for the final "count"
    loadChdFromDatabase(globalChdFileList);

    auto printInvalidPaths = [&]() {
        if (invalidPaths.empty()) return;
        if (totalFiles == 0 && validPaths.empty()) {
            std::cout << "\r" << boldLabel << "Total files processed: 0\n" << std::flush;
        }
        std::cout << "\n" << boldLabel << "Invalid paths omitted from CHD search: " << errLabel;
        for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
            std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
        }
        std::cout << boldLabel << ".\n";
    };

    auto printErrorMessages = [&]() {
        if (uniqueErrorMessages.empty()) return;
        for (const auto& error : uniqueErrorMessages) std::cout << error;
        std::cout << "\n";
    };

    if (promptFlag && (!uniqueErrorMessages.empty() || !invalidPaths.empty())) {
        printInvalidPaths();
        printErrorMessages();
    }

    // Attempt to save new CHD entries to the database
    const bool saveSuccess = g_operationCancelled ? false : saveChdToDatabase(allChdFiles, newCHDFound);
    const auto end_time = std::chrono::high_resolution_clock::now();

    if (!promptFlag) return;

    const double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << boldLabel << "\nTotal time taken: " << std::fixed << std::setprecision(1)
              << total_elapsed << " seconds\n";

    // Status logic adapted for CHD naming
    if (g_operationCancelled) {
        std::cout << "\n" << okLabel << "CHD Database Refresh: [" << warnLabel << "Cancelled" << okLabel << "]" << boldLabel << "\n";
    } else if (!allChdFiles.empty() && newCHDFound.load() && !saveSuccess) {
        std::cout << "\n" << errLabel << "CHD Database Refresh failed: [" << warnLabel << "Unable to access database file" << errLabel << "]" << boldLabel << "\n";
    } else if (validPaths.empty()) {
        std::cout << "\n" << errLabel << "CHD Database refresh failed: [" << warnLabel << "Lack of valid paths" << errLabel << "]" << boldLabel << "\n";
    } else if (!allChdFiles.empty() && !newCHDFound.load() && !saveSuccess) {
        std::cout << "\n" << okLabel << "CHD Database Refresh: [" << warnLabel << "No new CHD found" << okLabel << "]" << boldLabel << "\n";
    } else if (allChdFiles.empty()) {
        std::cout << "\n" << okLabel << "CHD Database Refresh: [" << warnLabel << "No CHD found" << okLabel << "]" << boldLabel << "\n";
    } else if (!allChdFiles.empty() && saveSuccess && newCHDFound.load()) {
        // Calculate the actual number of new entries added to the list
        int result = countDifferentChdEntries(allChdFiles, globalChdFileList);
        std::cout << "\n" << okLabel << "CHD Database Refresh: [" << importColor << result << " CHD imported" << okLabel << "]" << boldLabel << "\n";
    }

    std::cout << color << "\n↵ to continue..." << reset; 
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    // Recurse back to CHD refresh menu
    std::string initialDir = ""; 
    refreshChdForDatabase(initialDir, promptFlag, maxDepth, filterHistory, newCHDFound);
}


void refreshChdForDatabase(std::string& initialDir, bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newCHDFound) {
    try {
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        resetReadlinePagination();
        rl_attempted_completion_function = my_special_completion_entry;
        g_operationCancelled.store(false);
        
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        std::string input = initialDir;
        if (input.empty()) {
            if (promptFlag) clearScrollBuffer();
            loadHistory(filterHistory);

            rl_bind_key('\f', clear_screen_and_buffer);
            rl_bind_key('\t', rl_complete);
            
            std::string_view primary = isOrig ? originalColors::green : theme->accent;
            std::string_view secondary = isOrig ? originalColors::blue : theme->muted;

            // Prompt specifically tailored for CHD files
            std::string prompt;
            prompt.reserve(512);
            prompt.append("\001").append(primary).append("\002FolderPaths")
                  .append("\001").append(secondary).append("\002 ↵ to scan for ")
                  .append("\001").append(primary).append("\002.chd")
                  .append("\001").append(secondary).append("\002 entries and import into ")
                  .append("\001").append(primary).append("\002local CHD")
                  .append("\001").append(secondary).append("\002 database, ? ↵ help, ↵ return:\n")
                  .append("\001").append(originalColors::boldAlt).append("\002");

            char* rawSearchQuery = readline(prompt.c_str());
            if (!rawSearchQuery) {
                input.clear();
            } else {
                std::unique_ptr<char, decltype(&std::free)> searchQuery(rawSearchQuery, &std::free);
                input = trimWhitespace(searchQuery.get());
                
                if (input == "?") {
                    bool isCpMv = false, import2ISO = false; // Note: import2ISO false implies CHD/Other
                    helpSearches(isCpMv, import2ISO);
                    std::string dummy = "";
                    return refreshChdForDatabase(dummy, promptFlag, maxDepth, filterHistory, newCHDFound);
                }
                
                if (input.starts_with("*") || input.starts_with("?") || input.starts_with("!") || isValidInput(input)) {
                    // Note: You may need a databaseChdSwitches if the logic differs significantly
                    databaseSwitches(input, promptFlag, maxDepth, filterHistory, newCHDFound);
                    return;
                }
                
                if (!input.empty() && promptFlag) {
                    add_history(input.c_str());
                    std::cout << "\n";
                }
            }
        }

        if (input.find_first_not_of(" \t\n\r") == std::string::npos) return;
        
        std::unordered_set<std::string> uniquePaths;
        std::vector<std::string> validPaths;
        std::unordered_set<std::string> invalidPaths;
        std::unordered_set<std::string> uniqueErrorMessages;
        std::vector<std::string> allChdFiles;
        std::atomic<size_t> totalFiles{0};

        if (promptFlag) {
            std::cout << "\033[3H\033[J\n";
            disableInput();
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        std::istringstream iss(input);
        std::string path;

        while (std::getline(iss, path, ';')) {
            if (!isValidDirectory(path)) {
                if (promptFlag) {
                    std::string errCol = isOrig ? std::string(originalColors::red) : std::string(theme->secondary);
                    invalidPaths.insert(errCol + path);
                }
                continue;
            }

            if (uniquePaths.insert(path).second) {
                validPaths.push_back(path);
            }
        }
        
        if (validPaths.empty()) {
            if (promptFlag) {
                flushStdin();
                restoreInput();
                resetReadlinePagination();
                if (!invalidPaths.empty()) {
                    verboseChdForDatabase(allChdFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newCHDFound);
                }
            }  
            return;
        }
        
        {
            auto& pool = getStaticThreadPool();
            std::vector<std::future<void>> futures;
            std::mutex processMutex;
            std::mutex traverseErrorMutex;

            for (const auto& validPath : validPaths) {
                futures.emplace_back(
                    pool.enqueue([validPath, &allChdFiles, &uniqueErrorMessages, &totalFiles, 
                                  &processMutex, &traverseErrorMutex, &maxDepth, &promptFlag]() {
                        // Traverse specifically looking for .chd files
                        traverseChd(validPath, allChdFiles, uniqueErrorMessages, totalFiles, 
                                   processMutex, traverseErrorMutex, maxDepth, promptFlag);
                    })
                );
            }

            for (auto& future : futures) {
                future.wait();
                if (g_operationCancelled.load()) break;
            }
        }
        
        if (!g_operationCancelled.load()) {
            std::unordered_set<std::string> uniqueFiles(allChdFiles.begin(), allChdFiles.end());
            allChdFiles.assign(uniqueFiles.begin(), uniqueFiles.end());
        }
        
        if (promptFlag) {
            flushStdin();
            restoreInput();
            resetReadlinePagination();
                
            std::cout << "\r" << (isOrig ? originalColors::boldAlt : theme->accent) 
                      << "Total CHD files processed: " << totalFiles;
            
            if (!invalidPaths.empty() || !validPaths.empty()) {
                std::cout << "\n";
            }

            if (validPaths.empty()) {
                input = "";
                clear_history();
                std::cout << "\033[1A\033[K";
            }
            if (!validPaths.empty() && !input.empty()) {
                saveHistory(filterHistory);
                clear_history();
            }
            verboseChdForDatabase(allChdFiles, totalFiles, validPaths, invalidPaths, uniqueErrorMessages, promptFlag, maxDepth, filterHistory, start_time, newCHDFound);
        } else {
            if (!g_operationCancelled.load()) {
                // Save specifically to the CHD database
                saveChdToDatabase(allChdFiles, newCHDFound);
            }
        }
    } catch (const std::exception& e) {
        const ListTheme* theme = getActiveTheme();
        const bool isOrig = (globalTheme == "original");

        std::cerr << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "Unable to access CHD database: " << e.what() << originalColors::boldAlt << std::endl;
        
        std::cout << color << "\n↵ to continue..." << reset; 

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::string dummyDir = "";
        refreshChdForDatabase(dummyDir, promptFlag, maxDepth, filterHistory, newCHDFound);
    }
}

void traverseChd(const std::filesystem::path& path, std::vector<std::string>& chdFiles, 
                std::unordered_set<std::string>& uniqueErrorMessages, 
                std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, 
                std::mutex& traverseErrorsMutex, int maxDepth, bool promptFlag) {
    
    const size_t BATCH_SIZE = 100; 
    std::vector<std::string> localChdFiles; 
    std::atomic<bool> g_CancelledMessageAdded{false}; 
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    g_operationCancelled.store(false);

    // Case-insensitive extension comparison
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
            
            // Check for user cancellation (SIGINT)
            if (g_operationCancelled.load()) {
                if (!g_CancelledMessageAdded.exchange(true)) {
                    std::lock_guard<std::mutex> lock(traverseErrorsMutex);
                    uniqueErrorMessages.clear(); 
                    
                    std::string warnCol = std::string(isOriginal ? originalColors::yellow : theme->warning);
                    std::string msg = "\n" + warnCol + "CHD search interrupted by user." + std::string(originalColors::boldAlt);
                    uniqueErrorMessages.insert(msg);
                }
                break;
            }

            // Depth control
            if (maxDepth >= 0 && it.depth() > maxDepth) {
                it.disable_recursion_pending(); 
                continue;
            }

            const auto& entry = *it; 

            // Update UI progress if prompt is enabled
            if (promptFlag && entry.is_regular_file()) {
                totalFiles.fetch_add(1, std::memory_order_acq_rel); 
                if (totalFiles % 100 == 0) { 
                    std::lock_guard<std::mutex> lock(couNtMutex); 
                    std::cout << "\r" << (isOriginal ? originalColors::boldAlt : theme->accent) 
                              << "Total files processed: " << totalFiles << std::flush;
                }
            }

            if (!entry.is_regular_file()) continue;

            const auto& filePath = entry.path(); 

            // Filter specifically for .chd extension
            if (!iequals(filePath.extension().string(), ".chd")) continue;

            localChdFiles.push_back(filePath.string());

            // Batch insertion to minimize mutex contention
            if (localChdFiles.size() >= BATCH_SIZE) {
                std::lock_guard<std::mutex> lock(traverseFilesMutex); 
                chdFiles.insert(chdFiles.end(), localChdFiles.begin(), localChdFiles.end()); 
                localChdFiles.clear(); 
            }
        }

        // Finalize remaining files in the local buffer
        if (!localChdFiles.empty()) {
            std::lock_guard<std::mutex> lock(traverseFilesMutex);
            chdFiles.insert(chdFiles.end(), localChdFiles.begin(), localChdFiles.end());
        }

    } catch (const std::filesystem::filesystem_error& e) {
        std::string errCol = std::string(isOriginal ? originalColors::red : theme->secondary);
        std::string formattedError = "\n" + errCol + "Error: " + path.string() + " - " + 
                                     e.what() + std::string(originalColors::boldAlt);
        
        if (promptFlag) {
            std::lock_guard<std::mutex> errorLock(traverseErrorsMutex); 
            uniqueErrorMessages.insert(formattedError); 
        }
    }
}
