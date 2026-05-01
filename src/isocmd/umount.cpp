// SPDX-License-Identifier: GPL-3.0-or-later

#include "../threadpool.h"
#include "../display.h"
#include "../umount.h"
#include "../themes.h"
#include "../concurrency.h"
#include "../stringManipulation.h"

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
    std::string_view squareColor = UI::Palette::DimGray;

    if (displayConfig::toggleFullListUmount) {
		formattedDir = std::get<0>(dirParts) + formattedDir 
					   + std::string(squareColor) + std::get<2>(dirParts) + std::string(UI::Palette::BoldReset);
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
void unmountISO(
    const std::vector<std::string>& isoDirs,
    std::unordered_set<std::string>& unmountedFiles,
    std::unordered_set<std::string>& unmountedErrors,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    const bool hasRoot = (geteuid() == 0);
    VerboseMessageFormatter messageFormatter;
    std::vector<std::string> errorMessages, successMessages;

    constexpr size_t kBatchSize = 50;
    if (!silentMode) {
        errorMessages.reserve(kBatchSize);
        successMessages.reserve(kBatchSize);
    }

    auto flushTemporaryBuffers = [&]() {
        if (silentMode) return;
        std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
        if (!successMessages.empty()) {
            unmountedFiles.insert(successMessages.begin(), successMessages.end());
            successMessages.clear();
        }
        if (!errorMessages.empty()) {
            unmountedErrors.insert(errorMessages.begin(), errorMessages.end());
            errorMessages.clear();
        }
    };

    auto maybeFlush = [&]() {
        if (!silentMode &&
            (successMessages.size() >= kBatchSize ||
             errorMessages.size()   >= kBatchSize)) {
            flushTemporaryBuffers();
        }
    };

    // ----------------------------------------------------------------
    // Early-out: no root → bulk-increment and flush error messages
    // ----------------------------------------------------------------
    if (!hasRoot) {
        if (!silentMode) {
            for (const auto& isoDir : isoDirs)
                errorMessages.push_back(
                    formatDirForDisplay(isoDir, messageFormatter, "root_error"));
            flushTemporaryBuffers();
        }
        failedTasks->fetch_add(isoDirs.size(), std::memory_order_relaxed);
        return;
    }

    for (const auto& isoDir : isoDirs) {

        if (g_operationCancelled.load(std::memory_order_relaxed)) {
            if (!silentMode)
                errorMessages.push_back(
                    formatDirForDisplay(isoDir, messageFormatter, "cancel"));
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            maybeFlush();
            continue;
        }

        // MNT_DETACH allows lazy unmount even if the mount point is busy
        const int result = umount2(isoDir.c_str(), MNT_DETACH);

        if (result == 0 || isDirectoryEmpty(isoDir)) {
            rmdir(isoDir.c_str());
            completedTasks->fetch_add(1, std::memory_order_relaxed);
            if (!silentMode)
                successMessages.push_back(
                    formatDirForDisplay(isoDir, messageFormatter, "success"));
        } else {
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            if (!silentMode)
                errorMessages.push_back(
                    formatDirForDisplay(isoDir, messageFormatter, "error"));
        }

        maybeFlush();
    }

    flushTemporaryBuffers();
}
