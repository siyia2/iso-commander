// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"


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
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_{name}' pattern found.\033[0m\033[0;1m\n";
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
        printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "");

    return true;
}


// Function toggle between long and short vebose logging in umount
std::string modifyDirectoryPath(const std::string& dir) {
    if (displayConfig::toggleFullListUmount) {
        return dir;
    }

    // We know:
    // - "/mnt/iso_" is 9 characters
    // - The total length including '~' at the end is 6 characters from the start
    
    // First check if string is long enough
    if (dir.length() < 9) {  // Must be at least as long as "/mnt/iso_"
        return dir;
    }
    
    // Verify the '_' is where we expect it
    if (dir[8] != '_') {
        return dir;
    }
    
    // Find the last '~'
    size_t lastTildePos = dir.find_last_of('~');
    if (lastTildePos == std::string::npos) {
        return dir;
    }
    
    // Extract everything between the known '_' position and the '~'
    return dir.substr(9, lastTildePos - 9);
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    
    // Early exit if cancelled before starting
    if (g_operationCancelled.load()) {
		return;
	}

    // Root check with cancellation awareness
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            if (g_operationCancelled.load()) {
				break;
			}
            std::string modifiedDir = modifyDirectoryPath(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << modifiedDir
                        << "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
            {
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                unmountedErrors.emplace(errorMessage.str());
            }
        }
        return;
    }

    std::vector<std::pair<std::string, int>> unmountResults;
    for (const auto& isoDir : isoDirs) {
        if (g_operationCancelled.load()) {
            break;
        }
        
        int result = umount2(isoDir.c_str(), MNT_DETACH);
        unmountResults.emplace_back(isoDir, result);
    }

    // Process results only if not cancelled
    if (!g_operationCancelled.load()) {
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
                {
                    std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                    unmountedFiles.emplace("\033[0;1mUnmounted: \033[1;92m'" + modifiedDir + "\033[1;92m'\033[0m.");
                }
            }
        }

        // Additional cleanup pass
        for (const auto& dir : isoDirs) {
            if (dir.find("/mnt/iso_") == 0 && isDirectoryEmpty(dir) && rmdir(dir.c_str()) == 0) {
                std::string modifiedDir = modifyDirectoryPath(dir);
                {
                    std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                    unmountedFiles.emplace("\033[0;1mRemoved empty ISO directory: \033[1;92m'" + modifiedDir + "\033[1;92m'\033[0m.");
                }
            }
        }

        // Handle failures
        for (const auto& dir : failedUnmounts) {
            std::string modifiedDir = modifyDirectoryPath(dir);
            {
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                unmountedErrors.emplace("\033[1;91mFailed to unmount: \033[1;93m'" + modifiedDir + "'\033[1;91m.\033[0;1m {notAnISO}");
            }
        }
    }
}


// Main function to send ISOs for unmount
void prepareUnmount(const std::string& input, std::vector<std::string>& currentFiles, std::set<std::string>& operationFiles, std::set<std::string>& operationFails, std::set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
    std::set<int> indicesToProcess;
    
    // Setup signal handler and reset cancellation flag
    setupSignalHandlerCancellations();

    // Handle input ("00" = all files, else parse input)
    if (input == "00") {
        for (size_t i = 0; i < currentFiles.size(); ++i)
            indicesToProcess.insert(i + 1);
    } else {
        tokenizeInput(input, currentFiles, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            umountMvRmBreak = false;
            return;
        }
    }

    // Create selected files vector from indices
    std::vector<std::string> selectedMountpoints;
    selectedMountpoints.reserve(indicesToProcess.size());
    for (int index : indicesToProcess)
        selectedMountpoints.push_back(currentFiles[index - 1]);

    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing \033[1;93mumount\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";

    // Thread pool setup
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedMountpoints.size()), maxThreads);
    const size_t chunkSize = std::min(size_t(100), selectedMountpoints.size()/numThreads + 1);
    std::vector<std::vector<std::string>> chunks;

    // Split work into chunks
    for (size_t i = 0; i < selectedMountpoints.size(); i += chunkSize) {
        auto end = std::min(selectedMountpoints.begin() + i + chunkSize, selectedMountpoints.end());
        chunks.emplace_back(selectedMountpoints.begin() + i, end);
    }

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> umountFutures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Start progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
        selectedMountpoints.size(),
        &isProcessingComplete,
        &verbose
    );

	// Mutex for thread-safe completion counting
    std::mutex completionMutex;
	
    for (const auto& chunk : chunks) {
        umountFutures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) {
                return;
            }

            std::vector<std::string> successfulUmounts;
            for (const auto& file : chunk) {
                if (g_operationCancelled.load()) {
                    break;
                }

                bool umountSuccess = false;
                {
                    std::lock_guard<std::mutex> lock(completionMutex);
                    // Process single file mount
                    std::set<std::string> tempUmounted, tempFailed;
                    std::vector<std::string> singleFileChunk = {file};
                    unmountISO(singleFileChunk, tempUmounted, tempFailed);

                    // Update global sets and track success
                    operationFiles.insert(tempUmounted.begin(), tempUmounted.end());
                    operationFails.insert(tempFailed.begin(), tempFailed.end());
                    umountSuccess = !tempUmounted.empty();
                }

                // Only increment completed tasks if not cancelled
                if (!g_operationCancelled.load()) {
                    if (umountSuccess) {
                        successfulUmounts.push_back(file);
                    }
                    completedTasks.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }));
    }

    // Wait for completion or cancellation
    for (auto& future : umountFutures) {
        future.wait();
        if (g_operationCancelled.load()) {
			operationFails.clear();
			operationFails.emplace("\033[1;33mUnmount operation interrupted by user - partial cleanup performed.\033[0m");
			break;
		}
    }

    // Cleanup
    isProcessingComplete.store(true);
    progressThread.join();
}

