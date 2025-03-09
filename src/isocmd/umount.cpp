// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"
#include "../umount.h"


// Mounpoint location
const std::string MOUNTED_ISO_PATH = "/mnt";

// Memory map for umount string transformations
std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>> transformationCacheUmount;


// Function to load and display mount-points
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak) {
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
			if (filename.find("iso_") == 0) { // filename.starts_with("iso_")
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
        std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>>().swap(transformationCacheUmount); //De-allocate memory map when no mounpoints exist
        return false;
    }

    clearScrollBuffer();

    if (filteredFiles.size() == isoDirs.size() || umountMvRmBreak) {
		filteredFiles = isoDirs;
        isFiltered = false;
    }
    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "");

    return true;
}


// Function toggle between long and short verbose logging in umount
std::tuple<std::string, std::string, std::string> extractDirectoryAndFilenameFromIso(const std::string& dir) {
    // Check if we have a cached transformation
    auto cacheIt = transformationCacheUmount.find(dir);
    if (cacheIt != transformationCacheUmount.end()) {
        return cacheIt->second;
    }
    
    // Find the first underscore in the input.
    size_t underscorePos = dir.find('_');
    if (underscorePos == std::string::npos) {
        transformationCacheUmount[dir] = std::make_tuple(dir, "", "");
        return std::make_tuple(dir, "", "");
    }
    
    // Find the last tilde.
    size_t lastTildePos = dir.find_last_of('~');
    if (lastTildePos == std::string::npos || lastTildePos <= underscorePos) {
        // If no tilde exists or it's not after the underscore,
        // return directory (before underscore), filename (everything after underscore), and an empty hash.
        auto result = std::make_tuple(dir.substr(0, underscorePos), dir.substr(underscorePos + 1), "");
        transformationCacheUmount[dir] = result;
        return result;
    }
    
    // Extract the filename part only:
    // Filename: substring from just after the underscore up to (but not including) the last tilde.
    std::string filenamePart = dir.substr(underscorePos + 1, lastTildePos - underscorePos - 1);
    
    // Extract the hash part:
    // Hash: substring from the last tilde to the end of the string.
    std::string hashPart = dir.substr(lastTildePos);
    
    auto result = std::make_tuple("", filenamePart, hashPart);  // Empty directory part
    transformationCacheUmount[dir] = result;
    return result;
}


// Then update the unmountISO function to handle the pair
void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks) {

    // Create the message formatter
    VerboseMessageFormatter messageFormatter;
    
    // Define batch size for insertions
    const size_t BATCH_SIZE = 1000;

    // Pre-allocate containers with batch capacity
    std::vector<std::string> errorMessages;
    std::vector<std::string> successMessages;

    errorMessages.reserve(BATCH_SIZE);
    successMessages.reserve(BATCH_SIZE);

    // Function to flush temporary buffers to sets
    auto flushTemporaryBuffers = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);

        if (!successMessages.empty()) {
            unmountedFiles.insert(successMessages.begin(), successMessages.end());
            successMessages.clear();
        }

        if (!errorMessages.empty()) {
            unmountedErrors.insert(errorMessages.begin(), errorMessages.end());
            errorMessages.clear();
        }
    };

    // Helper to check if any buffer needs flushing
    auto checkAndFlush = [&]() {
        if (errorMessages.size() >= BATCH_SIZE || successMessages.size() >= BATCH_SIZE) {
            flushTemporaryBuffers();
        }
    };

    // Root check with cancellation awareness
    bool hasRoot = (geteuid() == 0);

    if (!hasRoot) {
        for (const auto& isoDir : isoDirs) {
            auto dirParts = extractDirectoryAndFilenameFromIso(isoDir);
            std::string formattedDir;
            // Add /mnt/_
            if (displayConfig::toggleFullListUmount) formattedDir = std::get<0>(dirParts);
            // Add filename
            formattedDir += std::get<1>(dirParts);
            // Add ~ hash part
			if (displayConfig::toggleFullListUmount) formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
            
            
            if (!g_operationCancelled.load()) {
                errorMessages.push_back(messageFormatter.format("root_error", formattedDir));
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
            } else {
                errorMessages.push_back(messageFormatter.format("cancel", formattedDir));
            }

            // Check if we need to flush based on buffer sizes
            checkAndFlush();
        }
        // Final flush for remaining entries
        flushTemporaryBuffers();
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
    for (const auto& [dir, result] : unmountResults) {
        if (g_operationCancelled.load()) break;
        bool isEmpty = isDirectoryEmpty(dir);
        auto dirParts = extractDirectoryAndFilenameFromIso(dir);
        std::string formattedDir;
        // Add /mnt/_
        if (displayConfig::toggleFullListUmount) formattedDir = std::get<0>(dirParts);
        // Add filename
        formattedDir += std::get<1>(dirParts);
        // Add ~ hash part
		if (displayConfig::toggleFullListUmount) formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";

        if (result == 0 || isEmpty) {
            // Successful unmount
            if (isEmpty && rmdir(dir.c_str()) == 0) {
                successMessages.push_back(messageFormatter.format("success", formattedDir));
                completedTasks->fetch_add(1, std::memory_order_acq_rel);
            }
        } else {
            // Failed unmount
            errorMessages.push_back(messageFormatter.format("error", formattedDir));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }

        // Check if we need to flush based on buffer sizes
        checkAndFlush();
    }

    // If cancellation occurred, process the remaining isoDirs as cancelled
    if (g_operationCancelled.load()) {
        for (size_t i = unmountResults.size(); i < isoDirs.size(); ++i) {
            auto dirParts = extractDirectoryAndFilenameFromIso(isoDirs[i]);
            std::string formattedDir;
            // Add /mnt/_
            if (displayConfig::toggleFullListUmount) formattedDir = std::get<0>(dirParts);
            // Add filename
            formattedDir += std::get<1>(dirParts);
            // Add ~ hash part
			if (displayConfig::toggleFullListUmount) formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
            
            errorMessages.push_back(messageFormatter.format("cancel", formattedDir));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to flush based on buffer sizes
            checkAndFlush();
        }
    }

    // Final flush for any remaining entries
    flushTemporaryBuffers();
}


// Main function to send ISOs for unmount
void prepareUnmount(const std::string& input, const std::vector<std::string>& currentFiles, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
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

