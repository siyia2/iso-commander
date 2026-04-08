/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @brief Checks if a file exists at the specified path.
 */
bool fileExists(const std::string& fullPath) {
    return std::filesystem::exists(fullPath);
}

/**
 * @brief Orchestrates the conversion of ISO files to CHD format.
 */
void convertToCHD(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& successOuts, 
                  std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, 
                  std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, 
                  std::atomic<size_t>* failedTasks, std::atomic<bool>& newCHDFound) {

    namespace fs = std::filesystem;
    const size_t BATCH_SIZE = 50;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // UI Coloring Setup
    std::string_view errLabel      = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath       = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view missingLabel  = isOriginal ? originalColors::purple    : theme->secondary;
    std::string_view okLabel       = isOriginal ? originalColors::boldAlt   : theme->muted;
    std::string_view okPath        = isOriginal ? originalColors::green     : theme->primary;
    std::string_view skipLabel     = isOriginal ? originalColors::yellow    : theme->warning;
    std::string_view skipPath      = isOriginal ? originalColors::green     : theme->primary;

    std::unordered_set<std::string> uniqueDirectories;
    for (const auto& filePath : isoFiles) {
        fs::path path(filePath);
        if (path.has_parent_path()) {
            uniqueDirectories.insert(path.parent_path().string());
        }
    }

    std::string result = std::accumulate(uniqueDirectories.begin(), uniqueDirectories.end(), std::string(),
        [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ";" + b; });

    uid_t real_uid; gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs;

    auto batchInsertMessages = [&]() {
        if (localSuccessMsgs.size() >= BATCH_SIZE || localFailedMsgs.size() >= BATCH_SIZE || localSkippedMsgs.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            failedOuts.insert(localFailedMsgs.begin(),  localFailedMsgs.end());
            skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            localSuccessMsgs.clear(); localFailedMsgs.clear(); localSkippedMsgs.clear();
        }
    };

    for (const std::string& inputPath : isoFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
        const std::string displayPath  = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

        // 1. Basic Existence Check
        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(missingLabel).append("Missing ISO: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(".");
            localFailedMsgs.push_back(std::move(msg));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

        // 2. Read Permission Check
        std::ifstream file(inputPath, std::ios::binary);
        if (!file.good()) {
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Cannot read ISO ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(". Check permissions.");
            localFailedMsgs.push_back(std::move(msg));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }
        file.close();

        // 3. Output Path and Skip Logic
        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".chd";
        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(skipLabel).append("CHD already exists: ")
               .append(skipPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(". Skipped.");
            localSkippedMsgs.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

        // 4. Conversion Logic using libchdr wrapper
        // Note: You need to implement convertIsoToChd in your ccd2iso_...cpp file 
        // using the chd_create/chd_write functions from your library.
        bool conversionSuccess = convertIsoToChd(inputPath, outputPath, completedBytes);

        auto [outDir, outName] = extractDirectoryAndFilename(outputPath, "conversions");

        if (conversionSuccess) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid);

            std::string msg;
            msg.reserve(128);
            msg.append(okLabel).append("ISO converted to CHD: ")
               .append(okPath).append("'").append(outDir).append("/").append(outName).append("'")
               .append(originalColors::boldAlt).append(".");
            localSuccessMsgs.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (fs::exists(outputPath)) fs::remove(outputPath);
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Conversion failed for ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(" ")
               .append(g_operationCancelled.load() ? "cancelled" : "error").append(".");
            localFailedMsgs.push_back(std::move(msg));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        batchInsertMessages();
    }

    // Final Sync
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
        failedOuts.insert(localFailedMsgs.begin(),  localFailedMsgs.end());
        skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
    }

    if (!successOuts.empty()) {
        bool pFlag = false, fHistory = false; int mDepth = 0;
        refreshForDatabase(result, pFlag, mDepth, fHistory, newCHDFound);
    }
}
