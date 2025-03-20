// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"
#include "../umount.h"


// Function to check if directory is empty for umount
bool isDirectoryEmpty(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    
    return std::filesystem::directory_iterator(path) == 
           std::filesystem::directory_iterator();
}


// Function to perform unmount using umount2
void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks) {

    VerboseMessageFormatter messageFormatter;
    const size_t BATCH_SIZE = 1000;
    std::vector<std::string> errorMessages, successMessages;
    errorMessages.reserve(BATCH_SIZE);
    successMessages.reserve(BATCH_SIZE);

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

    auto checkAndFlush = [&]() {
        if (errorMessages.size() >= BATCH_SIZE || successMessages.size() >= BATCH_SIZE) {
            flushTemporaryBuffers();
        }
    };

    bool hasRoot = (geteuid() == 0);

    if (!hasRoot) {
        for (const auto& isoDir : isoDirs) {
            auto dirParts = parseMountPointComponents(isoDir);
            std::string formattedDir;
            if (displayConfig::toggleFullListUmount) 
                formattedDir = std::get<0>(dirParts);
            formattedDir += std::get<1>(dirParts);
            if (displayConfig::toggleFullListUmount) 
                formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
            
            if (!g_operationCancelled.load()) {
                errorMessages.push_back(messageFormatter.format("root_error", formattedDir));
                failedTasks->fetch_add(1);
            } else {
                errorMessages.push_back(messageFormatter.format("cancel", formattedDir));
            }
            checkAndFlush();
        }
        flushTemporaryBuffers();
        return;
    }

    std::vector<std::pair<std::string, int>> unmountResults;
    unmountResults.reserve(isoDirs.size());

    for (const auto& isoDir : isoDirs) {
        if (!g_operationCancelled.load()) {
			int result = umount2(isoDir.c_str(), MNT_DETACH);
			unmountResults.emplace_back(isoDir, result);
		}
    }

    for (const auto& [dir, result] : unmountResults) {
		if (!g_operationCancelled.load()) {
			bool isEmpty = isDirectoryEmpty(dir);
			auto dirParts = parseMountPointComponents(dir);
			std::string formattedDir;
			if (displayConfig::toggleFullListUmount) 
				formattedDir = std::get<0>(dirParts);
			formattedDir += std::get<1>(dirParts);
			if (displayConfig::toggleFullListUmount) 
				formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";

			if (result == 0 || isEmpty) {
				if (isEmpty && rmdir(dir.c_str()) == 0) {
					successMessages.push_back(messageFormatter.format("success", formattedDir));
					completedTasks->fetch_add(1);
				}
			} else {
				errorMessages.push_back(messageFormatter.format("error", formattedDir));
				failedTasks->fetch_add(1);
			}
			checkAndFlush();
		}
    }

    if (g_operationCancelled.load()) {
        for (size_t i = unmountResults.size(); i < isoDirs.size(); ++i) {
            const std::string& isoDir = isoDirs[i];
            auto dirParts = parseMountPointComponents(isoDir);
            std::string formattedDir;
            if (displayConfig::toggleFullListUmount) 
                formattedDir = std::get<0>(dirParts);
            formattedDir += std::get<1>(dirParts);
            if (displayConfig::toggleFullListUmount) 
                formattedDir += "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
            
            errorMessages.push_back(messageFormatter.format("cancel", formattedDir));
            failedTasks->fetch_add(1);
            checkAndFlush();
        }
    }

    flushTemporaryBuffers();
}


// Main function to send ISOs for unmount
void prepareUnmount(const std::string& input, const std::vector<std::string>& currentFiles, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose) {
    // Setup signal handler at the start of the operation
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
    for (int index : indicesToProcess) {
        selectedMountpoints.push_back(currentFiles[index - 1]);
	}

    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing" << (selectedMountpoints.size() > 1 ? " tasks" : " task") << " for \033[1;93mumount\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";
	std::string coloredProcess = std::string("\033[1;93mumount\033[0;1m") + (selectedMountpoints.size() > 1 ? " tasks" : " task");

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
        &verbose,
        std::string(coloredProcess)
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
    }

    // Cleanup
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
}

