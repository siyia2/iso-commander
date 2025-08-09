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
void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool quietMode) {
    bool hasRoot = (geteuid() == 0);
    
    // Only allocate resources if not in quiet mode
    VerboseMessageFormatter messageFormatter;
    std::vector<std::string> errorMessages, successMessages;
    if (!quietMode) {
        const size_t BATCH_SIZE = 1000;
        errorMessages.reserve(BATCH_SIZE);
        successMessages.reserve(BATCH_SIZE);
    }

    // Flush function - no-op in quiet mode
    auto flushTemporaryBuffers = [&]() {
        if (quietMode) return;
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

    // Check cancellation helper
    auto isCancelled = []() { return g_operationCancelled.load(); };

    if (!hasRoot) {
        for (const auto& isoDir : isoDirs) {
            if (isCancelled()) {
                failedTasks->fetch_add(1);
                continue;
            }
            
            if (!quietMode) {
                auto dirParts = parseMountPointComponents(isoDir);
                std::string formattedDir = std::get<1>(dirParts);  // Always include base name
                if (displayConfig::toggleFullListUmount) {
                    formattedDir = std::get<0>(dirParts) + formattedDir + 
                                  "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
                }
                errorMessages.push_back(messageFormatter.format("root_error", formattedDir));
            }
            failedTasks->fetch_add(1);
        }
        if (!quietMode) flushTemporaryBuffers();
        return;
    }

    // Process unmounting
    for (const auto& isoDir : isoDirs) {
        if (isCancelled()) {
            failedTasks->fetch_add(1);
            if (!quietMode) {
                auto dirParts = parseMountPointComponents(isoDir);
                std::string formattedDir = std::get<1>(dirParts);
                if (displayConfig::toggleFullListUmount) {
                    formattedDir = std::get<0>(dirParts) + formattedDir + 
                                  "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
                }
                errorMessages.push_back(messageFormatter.format("cancel", formattedDir));
            }
            continue;
        }

        // Attempt unmount
        int result = umount2(isoDir.c_str(), MNT_DETACH);
        bool directoryEmpty = isDirectoryEmpty(isoDir);
        bool success = (result == 0) || directoryEmpty;

        if (success) {
            // Remove directory if empty
            if (directoryEmpty) {
                rmdir(isoDir.c_str());  // Attempt removal but ignore result
            }
            completedTasks->fetch_add(1);
            
            if (!quietMode) {
                auto dirParts = parseMountPointComponents(isoDir);
                std::string formattedDir = std::get<1>(dirParts);
                if (displayConfig::toggleFullListUmount) {
                    formattedDir = std::get<0>(dirParts) + formattedDir + 
                                  "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
                }
                successMessages.push_back(messageFormatter.format("success", formattedDir));
            }
        } else {
            failedTasks->fetch_add(1);
            if (!quietMode) {
                auto dirParts = parseMountPointComponents(isoDir);
                std::string formattedDir = std::get<1>(dirParts);
                if (displayConfig::toggleFullListUmount) {
                    formattedDir = std::get<0>(dirParts) + formattedDir + 
                                  "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
                }
                errorMessages.push_back(messageFormatter.format("error", formattedDir));
            }
        }

        // Batch flushing check
        if (!quietMode && 
            (successMessages.size() >= 1000 || errorMessages.size() >= 1000)) {
            flushTemporaryBuffers();
        }
    }

    if (!quietMode) flushTemporaryBuffers();
}
