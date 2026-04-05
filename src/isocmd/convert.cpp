// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"


// Function to check if a file already exists for conversion output
bool fileExists(const std::string& fullPath) {
        return std::filesystem::exists(fullPath);
}


// Function to convert a BIN/IMG/MDF/NRG file to ISO format
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, 
                  std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, 
                  const bool& modeMdf, const bool& modeNrg, std::atomic<size_t>* completedBytes, 
                  std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::atomic<bool>& newISOFound) {

    namespace fs = std::filesystem;
    const size_t BATCH_SIZE = 1000;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // Semantic color mapping using global struct
    std::string_view errLabel     = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath      = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view missingLabel = isOriginal ? originalColors::purple   : theme->secondary;
    std::string_view okLabel      = isOriginal ? originalColors::bold     : theme->muted;
    std::string_view okPath       = isOriginal ? originalColors::green    : theme->primary;
    std::string_view skipLabel    = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view skipPath     = isOriginal ? originalColors::green    : theme->primary;

    std::unordered_set<std::string> uniqueDirectories;
    for (const auto& filePath : imageFiles) {
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

    for (const std::string& inputPath : imageFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
        const std::string displayPath  = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

        // 1. Check Existence
        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(missingLabel).append("Missing file: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::reset).append(originalColors::boldAlt).append(".");
            localFailedMsgs.push_back(std::move(msg));

            auto& cache = modeNrg ? nrgFilesCache : (modeMdf ? mdfMdsFilesCache : binImgFilesCache);
            cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

        // 2. Check Readability
        std::ifstream file(inputPath);
        if (!file.good()) {
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("The specified file ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::reset).append(errLabel).append(" cannot be read. Check permissions.")
               .append(originalColors::reset).append(originalColors::boldAlt);
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

        // 3. Check for existing ISO (Skip logic)
        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(skipLabel).append("ISO already exists for: ")
               .append(skipPath).append("'").append(displayPath).append("'")
               .append(originalColors::reset).append(skipLabel).append(". Skipped conversion.")
               .append(originalColors::reset).append(originalColors::boldAlt);
            localSkippedMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

        // 4. Perform Conversion
        bool conversionSuccess = false;
        if (modeMdf)            conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        else if (modeNrg)       conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        else                    conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);

        auto [outDir, outName] = extractDirectoryAndFilename(outputPath, "conversions");

        if (conversionSuccess) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid);

            std::string fileNameLower = fileNameOnly;
            toLowerInPlace(fileNameLower);
            std::string_view fileType = fileNameLower.ends_with(".bin") ? "BIN" :
                                        fileNameLower.ends_with(".img") ? "IMG" :
                                        fileNameLower.ends_with(".mdf") ? "MDF" :
                                        fileNameLower.ends_with(".nrg") ? "NRG" : "Image";
            std::string msg;
            msg.reserve(128);
            msg.append(okLabel).append(fileType).append(" file converted to ISO: ")
               .append(okPath).append("'").append(outDir).append("/").append(outName).append("'")
               .append(originalColors::reset).append(originalColors::boldAlt).append(".");
            localSuccessMsgs.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (fs::exists(outputPath)) fs::remove(outputPath);
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Conversion of ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::reset).append(errLabel).append(" ")
               .append(g_operationCancelled.load() ? "cancelled" : "failed").append(".")
               .append(originalColors::reset).append(originalColors::boldAlt);
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
        refreshForDatabase(result, pFlag, mDepth, fHistory, newISOFound);
    }
}
