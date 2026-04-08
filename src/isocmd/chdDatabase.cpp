
#include "../headers.h"
#include "../threadpool.h"

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
