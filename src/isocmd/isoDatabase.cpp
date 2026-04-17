// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"
#include "../themes.h"

// Local ISO Database mutex
namespace {
    std::mutex dbFileMutex;
}

/**
 * @brief Removes non-existent ISO paths from the database and in-memory cache
 * 
 * This function reads the database file, checks each path for existence using
 * parallel filesystem access, and rewrites the database with only valid paths.
 * 
 * @param globalIsoFileList Reference to the in-memory list of ISO files to update
 */
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList)
{
    std::vector<std::string> retained;
    bool anyRemoved = false;
    {
        std::lock_guard<std::mutex> fileLock(dbFileMutex);

        int fd = open(databaseFilePath.c_str(), O_RDWR, 0644);
        if (fd == -1) {
            if (errno == ENOENT) {
                std::lock_guard<std::mutex> lock(updateListMutex);
                globalIsoFileList.clear();
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

        std::vector<std::string> cache;
        char* linePtr = nullptr;
        size_t len    = 0;
        while (getline(&linePtr, &len, file) != -1) {
            std::string line(linePtr);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (!line.empty()) cache.push_back(std::move(line));
        }
        free(linePtr);
        fclose(file);

        if (cache.empty()) return;

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
                        if (access(cache[j].c_str(), F_OK) == 0) {
                            pathExists[j] = 1;
                            existingCount.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }));
        }

        for (auto& f : futures) f.get();

        if (existingCount == cache.size()) return;

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

        if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) return;

        ssize_t written = ::write(fd, buf.data(), buf.size());
        if (written == -1 || static_cast<size_t>(written) != buf.size()) return;
		isoListDirty.store(true);
        fdGuard.release();
        flock(fd, LOCK_UN);
        close(fd);
    }

    if (anyRemoved) {
        std::lock_guard<std::mutex> lock(updateListMutex);
        globalIsoFileList = std::move(retained);
    }
}

/**
 * @brief Counts non-empty lines in a file for statistics display
 * 
 * @param filePath Path to the file to analyze
 * @return Number of non-empty lines, or -1 if file cannot be opened
 */
int countNonEmptyLines(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Unable to open file: " << filePath << std::endl;
        return -1;
    }

    int nonEmptyLineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.find_first_not_of(" \t\n\r\f\v") != std::string::npos) {
            ++nonEmptyLineCount;
        }
    }

    file.close();
    return nonEmptyLineCount;
}

/**
 * @brief Gets the user's home directory path
 * 
 * @return Home directory path string, or empty string if not found
 */
std::string getHomeDirectory() {
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir);
    }
    return "";
}

/**
 * @brief Passes successfully operated ISO file paths to the database.
 *
 * Accepts a semicolon-separated list of destination paths produced by a
 * completed copy or move operation. Each non-empty path is collected and
 * forwarded to saveToDatabase() in a single batch call.
 *
 * @param filePathsStr  Semicolon-delimited string of destination ISO paths.
 * @param newISOFound   Atomic flag set to @c true by saveToDatabase() if at
 *                      least one new ISO entry is added.
 *
 * @note Paths are assumed valid — they were written successfully by the
 *       preceding operation. No filesystem validation is performed.
 */
void updateDatabaseAfterOperations(const std::string& filePathsStr, 
                                    std::atomic<bool>& newISOFound) {
    std::vector<std::string> allIsoFiles;
    std::istringstream iss(filePathsStr);
    std::string path;

    while (std::getline(iss, path, ';')) {
        if (!path.empty())
            allIsoFiles.push_back(std::move(path));
    }

    if (!allIsoFiles.empty())
        saveToDatabase(allIsoFiles, newISOFound);
}

/**
 * @brief Reduces hierarchical paths by grouping related directories
 * 
 * This function groups paths by their first 3 directory levels and reduces
 * redundant parent paths to optimize directory traversal.
 * 
 * @param paths Vector of semicolon-delimited path strings
 * @return Vector of reduced/optimized path strings
 */
std::vector<std::string> hierarchicalPathReduction(const std::vector<std::string>& paths) {
    std::map<std::string, std::vector<std::string>> pathGroups;
    std::vector<std::string> allPaths;
    
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
    
    for (const auto& path : allPaths) {
        size_t slashCount = 0, pos = 0;
        
        for (size_t i = 1; i < path.length() && slashCount < 3; ++i) {
            if (path[i] == '/') {
                pos = i;
                slashCount++;
            }
        }
        
        std::string key = (slashCount >= 3) ? path.substr(0, pos + 1) : path;
        pathGroups[key].push_back(path);
    }
    
    std::vector<std::string> finalPaths;
    for (const auto& [prefix, groupPaths] : pathGroups) {
        finalPaths.push_back((groupPaths.size() > 1) ? prefix : groupPaths[0]);
    }
    
    std::sort(finalPaths.begin(), finalPaths.end());
    std::vector<std::string> result;
    
    for (const auto& path : finalPaths) {
        int levels = std::count(path.begin() + 1, path.end(), '/');
        
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

/**
 * @brief Performs background ISO file import without blocking the UI
 * 
 * Reads history paths, reduces them hierarchically, traverses directories
 * to find ISO files, and saves them to the database.
 * 
 * @param isImportRunning Atomic flag indicating if import is in progress
 * @param newISOFound Atomic flag set to true if new ISOs were found
 */
void backgroundDatabaseImport(std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    std::vector<std::string> paths;
    int localMaxDepth = -1;
    bool localPromptFlag = false;
    
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
    
    if (paths.size() > 1) {
        auto it = std::find(paths.begin(), paths.end(), "/");
        if (it != paths.end()) {
            paths.erase(it);
        }
    }
    
    std::vector<std::string> finalPaths = hierarchicalPathReduction(paths);
    
    if (finalPaths.empty()) {
        isImportRunning.store(false);
        return;
    }

    std::vector<std::string> allIsoFiles;
    std::atomic<size_t> totalFiles{0};
    std::unordered_set<std::string> uniqueErrorMessages;
    std::mutex processMutex;
    std::mutex traverseErrorMutex;
    
    auto& pool = getStaticThreadPool();
    std::vector<std::future<void>> futures;
    
    for (const auto& path : finalPaths) {
        if (isValidDirectory(path)) {
            // Use the singleton pool
            futures.emplace_back(pool.enqueue([&, path]() {
                traverse(path, allIsoFiles, uniqueErrorMessages,
                         totalFiles, processMutex, traverseErrorMutex,
                         localMaxDepth, localPromptFlag);
            }));
        }
    }
    
    // Wait for all traversal tasks to complete
    for (auto& future : futures) {
        if (future.valid()) {
            future.wait();
        }
    }
    
    saveToDatabase(allIsoFiles, newISOFound);
    isImportRunning.store(false);
}

/**
 * @brief Loads ISO database from file into memory
 * 
 * @param outList Reference to vector that will receive the loaded ISO paths
 */
void loadFromDatabase(std::vector<std::string>& outList) {
    std::lock_guard<std::mutex> fileLock(dbFileMutex);
    
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
        outList.clear();
        return;
    }
    
    std::vector<char> buffer(fileStat.st_size);
    ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);
    
    flock(fd, LOCK_UN);
    close(fd);
    
    if (bytesRead <= 0 || bytesRead > fileStat.st_size) return;
    
    std::vector<std::string> loadedFiles;
    char* start = buffer.data();
    char* end = buffer.data() + bytesRead;
    
    while (start < end) {
        char* lineEnd = std::find(start, end, '\n');
        std::string line(start, lineEnd);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) loadedFiles.push_back(std::move(line));
        start = (lineEnd < end) ? lineEnd + 1 : end;
    }
    outList = std::move(loadedFiles);
}

/**
 * @brief Saves new ISO file paths to the database, merging with existing entries.
 *
 * Deduplicates incoming paths against the existing cache, appends new entries,
 * and trims to maxDatabaseSize if needed. Rewrites the entire database file
 * atomically under an exclusive lock.
 *
 * @param globalIsoFileList  Const reference to vector of ISO file paths to add.
 * @param newISOFound        Set to true if at least one new entry was added,
 *                           false if all paths already existed in the cache.
 * @return true  if new entries were written successfully.
 * @return false if no new entries were found, or if an I/O error occurred.
 */
bool saveToDatabase(const std::vector<std::string>& globalIsoFileList, std::atomic<bool>& newISOFound) {
    std::filesystem::path cachePath = databaseDirectory;
    cachePath /= databaseFilename;
    if (!std::filesystem::exists(databaseDirectory) && !std::filesystem::create_directories(databaseDirectory)) {
        return false;
    }
    if (!std::filesystem::is_directory(databaseDirectory)) {
        return false;
    }
    std::lock_guard<std::mutex> fileLock(dbFileMutex);
    int fd = open(cachePath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd == -1) return false;
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return false;
    }
    std::vector<std::string> existingCache;
    struct stat fileStat;
    if (fstat(fd, &fileStat) == 0 && fileStat.st_size > 0) {
        std::vector<char> buffer(fileStat.st_size);
        ssize_t bytesRead = ::read(fd, buffer.data(), fileStat.st_size);
        if (bytesRead > 0 && bytesRead <= fileStat.st_size) {
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
                start = (lineEnd < end) ? lineEnd + 1 : end;
            }
        }
    }
    std::unordered_set<std::string> existingSet(existingCache.begin(), existingCache.end());
    std::vector<std::string> newEntries;
    bool localNewISOFound = false;
    for (const auto& iso : globalIsoFileList) {
        if (existingSet.find(iso) == existingSet.end()) {
            newEntries.push_back(iso);
            localNewISOFound = true;
        }
    }
    if (newEntries.empty()) {
        newISOFound.store(false);
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }
    std::vector<std::string> combinedCache = existingCache;
    combinedCache.insert(combinedCache.end(), newEntries.begin(), newEntries.end());
    if (combinedCache.size() > maxDatabaseSize) {
        combinedCache.erase(combinedCache.begin(), combinedCache.begin() + (combinedCache.size() - maxDatabaseSize));
    }
    if (ftruncate(fd, 0) == -1 || lseek(fd, 0, SEEK_SET) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }
    std::string output;
    for (const auto& entry : combinedCache)
        output += entry + "\n";
    if (::write(fd, output.data(), output.size()) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }
    flock(fd, LOCK_UN);
    close(fd);
    newISOFound.store(localNewISOFound);
    isoListDirty.store(true);
    return true;
}

/**
 * @brief Displays database statistics including on-disk and RAM usage
 * 
 * Shows information about ISO database, history database, transformation cache,
 * and various file format caches.
 * 
 * @param databaseFilePath Path to the ISO database file
 * @param maxDatabaseSize Maximum allowed database size in bytes
 * @param transformationCache Map of transformation cache entries
 * @param globalIsoFileList Vector of ISO files in memory
 */
void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize, 
                                const std::unordered_map<std::string, std::string>& transformationCache, 
                                const std::vector<std::string>& globalIsoFileList) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();

    auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveDatabaseTheme();

    try {
        for (const auto& path : {databaseFilePath, historyFilePath, filterHistoryFilePath}) {
            if (!std::filesystem::exists(path)) {
                std::ofstream createFile(path);
            }
        }

        std::cout << "\n" << accent << "=== ISO Database ===" << reset << "\n";
        
        std::uintmax_t fileSizeInBytes = std::filesystem::file_size(databaseFilePath);
        double fileSizeInKB = fileSizeInBytes / 1024.0;
        double cachesizeInKb = maxDatabaseSize / 1024.0;
        double usagePercentage = (fileSizeInBytes * 100.0) / maxDatabaseSize;
        
        std::cout << "\n" << label << "Capacity: " << data << std::fixed << std::setprecision(0) 
                  << fileSizeInKB << "KB/" << cachesizeInKb << "KB (" << std::setprecision(1) << usagePercentage << "%)"
                  << "\n" << label << "Entries: " << data << countNonEmptyLines(databaseFilePath) 
                  << "\n" << label << "Location: " << data << "'" << databaseFilePath << "'" << reset << "\n";

        std::cout << "\n" << accent << "=== History Database ===" << reset << "\n"
                  << "\n" << label << "FolderPath Entries: " << data << countNonEmptyLines(historyFilePath) << "/" << MAX_HISTORY_LINES
                  << "\n" << label << "Location: " << data << "'" << historyFilePath << "'"
                  << "\n\n" << label << "FilterTerm Entries: " << data << countNonEmptyLines(filterHistoryFilePath) << "/" << MAX_HISTORY_PATTERN_LINES
                  << "\n" << label << "Location: " << data << "'" << filterHistoryFilePath << "'" << std::endl;
        
        std::cout << "\n" << accent << "=== Buffered Entries ===" << reset << "\n";
        
        std::cout << str << "\nSTR → RAM: " << data
                  << (transformationCache.size() + cachedParsesForUmount.size()) << "\n";
        
        std::cout << "\n" << label << "ISO → RAM: " << data << globalIsoFileList.size() << "\n";
        
        std::cout << "\n" << warning << "BIN/IMG → RAM: " << data << binImgFilesCache.size() << "\n"
                  << warning << "DAA/GBI → RAM: " << data << daaGbiFilesCache.size() << "\n"
                  << warning << "CHD → RAM: " << data << chdFilesCache.size() << "\n"
                  << warning << "MDF → RAM: " << data << mdfMdsFilesCache.size() << "\n"
                  << warning << "NRG → RAM: " << data << nrgFilesCache.size() << "\n";

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "\n" << error << "Error: Unable to access configuration file: "
                  << warning << "'" << configPath << "'"
                  << error << ".\n" << reset;
    }

    std::cout << color << "\n↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Updates the auto-update configuration setting
 * 
 * Modifies the auto_update setting in the configuration file and updates
 * the in-memory cache.
 * 
 * @param configPath Path to the configuration file
 * @param inputSearch Command string containing the new setting ("*auto:on" or "*auto:off")
 */
void updateAutoUpdateConfig(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    auto db = resolveDatabaseTheme();

    fs::path p(configPath);
    if (!fs::exists(p.parent_path()) && !p.parent_path().empty()) 
        fs::create_directories(p.parent_path());

    syncCache(configPath);

    bool isEnabling = (inputSearch == "*auto:on");
    g_configCache["auto_update"] = isEnabling ? "on" : "off";

    if (writeConfig(configPath, g_configCache)) {
        std::cout << "\n" << db.label << "Automatic background updates have been "
                  << (isEnabling ? db.data : db.error) << (isEnabling ? "enabled" : "disabled")
                  << db.label << ".\033[J" << db.reset << "\n";
    } else {
        std::cerr << "\n" << db.error << "Error: Unable to access configuration file: " 
                  << db.warning << "'" << configPath << "'" 
                  << db.error << ".\033[J" << db.reset << "\n";
    }

    std::cout << color << "\n↵ to continue..." << db.reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Handles database management commands and statistics display
 * 
 * Processes various database-related commands including stats display,
 * cache clearing, configuration updates, and theme changes.
 * 
 * @param inputSearch Command string to process
 * @param promptFlag Flag controlling prompt behavior
 * @param maxDepth Maximum directory traversal depth
 * @param filterHistory Flag for filter history management
 * @param newISOFound Atomic flag indicating if new ISOs were found
 */
void databaseSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& filterHistory, std::atomic<bool>& newISOFound) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    
    auto db = resolveDatabaseTheme();
    
    if (inputSearch == "?stats") {
        displayDatabaseStatistics(databaseFilePath, maxDatabaseSize, transformationCache, globalIsoFileList);
    } else if (inputSearch == "?config") {
        displayConfigurationOptions(configPath);
    } else if (inputSearch == "!clr") {
        std::ofstream ofs(databaseFilePath, std::ofstream::out | std::ofstream::trunc);
        if (!ofs) {
            std::cerr << "\n" << db.error << "Error clearing ISO database: " 
                      << db.warning << "'" << databaseFilePath << "'" 
                      << db.error << ". File missing or inaccessible.\033[J" << std::endl;
            
            std::cout << "\n" << color << "↵ to continue..." << db.reset;
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            ofs.close();
            for (auto it = transformationCache.begin(); it != transformationCache.end();) {
                const std::string& key = it->first;
                if (key.size() >= 4) {
                    std::string ext = key.substr(key.size() - 4);
                    toLowerInPlace(ext);
                    if (ext == ".iso") {
                        it = transformationCache.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            
            std::cout << "\n" << db.data << "ISO database cleared successfully." << "\033[J" << std::endl;
            std::cout << color << "\n↵ to continue..." << db.reset; 
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::vector<std::string>().swap(globalIsoFileList);
        }
    } else if (inputSearch == "!clr_paths" || inputSearch == "!clr_filter") {
        clearHistory(inputSearch);
    } else if (inputSearch == "*auto:on" || inputSearch == "*auto:off") {
        updateAutoUpdateConfig(configPath, inputSearch);
    } else if (inputSearch == "*flno:on" || inputSearch == "*flno:off") {
        needSortingAfterflno = true;
        updateFilenamesOnly(configPath, inputSearch);
    } else if (inputSearch.substr(0, 12) == "*pagination:") {
        updatePagination(inputSearch, configPath);
    } else if (inputSearch.substr(0, 6) == "*skin:" || inputSearch.substr(0, 7) == "*theme:") {
        updateUIAppearance(configPath, inputSearch);
    } else if (isValidInput(inputSearch)) {
        setDisplayMode(inputSearch);
    }

    refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
}
