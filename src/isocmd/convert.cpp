// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/types.h>
#include <unistd.h>

// Project Headers
#include "../globalMutexes.h"
#include "../convert.h"
#include "../display.h"
#include "../state.h"
#include "../verbose.h"
#include "../stringManipulation.h"
#include "../themes.h"

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

    ConversionThemeStrings themes = getConversionThemeStrings();

    uid_t real_uid; gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs;

    auto batchInsertMessages = [&]() {
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
        if (!localSuccessMsgs.empty()) {
            verboseSets.operationCompleted.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            localSuccessMsgs.clear();
        }
        if (!localFailedMsgs.empty()) {
            verboseSets.operationFailed.insert(localFailedMsgs.begin(), localFailedMsgs.end());
            localFailedMsgs.clear();
        }
        if (!localSkippedMsgs.empty()) {
            verboseSets.operationSkipped.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            localSkippedMsgs.clear();
        }
    };

    for (const std::string& inputPath : imageFiles) {
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");

        // Build displayPath without an intermediate std::string when possible
        std::string displayPath;
        if (!displayConfig::toggleNamesOnly) {
            displayPath.reserve(directory.size() + 1 + fileNameOnly.size());
            displayPath.append(directory).append("/").append(fileNameOnly);
        } else {
            displayPath = fileNameOnly; // single assignment, no concat
        }

        if (!fs::exists(inputPath)) {
            std::string msg;
            msg.reserve(themes.missingLabel.size() * 2 + themes.errPath.size() +
                        displayPath.size() + 32);
            msg.append(themes.missingLabel).append("Convert2ISO: ")
               .append(themes.errPath).append("'").append(displayPath).append("'")
               .append(themes.missingLabel).append(": Failed → MissingFile.");
            localFailedMsgs.push_back(std::move(msg));

            {
                std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
                auto& cache = modeNrg ? GlobalState::nrgFilesCache :
                              (modeMdf ? GlobalState::mdfMdsFilesCache :
                               (modeChd ? GlobalState::chdFilesCache :
                                (modeDaa ? GlobalState::daaGbiFilesCache : GlobalState::binImgFilesCache)));
                cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());
            }

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        if (!std::ifstream(inputPath)) {  // avoid keeping file handle open longer than needed
            std::string msg;
            msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() +
                        displayPath.size() + 48);
            msg.append(themes.errLabel).append("Convert2ISO: ")
               .append(themes.errPath).append("'").append(displayPath).append("'")
               .append(themes.errLabel).append(": Failed → NoAccess.");
            localFailedMsgs.push_back(std::move(msg));

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            if (localFailedMsgs.size() >= BATCH_SIZE) batchInsertMessages();
            continue;
        }

        // Build outputPath in-place: replace extension with ".iso"
        std::string outputPath;
        {
            const size_t dotPos = inputPath.find_last_of('.');
            const size_t baseLen = (dotPos != std::string::npos) ? dotPos : inputPath.size();
            outputPath.reserve(baseLen + 4);
            outputPath.assign(inputPath, 0, baseLen);
            outputPath.append(".iso");
        }

        if (fileExists(outputPath)) {
            std::string msg;
            msg.reserve(themes.skipLabel.size() * 2 + themes.skipPath.size() +
                        displayPath.size() + 32);
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

            if (successfulOutputPaths && outPathsMutex) {
                std::lock_guard<std::mutex> lock(*outPathsMutex);
                successfulOutputPaths->push_back(outputPath); // can't move; needed below
            }

            // Determine file type from extension without copying the full filename
            std::string_view fileType = "Image";
            {
                const size_t dotPos = fileNameOnly.find_last_of('.');
                if (dotPos != std::string_view::npos) {
                    std::string_view ext = std::string_view(fileNameOnly).substr(dotPos + 1);
                    // Case-insensitive compare via known small set
                    auto iequal = [](std::string_view a, const char* b) {
                        if (a.size() != std::strlen(b)) return false;
                        for (size_t i = 0; i < a.size(); ++i)
                            if (std::tolower((unsigned char)a[i]) != b[i]) return false;
                        return true;
                    };
                    if      (iequal(ext, "bin")) fileType = "BIN";
                    else if (iequal(ext, "img")) fileType = "IMG";
                    else if (iequal(ext, "mdf")) fileType = "MDF";
                    else if (iequal(ext, "nrg")) fileType = "NRG";
                    else if (iequal(ext, "chd")) fileType = "CHD";
                    else if (iequal(ext, "daa")) fileType = "DAA";
                    else if (iequal(ext, "gbi")) fileType = "GBI";
                }
            }

            std::string msg;
            msg.reserve(themes.okLabel.size() * 2 + themes.okPath.size() +
                        outputPath.size() + fileType.size() + 16);
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
            msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() +
                        displayPath.size() + 24);
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
