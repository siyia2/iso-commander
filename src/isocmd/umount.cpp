// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF


// Function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;  // Unable to open directory
    }

    errno = 0;
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (++count > 2) {
            closedir(dir);
            return false;  // Directory not empty
        }
    }

    closedir(dir);
    return errno == 0 && count <= 2;  // Empty if only "." and ".." entries and no errors
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::mutex& Mutex4Low) {
    // Early root privilege check
    if (geteuid() != 0) {
        std::lock_guard<std::mutex> lowLock(Mutex4Low);
        for (const auto& isoDir : isoDirs) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDir
                         << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            unmountedErrors.emplace(errorMessage.str());
        }
        return;
    }

    // Parallel execution for unmounting
    std::vector<std::pair<std::string, int>> unmountResults;
    {
        std::vector<std::thread> threads;
        std::mutex resultMutex;

        for (const auto& isoDir : isoDirs) {
            threads.emplace_back([&](const std::string& dir) {
                int result = umount2(dir.c_str(), MNT_DETACH);
                std::lock_guard<std::mutex> lock(resultMutex);
                unmountResults.emplace_back(dir, result);
            }, isoDir);
        }

        for (auto& thread : threads) {
            thread.join();
        }
    }

    // Process unmount results
    std::vector<std::string> successfulUnmounts;
    std::vector<std::string> failedUnmounts;

    for (const auto& [dir, result] : unmountResults) {
        bool isEmpty = isDirectoryEmpty(dir);
        
        // If unmount fails but directory is empty, treat it as a potential removal
        if (result != 0 && !isEmpty) {
            failedUnmounts.push_back(dir);
        } else {
            successfulUnmounts.push_back(dir);
        }
    }

    // Handle successful and unsuccessful unmounts, with special focus on /mnt/iso_ directories
    {
        std::lock_guard<std::mutex> lowLock(Mutex4Low);
        
        // Try to remove all successful unmount directories
        for (const auto& dir : successfulUnmounts) {
            if (isDirectoryEmpty(dir)) {
                if (rmdir(dir.c_str()) == 0) {
                    std::string removedDirInfo = "\033[0;1mUnmounted: \033[1;92m'" + dir + "\033[1;92m'\033[0m.";
                    unmountedFiles.emplace(removedDirInfo);
                }
            }
        }

        // Additional pass to remove empty /mnt/iso_* directories
        for (const auto& dir : isoDirs) {
            // Check if directory starts with /mnt/iso_ and is empty
            if (dir.find("/mnt/iso_") == 0 && isDirectoryEmpty(dir)) {
                if (rmdir(dir.c_str()) == 0) {
                    std::string removedDirInfo = "\033[0;1mRemoved empty ISO directory: \033[1;92m'" + dir + "\033[1;92m'\033[0m.";
                    unmountedFiles.emplace(removedDirInfo);
                }
            }
        }

        // Handle failed unmounts (now only for non-empty directories)
        for (const auto& dir : failedUnmounts) {
            std::stringstream errorMessage;
            
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << dir 
                         << "\033[1;93m'\033[1;91m.\033[0;1m {notAnISO}";
            unmountedErrors.emplace(errorMessage.str());
        }
    }
}


// Main function to send ISOs for unmount
void prepareUnmount(std::vector<std::string>& selectedIsoDirs, std::set<std::string>& operationFiles, std::set<std::string>& operationFails, bool& verbose) {

    // Check if input is empty
    if (selectedIsoDirs.empty()) {
        clearScrollBuffer();
        std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Clear buffer and print start message
    clearScrollBuffer();
    std::cout << "\n\033[0;1m";

    // Initialization
    std::mutex umountMutex, lowLevelMutex;
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedIsoDirs.size()), maxThreads);
    ThreadPool pool(numThreads);
    std::vector<std::future<void>> unmountFutures;

    std::atomic<size_t> completedIsos(0);
    size_t totalIsos = selectedIsoDirs.size();
    std::atomic<bool> isComplete(false);

    // Start progress display thread
    std::thread progressThread(displayProgressBar, 
        std::ref(completedIsos), 
        std::cref(totalIsos), 
        std::ref(isComplete), 
        std::ref(verbose)
    );

    // Submit tasks to the thread pool
    for (const auto& iso : selectedIsoDirs) {
        unmountFutures.emplace_back(pool.enqueue([&]() {
            std::lock_guard<std::mutex> lock(umountMutex);
            unmountISO({iso}, operationFiles, operationFails, lowLevelMutex);
            completedIsos.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    // Wait for all tasks to complete
    for (auto& future : unmountFutures) {
        future.wait();
    }

    // Signal completion and join progress thread
    isComplete.store(true, std::memory_order_release);
    progressThread.join();
}

