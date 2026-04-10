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
 * Special handling is included for DAA multi-part archives: filenames with patterns
 * like `foo.part01.daa` or `foo.part001.daa` are stripped of the `.partNN[N]`
 * suffix to output to `foo.iso` rather than `foo.part01.iso`.
 *
 * Also updates file ownership for outputs, removes invalid cache entries, and triggers
 * a database refresh if new ISO files are successfully created.
 *
 * @param imageFiles     Vector of image file paths to convert.
 * @param successOuts    Set populated with success messages for completed conversions.
 * @param skippedOuts    Set populated with skip messages (e.g., output ISO already exists).
 * @param failedOuts     Set populated with failure messages (e.g., missing file, read error).
 * @param modeMdf        If `true`, use MDF-to-ISO conversion backend.
 * @param modeNrg        If `true`, use NRG-to-ISO conversion backend.
 * @param modeChd        If `true`, use CHD-to-ISO conversion backend.
 * @param modeDaa        If `true`, use DAA-to-ISO conversion backend (handles multi-part archives).
 * @param completedBytes Atomic counter for bytes processed (used by progress bar).
 * @param completedTasks Atomic counter for tasks successfully completed.
 * @param failedTasks    Atomic counter for tasks that failed or were cancelled.
 * @param newISOFound    Atomic flag set to `true` if at least one ISO was successfully created.
 *
 * @note Messages are batched (50 at a time) to reduce lock contention.
 *       The function is thread-safe and designed to be called from multiple worker threads.
 *       On conversion failure, any partially created output ISO is automatically removed.
 */
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, 
                  std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, 
                  const bool& modeMdf, const bool& modeNrg, const bool& modeChd, const bool& modeDaa,
                  std::atomic<size_t>* completedBytes, 
                  std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, 
                  std::atomic<bool>& newISOFound) {

    namespace fs = std::filesystem;
    const size_t BATCH_SIZE = 50;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view errLabel     = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath      = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view missingLabel = isOriginal ? originalColors::purple   : theme->secondary;
    std::string_view okLabel      = isOriginal ? originalColors::boldAlt      : theme->muted;
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

        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(128);
            msg.append(missingLabel).append("Missing file: ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(originalColors::boldAlt).append(".");
            localFailedMsgs.push_back(std::move(msg));

            auto& cache = modeNrg ? nrgFilesCache : (modeMdf ? mdfMdsFilesCache : binImgFilesCache);
            cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            continue;
        }

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
            continue;
        }

        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        // DAA multi-part files (foo.part01.daa / foo.part001.daa) should output
        // to foo.iso, not foo.part01.iso — strip the .partNN[N] suffix if present.
        if (modeDaa) {
            fs::path p(inputPath);
            std::string stem = p.stem().string(); // already stripped ".daa"
            // Check for .partNNN (3-digit) suffix first, then .partNN (2-digit)
            for (int digits : {3, 2}) {
                std::string prefix = ".part";
                size_t suffixLen = prefix.size() + digits;
                if (stem.size() > suffixLen) {
                    std::string candidate = stem.substr(stem.size() - suffixLen);
                    if (candidate.substr(0, prefix.size()) == prefix) {
                        std::string numPart = candidate.substr(prefix.size());
                        if (std::all_of(numPart.begin(), numPart.end(), ::isdigit)) {
                            stem = stem.substr(0, stem.size() - suffixLen);
                            break;
                        }
                    }
                }
            }
            outputPath = (p.parent_path() / (stem + ".iso")).string();
        }

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
            continue;
        }

        bool conversionSuccess = false;
        if (modeMdf) {
            conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        } else if (modeNrg) {
            conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        } else if (modeChd) {
            conversionSuccess = convertChdToIso(inputPath, outputPath, completedBytes);
        } else if (modeDaa) {
            conversionSuccess = convertDaaToIso(inputPath, outputPath, completedBytes);
        } else {
            conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);
        }

        auto [outDir, outName] = extractDirectoryAndFilename(outputPath, "conversions");

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
            msg.append(okLabel).append(fileType).append(" file converted to ISO: ")
               .append(okPath).append("'").append(outDir).append("/").append(outName).append("'")
               .append(originalColors::boldAlt).append(originalColors::boldAlt).append(".");
            localSuccessMsgs.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (fs::exists(outputPath)) fs::remove(outputPath);
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Conversion of ")
               .append(errPath).append("'").append(displayPath).append("'")
               .append(originalColors::boldAlt).append(errLabel).append(" ")
               .append(g_operationCancelled.load() ? "cancelled" : "failed").append(".")
               .append(originalColors::boldAlt).append(originalColors::boldAlt);
            localFailedMsgs.push_back(std::move(msg));
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        batchInsertMessages();
    }

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
