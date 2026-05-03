// SPDX-License-Identifier: GPL-3.0-or-later

// Third-Party Library Headers
#include <libmount/libmount.h>

// Project Headers
#include "../concurrency.h"
#include "../display.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../umount.h"
#include "../verbose.h"

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
 * @details Breaks the path into segments using parseMountPointComponents. 
 * Converts non-owning string_views into a styled, owning std::string based 
 * on ANSI palette settings and user display configuration.
 * 
 * @param isoDir The directory path to format.
 * @param fmt Reference to the VerboseMessageFormatter.
 * @param messageKey The key for the specific message template.
 * @return A fully constructed, styled string ready for display.
 */
static std::string formatDirForDisplay(const std::string& isoDir, VerboseMessageFormatter& fmt, const char* messageKey) {
    auto dirParts = parseMountPointComponents(isoDir);
    
    std::string formattedDir{std::get<1>(dirParts)};
    std::string_view squareColor = UI::Palette::DimGray;

    if (displayConfig::toggleFullListUmount) {
        formattedDir = std::string(std::get<0>(dirParts)) 
                       + formattedDir 
                       + std::string(squareColor) 
                       + std::string(std::get<2>(dirParts)) 
                       + std::string(UI::Palette::BoldReset);
    }
    
    return fmt.format(messageKey, formattedDir);
}

/**
 * @brief Performs unmount operations on a list of ISO mount points.
 * @details Uses the Linux umount2 system call with MNT_DETACH (lazy unmount).
 * Handles root permission checks and cancellation signals, and cleans up
 * empty mount point directories after a successful unmount.
 *
 * Results are written directly to the global verboseSets:
 *   - verboseSets.operationCompleted  Success messages.
 *   - verboseSets.operationFailed     Failure messages (root_error, cancel, error).
 *
 * @param isoDirs        Vector of mount point directories to unmount.
 * @param completedTasks Atomic counter incremented for each successful unmount.
 * @param failedTasks    Atomic counter incremented for each failure (including cancellation).
 * @param silentMode     Suppresses all message generation; only counters are updated.
 *
 * @warning Requires root (geteuid() == 0). Without it every entry gets "root_error".
 */
void unmountISO(
    const std::vector<std::string>& isoDirs,
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
            verboseSets.operationCompleted.insert(successMessages.begin(), successMessages.end());
            successMessages.clear();
        }
        if (!errorMessages.empty()) {
            verboseSets.operationFailed.insert(errorMessages.begin(), errorMessages.end());
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
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
            if (!silentMode)
                errorMessages.push_back(
                    formatDirForDisplay(isoDir, messageFormatter, "cancel"));
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            maybeFlush();
            continue;
        }
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
