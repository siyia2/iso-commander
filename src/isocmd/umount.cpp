// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"
#include "../umount.h"
#include "../themes.h"

/**
 * @brief Checks if a directory is empty.
 * @details Used primarily to determine if a mount point can be safely removed 
 * if a umount2 call fails but the directory contains no data.
 * * @param path The absolute path to the directory.
 * @return True if the directory exists and contains no entries, false otherwise.
 */
bool isDirectoryEmpty(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    
    return std::filesystem::directory_iterator(path) == 
           std::filesystem::directory_iterator();
}

/**
 * @brief Formats a mount point path for console output.
 * @details Uses parseMountPointComponents to break the path into segments and
 * applies ANSI styling based on the user's display configuration.
 * * @param isoDir The directory path to format.
 * @param fmt Reference to the VerboseMessageFormatter.
 * @param messageKey The key for the specific message template (e.g., "success", "error").
 * @return A styled string ready for display.
 */
static std::string formatDirForDisplay(const std::string& isoDir, VerboseMessageFormatter& fmt, const char* messageKey) {
    auto dirParts = parseMountPointComponents(isoDir);
    std::string formattedDir = std::get<1>(dirParts);
    std::string_view squareColor = originalColors::dimGray;

    if (displayConfig::toggleFullListUmount) {
		formattedDir = std::get<0>(dirParts) + formattedDir 
					   + std::string(squareColor) + std::get<2>(dirParts) + "\033[0m";
	}
    return fmt.format(messageKey, formattedDir);
}

/**
 * @brief Performs unmount operations on a list of ISO mount points.
 * @details Uses the Linux umount2 system call with MNT_DETACH (lazy unmount). 
 * It handles root permission checks, cancellation signals, and cleans up 
 * empty mount point directories. Results are collected in thread-safe sets.
 * * @param isoDirs Vector of directory paths to unmount.
 * @param unmountedFiles Set to store successful unmount messages.
 * @param unmountedErrors Set to store error/cancellation messages.
 * @param completedTasks Atomic counter for successful operations.
 * @param failedTasks Atomic counter for failed operations.
 * @param silentMode If true, suppresses message formatting and set insertion.
 */
void unmountISO(const std::vector<std::string>& isoDirs, 
                std::unordered_set<std::string>& unmountedFiles, 
                std::unordered_set<std::string>& unmountedErrors, 
                std::atomic<size_t>* completedTasks, 
                std::atomic<size_t>* failedTasks, 
                bool silentMode) {

    const bool hasRoot = (geteuid() == 0);
    VerboseMessageFormatter messageFormatter;
    std::vector<std::string> errorMessages, successMessages;

    if (!silentMode) {
        const size_t BATCH_SIZE = 50;
        errorMessages.reserve(BATCH_SIZE);
        successMessages.reserve(BATCH_SIZE);
    }

    // Transfers local batch buffers to the global shared sets
    auto flushTemporaryBuffers = [&]() {
        if (silentMode) return;
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

    auto isCancelled = []() { return g_operationCancelled.load(); };

    if (!hasRoot) {
        for (const auto& isoDir : isoDirs) {
            if (!silentMode) {
                errorMessages.push_back(formatDirForDisplay(isoDir, messageFormatter, "root_error"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        flushTemporaryBuffers();
        return;
    }

    for (const auto& isoDir : isoDirs) {
        if (isCancelled()) {
            if (!silentMode) {
                errorMessages.push_back(formatDirForDisplay(isoDir, messageFormatter, "cancel"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // MNT_DETACH allows the mount point to be unmounted even if busy (lazy unmount)
        const int result = umount2(isoDir.c_str(), MNT_DETACH);

        if (result == 0) {
            rmdir(isoDir.c_str());
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (!silentMode) {
                successMessages.push_back(formatDirForDisplay(isoDir, messageFormatter, "success"));
            }
        } else {
            // Check if the mount is already gone but the directory remains
            if (isDirectoryEmpty(isoDir)) {
                rmdir(isoDir.c_str());
                completedTasks->fetch_add(1, std::memory_order_acq_rel);
                if (!silentMode) {
                    successMessages.push_back(formatDirForDisplay(isoDir, messageFormatter, "success"));
                }
            } else {
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                if (!silentMode) {
                    errorMessages.push_back(formatDirForDisplay(isoDir, messageFormatter, "error"));
                }
            }
        }

        // Flush in batches to reduce lock contention on globalSetsMutex
        if (!silentMode && (successMessages.size() >= 50 || errorMessages.size() >= 50)) {
            flushTemporaryBuffers();
        }
    }

    flushTemporaryBuffers();
}
