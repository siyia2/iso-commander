// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"


const std::string MOUNTED_ISO_PATH = "/mnt";


// Function to load and display mount-points
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered) {
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
    disable_ctrl_d();

    // Static cache: previous hash and sorted directories.
    static size_t previousHash = 0;
    static std::vector<std::string> lastSortedDirs;
    std::vector<std::string> newIsoDirs;

	// Collect directories
	for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH)) {
		if (entry.is_directory()) {
			auto filename = entry.path().filename().string();
			if (filename.find("iso_") == 0) { // or filename.starts_with("iso_") in C++20
				newIsoDirs.push_back(entry.path().string());
			}
		}
	}

    // Compute an order-independent hash:
    size_t currentHash = 0;
    for (const auto& path : newIsoDirs) {
        currentHash += std::hash<std::string>{}(path);
    }
    currentHash += newIsoDirs.size();

    // Only sort if the set of directories has changed
    if (currentHash != previousHash) {
        sortFilesCaseInsensitive(newIsoDirs);
        // Cache the sorted vector and update the hash
        lastSortedDirs = newIsoDirs;
        previousHash = currentHash;
    } else {
        // Reuse the cached sorted vector if nothing has changed
        newIsoDirs = lastSortedDirs;
    }

    isoDirs = std::move(newIsoDirs);

    if (isoDirs.empty()) {
        clearScrollBuffer();
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_{name}' pattern found.\033[0m\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0m\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::vector<std::string>().swap(isoDirs); // De-allocate memory for static vector
        return false;
    }

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
void unmountISO(const std::vector<std::string>& isoDirs,
                std::unordered_set<std::string>& unmountedFiles,
                std::unordered_set<std::string>& unmountedErrors,
                std::atomic<size_t>* completedTasks,
                std::atomic<size_t>* failedTasks) {

    // Define batch size for insertions
    const size_t BATCH_SIZE = 1000;
    size_t totalProcessedEntries = 0;

    // Pre-define format strings to avoid repeated string constructions
    const std::string rootErrorPrefix = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string rootErrorSuffix  = "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";

    const std::string successPrefix    = "\033[0;1mUnmounted: \033[1;92m'";
    const std::string successSuffix    = "\033[1;92m'\033[0m.";

    const std::string errorPrefix      = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string errorSuffix      = "'\033[1;91m.\033[0;1m {notAnISO}";

    // Cancellation strings (make sure to use these names consistently)
    const std::string cancelPrefix     = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string cancelSuffix     = "'\033[1;91m.\033[0;1m {CXL}";

    // Pre-allocate containers with batch capacity
    std::vector<std::string> errorMessages;
    std::vector<std::string> successMessages;
    std::vector<std::string> removalMessages;

    errorMessages.reserve(BATCH_SIZE);
    successMessages.reserve(BATCH_SIZE);
    removalMessages.reserve(BATCH_SIZE);

    // Create a reusable string buffer
    std::string outputBuffer;
    outputBuffer.reserve(512);  // Reserve space for a typical message

    // Function to flush temporary buffers to sets
    auto flushTemporaryBuffers = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);

        if (!successMessages.empty()) {
            unmountedFiles.insert(successMessages.begin(), successMessages.end());
            successMessages.clear();
        }

        if (!removalMessages.empty()) {
            unmountedFiles.insert(removalMessages.begin(), removalMessages.end());
            removalMessages.clear();
        }

        if (!errorMessages.empty()) {
            unmountedErrors.insert(errorMessages.begin(), errorMessages.end());
            errorMessages.clear();
        }
        totalProcessedEntries = 0;
    };

    // Root check with cancellation awareness
    bool hasRoot = (geteuid() == 0);

    if (!hasRoot) {
        for (const auto& isoDir : isoDirs) {
            std::string modifiedDir = modifyDirectoryPath(isoDir);
            outputBuffer.clear();
            if (!g_operationCancelled.load()) {
                outputBuffer.append(rootErrorPrefix)
                            .append(modifiedDir)
                            .append(rootErrorSuffix);
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
            } else {
                // Append cancelled message when operation is cancelled
                outputBuffer.append(cancelPrefix)
                            .append(modifiedDir)
                            .append(cancelSuffix);
                // Optionally, you could update a cancellation counter here
            }
            errorMessages.push_back(outputBuffer);
            totalProcessedEntries++;

            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
        }
        if (totalProcessedEntries > 0) {
            flushTemporaryBuffers();
        }
        // If no root, skip unmount operations entirely
        return;
    }

    // For unmount operations, we'll collect results first, then process them in batches
    std::vector<std::pair<std::string, int>> unmountResults;
    unmountResults.reserve(isoDirs.size());

    // Perform unmount operations and record results
    for (const auto& isoDir : isoDirs) {
        if (g_operationCancelled.load()) {
            break;
        }
        int result = umount2(isoDir.c_str(), MNT_DETACH);
        unmountResults.emplace_back(isoDir, result);
    }

    // Process results only if not cancelled
    size_t processed = 0;
    for (const auto& [dir, result] : unmountResults) {
        if (g_operationCancelled.load()) break;
        bool isEmpty = isDirectoryEmpty(dir);
        std::string modifiedDir = modifyDirectoryPath(dir);
        outputBuffer.clear();

        if (result == 0 || isEmpty) {
            // Successful unmount
            if (isEmpty && rmdir(dir.c_str()) == 0) {
                outputBuffer.append(successPrefix)
                            .append(modifiedDir)
                            .append(successSuffix);
                successMessages.push_back(outputBuffer);
                completedTasks->fetch_add(1, std::memory_order_acq_rel);
            }
        } else {
            // Failed unmount
            outputBuffer.append(errorPrefix)
                        .append(modifiedDir)
                        .append(errorSuffix);
            errorMessages.push_back(outputBuffer);
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        totalProcessedEntries++;
        processed++;

        // Check if we need to flush
        if (totalProcessedEntries >= BATCH_SIZE) {
            flushTemporaryBuffers();
        }
    }

    // If cancellation occurred, process the remaining isoDirs as cancelled
    if (g_operationCancelled.load()) {
        for (size_t i = processed; i < isoDirs.size(); ++i) {
            std::string modifiedDir = modifyDirectoryPath(isoDirs[i]);
            outputBuffer.clear();
            outputBuffer.append(cancelPrefix)
                        .append(modifiedDir)
                        .append(cancelSuffix);
            errorMessages.push_back(outputBuffer);
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            totalProcessedEntries++;
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
        }
    }

    // Final flush for any remaining entries
    if (totalProcessedEntries > 0) {
        flushTemporaryBuffers();
    }
}


// Main function to send ISOs for unmount
void prepareUnmount(const std::string& input, std::vector<std::string>& currentFiles, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
    // Setup signal handler
    setupSignalHandlerCancellations();
    
    g_operationCancelled.store(false);
    
    std::unordered_set<int> indicesToProcess;

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
    std::vector<std::future<void>> unmountFutures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Start progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
        &failedTasks,
        selectedMountpoints.size(),
        &isProcessingComplete,
        &verbose
    );

    // Enqueue chunk tasks
    for (const auto& chunk : chunks) {
        unmountFutures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            unmountISO(chunk, operationFiles, operationFails, &completedTasks, &failedTasks);
        }));
    }

    // Wait for completion or cancellation
    for (auto& future : unmountFutures) {
        future.wait();
        if (g_operationCancelled.load()) break;
    }

    // Cleanup
    isProcessingComplete.store(true);
    progressThread.join();
}

