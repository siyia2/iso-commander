// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


const std::string MOUNTED_ISO_PATH = "/mnt";

bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered) {
     isoDirs.clear();
        for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH)) {
            if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                isoDirs.push_back(entry.path().string());
            }
        }
        sortFilesCaseInsensitive(isoDirs);

    // Check if ISOs exist
    if (isoDirs.empty()) {
		clearScrollBuffer();
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_*' pattern found.\033[0m\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return false;
    }

    // Sort ISOs case-insensitively
    sortFilesCaseInsensitive(isoDirs);

    // Display ISOs
    clearScrollBuffer();
        if (filteredFiles.size() == isoDirs.size()) {
				isFiltered = false;
		}
        printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS");

    return true;
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    // Early root privilege check
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDir
                         << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            unmountedErrors.emplace(errorMessage.str());
        }
        return;
    }

    // Sequential processing of unmounts
    std::vector<std::pair<std::string, int>> unmountResults;
    for (const auto& isoDir : isoDirs) {
        int result = umount2(isoDir.c_str(), MNT_DETACH);
        unmountResults.emplace_back(isoDir, result);
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
void prepareUnmount(const std::string& input, std::vector<std::string>& selectedIsoDirs, std::vector<std::string>& currentFiles, std::set<std::string>& operationFiles, std::set<std::string>& operationFails, std::set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
    std::set<int> selectedIndices;
    
    if (input != "00" && selectedIsoDirs.empty()) {
        tokenizeInput(input, currentFiles, uniqueErrorMessages, selectedIndices);
        for (int index : selectedIndices) {
            selectedIsoDirs.push_back(currentFiles[index - 1]);
        }
    }

    // Check if input is empty
    if (selectedIsoDirs.empty()) {
        umountMvRmBreak = false;
        clearScrollBuffer();
        std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Clear buffer and print start message
    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing \033[1;93mumount\033[0;1m operations...\n";

    // Chunking logic
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedIsoDirs.size()), maxThreads);
    std::vector<std::vector<std::string>> isoChunks;
    const size_t maxMountPointsPerChunk = 100;
    
    // Distribute ISOs evenly among threads, but not exceeding maxISOsPerChunk
    size_t totalMountPoints = selectedIsoDirs.size();
    size_t mountPointsPerThread = (totalMountPoints + numThreads - 1) / numThreads;
    size_t chunkSize = std::min(maxMountPointsPerChunk, mountPointsPerThread);

    for (size_t i = 0; i < totalMountPoints; i += chunkSize) {
        auto chunkEnd = std::min(selectedIsoDirs.begin() + i + chunkSize, selectedIsoDirs.end());
        isoChunks.emplace_back(selectedIsoDirs.begin() + i, chunkEnd);
    }

    // Initialization
    std::mutex lowLevelMutex;
    ThreadPool pool(numThreads);
    std::vector<std::future<void>> unmountFutures;
    std::atomic<size_t> completedIsos(0);
    size_t totalIsos = selectedIsoDirs.size();
    std::atomic<bool> isComplete(false);

    // Start progress display thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,       // Pass as raw pointer
        static_cast<size_t>(0), // Pass as size_t
        &completedIsos,       // Pass as raw pointer
        totalIsos,            // Pass as size_t
        &isComplete, 			// Pass as raw pointer
        &verbose               // Pass as raw pointer
    );

    // Submit tasks to the thread pool
    for (const auto& isoChunk : isoChunks) {
		unmountFutures.emplace_back(pool.enqueue([&]() {
			// Process the entire chunk at once
			unmountISO(isoChunk, operationFiles, operationFails);
			completedIsos.fetch_add(isoChunk.size(), std::memory_order_relaxed); // Increment for all ISOs in the chunk
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

