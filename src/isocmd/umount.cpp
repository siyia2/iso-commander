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
