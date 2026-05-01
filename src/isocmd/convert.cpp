// SPDX-License-Identifier: GPL-3.0-or-later

#include "../state.h"
#include "../display.h"
#include "../themes.h"
#include "../convert.h"
#include "../caches.h"
#include "../concurrency.h"
#include "../stringManipulation.h"

/**
 * @brief Checks if a file exists at the specified path.
 * @param fullPath The absolute or relative path to the file.
 * @return true if the file exists, false otherwise.
 */
bool fileExists(const std::string& fullPath) {
    return std::filesystem::exists(fullPath);
}

void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

/**
 * @brief Batch converts disk image files (BIN, IMG, MDF, NRG, CHD, CCD, DAA, GBI) to ISO format.
 *
 * Handles per-file validation (existence and basic readability), skips existing outputs,
 * selects the appropriate conversion backend based on mode flags, and aggregates
 * success/failed/skipped messages in a thread-safe manner.
 *
 * Also updates file ownership for outputs, removes invalid cache entries, and
 * collects successful output paths for later database indexing by the caller.
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
                  std::vector<std::string>* successfulOutputPaths,
                  std::mutex* outPathsMutex) {

    namespace fs = std::filesystem;
    const size_t BATCH_SIZE = 50;

    // Get themed strings from external function
    ConversionThemeStrings themes = getConversionThemeStrings();

    uid_t real_uid; gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs;

    auto batchInsertMessages = [&]() {
        std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
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
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");
        const std::string displayPath  = (!displayConfig::toggleNamesOnly ? directory + "/" : "") + fileNameOnly;

        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(themes.missingLabel).append("Convert2ISO: ")
               .append(themes.errPath).append("'").append(displayPath).append("'")
               .append(themes.missingLabel).append(": Missing file.");
            localFailedMsgs.push_back(std::move(msg));

            {
                std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
                auto& cache = modeNrg ? GlobalCaches::nrgFilesCache :
                              (modeMdf ? GlobalCaches::mdfMdsFilesCache :
                               (modeChd ? GlobalCaches::chdFilesCache :
                                (modeDaa ? GlobalCaches::daaGbiFilesCache : GlobalCaches::binImgFilesCache)));
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
            msg.append(themes.errLabel).append("Convert2ISO: ")
               .append(themes.errPath).append("'").append(displayPath).append("'")
               .append(themes.errLabel).append(": Cannot be read, check permissions.");
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(themes.skipLabel).append("Convert2ISO: ")
               .append(themes.skipPath).append("'").append(displayPath).append("'")
               .append(themes.skipLabel).append(": ISOalrExists → Skipped.");
            localSkippedMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localSkippedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

        bool conversionSuccess = false;
        if (modeMdf)       conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        else if (modeNrg)  conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        else if (modeChd)  conversionSuccess = convertChdToIso(inputPath, outputPath, completedBytes);
        else if (modeDaa)  conversionSuccess = convertDaaToIso(inputPath, outputPath, completedBytes);
        else               conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);

        if (conversionSuccess) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid);

            // Record the successful output path
            if (successfulOutputPaths && outPathsMutex) {
                std::lock_guard<std::mutex> lock(*outPathsMutex);
                successfulOutputPaths->push_back(outputPath);
            }

            std::string fileNameLower = fileNameOnly;
            toLowerInPlace(fileNameLower);
            std::string_view fileType = fileNameLower.ends_with(".bin") ? "BIN" :
                                        fileNameLower.ends_with(".img") ? "IMG" :
                                        fileNameLower.ends_with(".mdf") ? "MDF" :
                                        fileNameLower.ends_with(".nrg") ? "NRG" :
                                        fileNameLower.ends_with(".chd") ? "CHD" :
                                        fileNameLower.ends_with(".daa") ? "DAA" :
                                        fileNameLower.ends_with(".gbi") ? "GBI" : "Image";

            std::string msg;
            msg.reserve(128);
            msg.append(themes.okLabel).append("Convert2ISO: ")
               .append(themes.okPath).append("'").append(outputPath).append("'")
               .append(themes.okLabel).append(": ").append(fileType).append(" → ISO.");
            localSuccessMsgs.push_back(std::move(msg));

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localSuccessMsgs.size() >= BATCH_SIZE) batchInsertMessages();
        } else {
            if (fs::exists(outputPath)) fs::remove(outputPath);

            bool isCancelled = GlobalState::g_operationCancelled.load(std::memory_order_relaxed);
            std::string msg;
            msg.reserve(128);
            msg.append(themes.errLabel).append("Convert2ISO: ")
               .append(themes.errPath).append("'").append(displayPath).append("'")
               .append(themes.errLabel).append(": ").append(isCancelled ? "Cancelled." : "Failed.");
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
        }
    }

    batchInsertMessages();
}
