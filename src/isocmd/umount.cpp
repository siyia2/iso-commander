// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../display.h"
#include "../umount.h"


bool isDirectoryEmpty(const std::string& path) {
    if (!std::filesystem::exists(path))
        return false;
    return std::filesystem::directory_iterator(path) ==
           std::filesystem::directory_iterator();
}


// Extract repeated format block into a helper used everywhere in the loop
static std::string formatDir(
    const std::string& isoDir,
    VerboseMessageFormatter& fmt,
    const char* messageKey)
{
    auto dirParts   = parseMountPointComponents(isoDir);
    std::string dir = std::get<1>(dirParts);
    if (displayConfig::toggleFullListUmount) {
        dir = std::get<0>(dirParts) + dir +
              "\033[38;5;245m" + std::get<2>(dirParts) + "\033[0m";
    }
    return fmt.format(messageKey, dir);
}


void unmountISO(
    const std::vector<std::string>& isoDirs,
    std::unordered_set<std::string>& unmountedFiles,
    std::unordered_set<std::string>& unmountedErrors,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    // Const — geteuid() never changes mid-process
    const bool hasRoot = (geteuid() == 0);

    VerboseMessageFormatter messageFormatter;
    std::vector<std::string> errorMessages, successMessages;
    if (!silentMode) {
        constexpr size_t BATCH_SIZE = 1000;
        errorMessages.reserve(BATCH_SIZE);
        successMessages.reserve(BATCH_SIZE);
    }

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
            if (!silentMode)
                errorMessages.push_back(
                    formatDir(isoDir, messageFormatter, "root_error"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        flushTemporaryBuffers();  // safe — no-ops in silentMode
        return;
    }

    for (const auto& isoDir : isoDirs) {
        if (isCancelled()) {
            if (!silentMode)
                errorMessages.push_back(
                    formatDir(isoDir, messageFormatter, "cancel"));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        const int result = umount2(isoDir.c_str(), MNT_DETACH);

        if (result == 0) {
            // Attempt cleanup regardless of isDirectoryEmpty —
            // MNT_DETACH may leave dir appearing non-empty briefly;
            // rmdir() will simply fail with EBUSY in that case, which is fine
            rmdir(isoDir.c_str());
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (!silentMode)
                successMessages.push_back(
                    formatDir(isoDir, messageFormatter, "success"));
        } else {
            // Only treat as "already gone" if the path doesn't exist
            // at all — not just because the dir happens to be empty
            if (!std::filesystem::exists(isoDir)) {
                rmdir(isoDir.c_str());
                completedTasks->fetch_add(1, std::memory_order_acq_rel);
                if (!silentMode)
                    successMessages.push_back(
                        formatDir(isoDir, messageFormatter, "success"));
            } else {
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                if (!silentMode)
                    errorMessages.push_back(
                        formatDir(isoDir, messageFormatter, "error"));
            }
        }

        if (!silentMode &&
            (successMessages.size() >= 1000 || errorMessages.size() >= 1000))
        {
            flushTemporaryBuffers();
        }
    }

    flushTemporaryBuffers();
}
