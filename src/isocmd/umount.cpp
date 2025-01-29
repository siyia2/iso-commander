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

std::string modifyDirectoryPath(const std::string& dir) {
    if (toggleFullList) {
        return dir; // Return the original directory if toggleFullList is true
    }

    // Find the position of the first '_'
    size_t firstUnderscorePos = dir.find_first_of('_');
    // Find the position of the last '~'
    size_t lastTildePos = dir.find_last_of('~');

    // If either '_' or '~' is not found, return the original directory
    if (firstUnderscorePos == std::string::npos || lastTildePos == std::string::npos) {
        return dir;
    }

    // Extract the substring between '_' and '~'
    // Start at the character after '_' and end at the character before '~'
    std::string newDir = dir.substr(firstUnderscorePos + 1, lastTildePos - (firstUnderscorePos + 1));

    return newDir;
}

// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    
    std::atomic<bool> g_CancelledMessageAdded{false};
    
    // Early exit if cancelled before starting
    if (g_operationCancelled) return;

    // Root check with cancellation awareness
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            if (g_operationCancelled) break;
            std::string modifiedDir = modifyDirectoryPath(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << modifiedDir
                        << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            unmountedErrors.emplace(errorMessage.str());
        }
        return;
    }

    std::vector<std::pair<std::string, int>> unmountResults;
    for (const auto& isoDir : isoDirs) {
        if (g_operationCancelled) {
            if (!g_CancelledMessageAdded.exchange(true)) {
				unmountedErrors.clear();
                unmountedErrors.emplace("\033[1;33mUnmount Operation interrupted by user - partial cleanup performed.\033[0m");
            }
            break;
        }
        
        int result = umount2(isoDir.c_str(), MNT_DETACH);
        unmountResults.emplace_back(isoDir, result);
    }

    // Process results only if not cancelled
    if (!g_operationCancelled) {
        std::vector<std::string> successfulUnmounts;
        std::vector<std::string> failedUnmounts;
        
        for (const auto& [dir, result] : unmountResults) {
            bool isEmpty = isDirectoryEmpty(dir);
            (result == 0 || isEmpty) ? successfulUnmounts.push_back(dir) : failedUnmounts.push_back(dir);
        }

        // Handle successful unmounts
        for (const auto& dir : successfulUnmounts) {
            if (isDirectoryEmpty(dir) && rmdir(dir.c_str()) == 0) {
				std::string modifiedDir = modifyDirectoryPath(dir);
                unmountedFiles.emplace("\033[0;1mUnmounted: \033[1;92m'" + modifiedDir + "\033[1;92m'\033[0m.");
            }
        }

        // Additional cleanup pass
        for (const auto& dir : isoDirs) {
            if (dir.find("/mnt/iso_") == 0 && isDirectoryEmpty(dir) && rmdir(dir.c_str()) == 0) {
				std::string modifiedDir = modifyDirectoryPath(dir);
                unmountedFiles.emplace("\033[0;1mRemoved empty ISO directory: \033[1;92m'" + modifiedDir + "\033[1;92m'\033[0m.");
            }
        }

        // Handle failures
        for (const auto& dir : failedUnmounts) {
			std::string modifiedDir = modifyDirectoryPath(dir);
            unmountedErrors.emplace("\033[1;91mFailed to unmount: \033[1;93m'" + modifiedDir + "'\033[1;91m.\033[0;1m {notAnISO}");
        }
    }
}


// Main function to send ISOs for unmount
void prepareUnmount(const std::string& input, std::vector<std::string>& selectedIsoDirs, std::vector<std::string>& currentFiles, std::set<std::string>& operationFiles, std::set<std::string>& operationFails, std::set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
    std::set<int> selectedIndices;
    
    // Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
        
    // Reset cancellation flag
    g_operationCancelled = false;
    
    if (input != "00" && selectedIsoDirs.empty()) {
        tokenizeInput(input, currentFiles, uniqueErrorMessages, selectedIndices);
        for (int index : selectedIndices) {
            if (g_operationCancelled) break;
            selectedIsoDirs.push_back(currentFiles[index - 1]);
        }
    }

    if (selectedIsoDirs.empty()) {
        umountMvRmBreak = false;
        clearScrollBuffer();
        std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
        std::cout << "\n\033[1;32m↵ to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing \033[1;93mumount\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";

    // Thread pool setup
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedIsoDirs.size()), maxThreads);
    std::vector<std::vector<std::string>> isoChunks;
    const size_t chunkSize = std::min(size_t(100), selectedIsoDirs.size()/numThreads + 1);

    for (size_t i = 0; i < selectedIsoDirs.size(); i += chunkSize) {
        auto end = std::min(selectedIsoDirs.begin() + i + chunkSize, selectedIsoDirs.end());
        isoChunks.emplace_back(selectedIsoDirs.begin() + i, end);
    }

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> unmountFutures;
    std::atomic<size_t> completedIsos(0);
    std::atomic<bool> isComplete(false);

    // Progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedIsos,
        selectedIsoDirs.size(),
        &isComplete,
        &verbose
    );

    // Submit tasks with cancellation checks
    for (const auto& isoChunk : isoChunks) {
        unmountFutures.emplace_back(pool.enqueue([&, isoChunk]() {
            if (g_operationCancelled) return;
            
            unmountISO(isoChunk, operationFiles, operationFails);
            completedIsos.fetch_add(isoChunk.size(), std::memory_order_relaxed);
            
            if (g_operationCancelled) {
                isComplete.store(true);
            }
        }));
    }

    // Wait for completion or cancellation
    for (auto& future : unmountFutures) {
        future.wait();
        if (g_operationCancelled) break;
    }

    isComplete.store(true);
    progressThread.join();
}

