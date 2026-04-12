// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @brief Checks if a file exists at the specified path.
 * @param fullPath The absolute or relative path to the file.
 * @return true if the file exists, false otherwise.
 */
bool fileExists(const std::string& fullPath) {
    return std::filesystem::exists(fullPath);
}

/**
 * @brief Batch converts disk image files (BIN, IMG, MDF, NRG, CHD, CCD, DAA) to ISO format.
 *
 * Handles per-file validation (existence and basic readability), skips existing outputs,
 * selects the appropriate conversion backend based on mode flags, and aggregates
 * success/failed/skipped messages in a thread-safe manner.
 *
 * Also updates file ownership for outputs, removes invalid cache entries, and triggers
 * a database refresh if new ISO files are successfully created.
 */
void convertToISO(const std::vector<std::string>& imageFiles,
                  std::unordered_set<std::string>& successOuts,
                  std::unordered_set<std::string>& skippedOuts,
                  std::unordered_set<std::string>& failedOuts,
                  const bool& modeMdf,
                  const bool& modeNrg,
                  const bool& modeChd,
                  const bool& modeDaa,
                  std::atomic<size_t>* completedBytes,
                  std::atomic<size_t>* completedTasks,
                  std::atomic<size_t>* failedTasks,
                  std::atomic<bool>& newISOFound) {

    namespace fs = std::filesystem;
    const size_t BATCH_SIZE = 50;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view errLabel     = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath      = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view missingLabel = isOriginal ? originalColors::purple   : theme->secondary;
    std::string_view okLabel      = isOriginal ? originalColors::boldAlt  : theme->muted;
    std::string_view okPath       = isOriginal ? originalColors::green    : theme->primary;
    std::string_view skipLabel    = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view skipPath     = isOriginal ? originalColors::green    : theme->primary;

    // Collect unique directories for later database refresh
    std::unordered_set<std::string> uniqueDirectories;
    for (const auto& filePath : imageFiles) {
        fs::path path(filePath);
        if (path.has_parent_path())
            uniqueDirectories.insert(path.parent_path().string());
    }
    std::string result = std::accumulate(uniqueDirectories.begin(), uniqueDirectories.end(), std::string(),
        [](const std::string& a, const std::string& b) {
            return a.empty() ? b : a + ";" + b;
        });

    uid_t real_uid; gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    // Local buffers for batch insertion
    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs;

    auto batchInsertMessages = [&]() {
        if (localSuccessMsgs.size() >= BATCH_SIZE ||
            localFailedMsgs.size()  >= BATCH_SIZE ||
            localSkippedMsgs.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            failedOuts.insert(localFailedMsgs.begin(),  localFailedMsgs.end());
            skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            localSuccessMsgs.clear();
            localFailedMsgs.clear();
            localSkippedMsgs.clear();
        }
    };

    size_t processedCount = 0;
    bool cancellationEverSeen = false;

    for (size_t idx = 0; idx < imageFiles.size(); ++idx) {
        const std::string& inputPath = imageFiles[idx];

        // Check for cancellation before any work
        if (g_operationCancelled.load()) {
            cancellationEverSeen = true;
            break;
        }

        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
        const std::string displayPath  = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

        // Missing file
        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(missingLabel).append("Missing file: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(missingLabel).append(".");
            localFailedMsgs.push_back(std::move(msg));

            auto& cache = modeNrg ? nrgFilesCache :
                          (modeMdf ? mdfMdsFilesCache :
                           (modeChd ? chdFilesCache :
                            (modeDaa ? daaFilesCache : binImgFilesCache)));
            cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            ++processedCount;
            continue;
        }

        // Readability
        std::ifstream file(inputPath);
        if (!file.good()) {
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("The specified file ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(errLabel).append(" cannot be read. Check permissions.")
               .append(originalColors::boldAlt).append(originalColors::boldAlt);
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            ++processedCount;
            continue;
        }

        // Output already exists
        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(skipLabel).append("ISO already exists for: ")
               .append(skipPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(skipLabel).append(". Skipped conversion.")
               .append(originalColors::boldAlt).append(originalColors::boldAlt);
            localSkippedMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            ++processedCount;
            continue;
        }

        // Check for cancellation before heavy conversion
        if (g_operationCancelled.load()) {
            cancellationEverSeen = true;
            break;
        }

        // Perform conversion — return value is the sole authority on why
        // the converter stopped, immune to any external flag races
        ConversionResult conversionResult;
        if (modeMdf)
            conversionResult = convertMdfToIso(inputPath, outputPath, completedBytes);
        else if (modeNrg)
            conversionResult = convertNrgToIso(inputPath, outputPath, completedBytes);
        else if (modeChd)
            conversionResult = convertChdToIso(inputPath, outputPath, completedBytes);
        else if (modeDaa)
            conversionResult = convertDaaToIso(inputPath, outputPath, completedBytes);
        else
            conversionResult = convertCcdToIso(inputPath, outputPath, completedBytes);

        // Latch cancellation from the converter's own determination
        if (conversionResult == ConversionResult::Cancelled) {
            cancellationEverSeen = true;
        }

        auto [outDir, outName] = extractDirectoryAndFilename(outputPath, "conversions");

        if (conversionResult == ConversionResult::Success) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid);

            std::string fileNameLower = fileNameOnly;
            toLowerInPlace(fileNameLower);
            std::string_view fileType = fileNameLower.ends_with(".bin") ? "BIN" :
                                        fileNameLower.ends_with(".img") ? "IMG" :
                                        fileNameLower.ends_with(".mdf") ? "MDF" :
                                        fileNameLower.ends_with(".nrg") ? "NRG" :
                                        fileNameLower.ends_with(".chd") ? "CHD" :
                                        fileNameLower.ends_with(".daa") ? "DAA" : "Image";

            std::string msg;
            msg.reserve(128);
            msg.append(okLabel).append(fileType).append(" file converted to ISO: ")
               .append(okPath).append("'").append(outDir).append("/").append(outName).append("'")
               .append(originalColors::boldAlt).append(okLabel).append(".");
            localSuccessMsgs.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (fs::exists(outputPath))
                fs::remove(outputPath);

            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Conversion of ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(errLabel).append(" ");

            if (conversionResult == ConversionResult::Cancelled || cancellationEverSeen) {
                msg.append("cancelled.");
            } else {
                msg.append("failed.");
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
            }
            msg.append(originalColors::boldAlt).append(originalColors::boldAlt);
            localFailedMsgs.push_back(std::move(msg));
        }
        batchInsertMessages();
        ++processedCount;

        if (cancellationEverSeen) {
            break;
        }
    }

    // Report all remaining files as cancelled (no failure counter)
    if (cancellationEverSeen && processedCount < imageFiles.size()) {
        for (size_t idx = processedCount; idx < imageFiles.size(); ++idx) {
            const std::string& inputPath = imageFiles[idx];
            auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
            const std::string displayPath = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Conversion of ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(errLabel).append(" cancelled (operation aborted).")
               .append(originalColors::boldAlt).append(originalColors::boldAlt);
            localFailedMsgs.push_back(std::move(msg));
        }
    }

    // Flush remaining messages
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
        failedOuts.insert(localFailedMsgs.begin(),  localFailedMsgs.end());
        skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
    }

    // Trigger database refresh only if we have successes
    if (!successOuts.empty()) {
        bool pFlag = false, fHistory = false;
        int mDepth = 0;
        refreshForDatabase(result, pFlag, mDepth, fHistory, newISOFound);
    }
}
