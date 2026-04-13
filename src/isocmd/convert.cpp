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
                  std::atomic<size_t>* failedTasks) {

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

    uid_t real_uid; gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs;

    auto batchInsertMessages = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        if (!localSuccessMsgs.empty()) {
            successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            localSuccessMsgs.clear();
        }
        if (!localFailedMsgs.empty()) {
            failedOuts.insert(localFailedMsgs.begin(), localFailedMsgs.end());
            localFailedMsgs.clear();
        }
        if (!localSkippedMsgs.empty()) {
            skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            localSkippedMsgs.clear();
        }
    };

    for (const std::string& inputPath : imageFiles) {
        if (g_operationCancelled.load(std::memory_order_relaxed)) break;

        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
        const std::string displayPath  = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(missingLabel).append("Convert2ISO: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(missingLabel).append(": missing file.");
            localFailedMsgs.push_back(std::move(msg));

            {
                std::lock_guard<std::mutex> lock(globalSetsMutex);
                auto& cache = modeNrg ? nrgFilesCache :
                              (modeMdf ? mdfMdsFilesCache :
                               (modeChd ? chdFilesCache :
                                (modeDaa ? daaFilesCache : binImgFilesCache)));
                cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());
            }

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        std::ifstream file(inputPath);
        if (!file.good()) {
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Convert2ISO: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(errLabel).append(": cannot be read, check permissions.");
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(skipLabel).append("Convert2ISO: ")
               .append(skipPath).append("'").append(displayPath).append("'")
               .append(skipLabel).append(": ISO already exists, skipped.");
            localSkippedMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localSkippedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        if (g_operationCancelled.load(std::memory_order_relaxed)) break;

        bool conversionSuccess = false;
        if (modeMdf)       conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        else if (modeNrg)  conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        else if (modeChd)  conversionSuccess = convertChdToIso(inputPath, outputPath, completedBytes);
        else if (modeDaa)  conversionSuccess = convertDaaToIso(inputPath, outputPath, completedBytes);
        else               conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);

        if (conversionSuccess) {
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
            msg.append(okLabel).append("Convert2ISO: ")
               .append(okPath).append("'").append(displayPath).append("'")
               .append(okLabel).append(": ").append(fileType).append(" → ISO.");
            localSuccessMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localSuccessMsgs.size() >= BATCH_SIZE) batchInsertMessages();
        } else {
            if (fs::exists(outputPath)) fs::remove(outputPath);

            bool isCancelled = g_operationCancelled.load(std::memory_order_relaxed);
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Convert2ISO: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(errLabel).append(": ").append(isCancelled ? "cancelled." : "failed.");
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
        }
    }

    batchInsertMessages();
}
