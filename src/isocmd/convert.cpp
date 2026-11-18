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

namespace {

/**
 * @brief Represents the outcome of validating a target image file's accessibility.
 */
enum class ValidationResult {
    Valid,    ///< File exists and can be opened for reading.
    Missing,  ///< File does not exist on disk.
    NoAccess  ///< File exists but cannot be opened (permission/access issue).
};

/**
 * @brief Validates if an image file exists and is readable.
 * @param inputPath The absolute or relative path to the image file.
 * @return ValidationResult::Valid if accessible, ValidationResult::Missing if absent,
 *         or ValidationResult::NoAccess if stream opening fails.
 */
ValidationResult validateImageFile(const std::string& inputPath) {
    if (!std::filesystem::exists(inputPath)) {
        return ValidationResult::Missing;
    }
    if (!std::ifstream(inputPath)) {
        return ValidationResult::NoAccess;
    }
    return ValidationResult::Valid;
}

/**
 * @brief Removes an invalid or missing image path from the appropriate global state cache.
 * @param inputPath The path to remove from the cache.
 * @param modeMdf Flag indicating MDF/MDS format mode.
 * @param modeNrg Flag indicating NRG format mode.
 * @param modeChd Flag indicating CHD format mode.
 * @param modeDaa Flag indicating DAA/GBI format mode.
 */
void removeFromCache(const std::string& inputPath, bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa) {
    std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
    auto& cache = modeNrg ? GlobalState::nrgFilesCache :
                  (modeMdf ? GlobalState::mdfMdsFilesCache :
                   (modeChd ? GlobalState::chdFilesCache :
                    (modeDaa ? GlobalState::daaGbiFilesCache : GlobalState::binImgFilesCache)));
    cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());
}

/**
 * @brief Executes the appropriate conversion backend based on active format mode flags.
 * @param modeMdf Flag for MDF/MDS format conversion.
 * @param modeNrg Flag for NRG format conversion.
 * @param modeChd Flag for CHD format conversion.
 * @param modeDaa Flag for DAA/GBI format conversion.
 * @param inputPath The source image file path.
 * @param outputPath The target destination ISO file path.
 * @param completedBytes Pointer to an atomic counter tracking total processed bytes.
 * @return true if the backend conversion operation succeeds, false otherwise.
 */
bool runConversion(bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa,
                   const std::string& inputPath, const std::string& outputPath,
                   std::atomic<size_t>* completedBytes) {
    if (modeMdf)      return convertMdfToIso(inputPath, outputPath, completedBytes);
    if (modeNrg)      return convertNrgToIso(inputPath, outputPath, completedBytes);
    if (modeChd)      return convertChdToIso(inputPath, outputPath, completedBytes);
    if (modeDaa)      return convertDaaToIso(inputPath, outputPath, completedBytes);
    return convertCcdToIso(inputPath, outputPath, completedBytes);
}

/**
 * @brief Determines the file extension and maps it to a normalized upper-case image type string.
 * @param fileNameOnly The filename string_view (including extension) to inspect.
 * @return A string_view representing the recognized image type (e.g., "BIN", "NRG", "CHD"),
 *         or "Image" as a fallback.
 */
std::string_view getFileType(std::string_view fileNameOnly) {
    const size_t dotPos = fileNameOnly.find_last_of('.');
    if (dotPos == std::string_view::npos) return "Image";

    std::string_view ext = fileNameOnly.substr(dotPos + 1);
    auto iequal = [](std::string_view a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
        return true;
    };

    if (iequal(ext, "bin")) return "BIN";
    if (iequal(ext, "img")) return "IMG";
    if (iequal(ext, "mdf")) return "MDF";
    if (iequal(ext, "nrg")) return "NRG";
    if (iequal(ext, "chd")) return "CHD";
    if (iequal(ext, "daa")) return "DAA";
    if (iequal(ext, "gbi")) return "GBI";
    return "Image";
}

} // namespace

/**
 * @file convert_iso.cpp
 * @brief Disk image batch conversion utilities.
 */

namespace {

/**
 * @brief Formats the display path for logging based on configuration flags.
 *
 * @param directory The extracted directory path view.
 * @param fileNameOnly The extracted filename view.
 * @return std::string The formatted display path.
 */
std::string formatDisplayPath(std::string_view directory, std::string_view fileNameOnly) {
    if (!displayConfig::toggleNamesOnly) {
        std::string displayPath;
        displayPath.reserve(directory.size() + 1 + fileNameOnly.size());
        displayPath.append(directory).append("/").append(fileNameOnly);
        return displayPath;
    }
    return std::string(fileNameOnly);
}

/**
 * @brief Derives the destination ISO file path from the source image path.
 *
 * @param inputPath The source image file path.
 * @return std::string The target .iso file path.
 */
std::string deriveOutputPath(const std::string& inputPath) {
    const size_t dotPos = inputPath.find_last_of('.');
    const size_t baseLen = (dotPos != std::string::npos) ? dotPos : inputPath.size();
    std::string outputPath;
    outputPath.reserve(baseLen + 4);
    outputPath.assign(inputPath, 0, baseLen);
    outputPath.append(".iso");
    return outputPath;
}

/**
 * @brief Handles validation failure for an image file.
 *
 * @param inputPath Source file path.
 * @param displayPath Formatted display path for messages.
 * @param valRes The validation result enum.
 * @param themes Theme strings configuration.
 * @param modeMdf MDF/MDS mode flag.
 * @param modeNrg NRG mode flag.
 * @param modeChd CHD mode flag.
 * @param modeDaa DAA/GBI mode flag.
 * @param localFailedMsgs Local batch collection for failed messages.
 * @param failedTasks Atomic counter for failed tasks.
 * @param batchSize Threshold size for batch flushing.
 * @param batchInsertFunc Callback function to flush messages.
 */
void handleValidationFailure(const std::string& inputPath,
                             const std::string& displayPath,
                             ValidationResult valRes,
                             const ConversionThemeStrings& themes,
                             bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa,
                             std::vector<std::string>& localFailedMsgs,
                             std::atomic<size_t>* failedTasks,
                             size_t batchSize,
                             const std::function<void()>& batchInsertFunc) {
    std::string msg;
    if (valRes == ValidationResult::Missing) {
        msg.reserve(themes.missingLabel.size() * 2 + themes.errPath.size() + displayPath.size() + 32);
        msg.append(themes.missingLabel).append("Convert2ISO: ")
           .append(themes.errPath).append("'").append(displayPath).append("'")
           .append(themes.missingLabel).append(": Failed → MissingFile.");
        removeFromCache(inputPath, modeMdf, modeNrg, modeChd, modeDaa);
    } else {
        msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() + displayPath.size() + 48);
        msg.append(themes.errLabel).append("Convert2ISO: ")
           .append(themes.errPath).append("'").append(displayPath).append("'")
           .append(themes.errLabel).append(": Failed → NoAccess.");
    }
    localFailedMsgs.push_back(std::move(msg));
    failedTasks->fetch_add(1, std::memory_order_acq_rel);
    if (localFailedMsgs.size() >= batchSize) {
        batchInsertFunc();
    }
}

/**
 * @brief Handles the case where the output ISO file already exists.
 *
 * @param displayPath Formatted display path for messages.
 * @param themes Theme strings configuration.
 * @param localSkippedMsgs Local batch collection for skipped messages.
 * @param completedTasks Atomic counter for completed/skipped tasks.
 * @param batchSize Threshold size for batch flushing.
 * @param batchInsertFunc Callback function to flush messages.
 */
void handleExistingOutput(const std::string& displayPath,
                         const ConversionThemeStrings& themes,
                         std::vector<std::string>& localSkippedMsgs,
                         std::atomic<size_t>* completedTasks,
                         size_t batchSize,
                         const std::function<void()>& batchInsertFunc) {
    std::string msg;
    msg.reserve(themes.skipLabel.size() * 2 + themes.skipPath.size() + displayPath.size() + 32);
    msg.append(themes.skipLabel).append("Convert2ISO: ")
       .append(themes.skipPath).append("'").append(displayPath).append("'")
       .append(themes.skipLabel).append(": ISOalrExists → Skipped.");
    localSkippedMsgs.push_back(std::move(msg));
    completedTasks->fetch_add(1, std::memory_order_acq_rel);
    if (localSkippedMsgs.size() >= batchSize) {
        batchInsertFunc();
    }
}

/**
 * @brief Handles a successful conversion result.
 *
 * @param outputPath Destination ISO file path.
 * @param fileNameOnly Filename view for type detection.
 * @param real_uid Real User ID for ownership update.
 * @param real_gid Real Group ID for ownership update.
 * @param themes Theme strings configuration.
 * @param localSuccessMsgs Local batch collection for success messages.
 * @param completedTasks Atomic counter for completed tasks.
 * @param successfulOutputPaths Optional vector to collect successful paths.
 * @param outPathsMutex Mutex guarding successfulOutputPaths.
 * @param batchSize Threshold size for batch flushing.
 * @param batchInsertFunc Callback function to flush messages.
 */
void handleSuccessfulConversion(const std::string& outputPath,
                               std::string_view fileNameOnly,
                               uid_t real_uid, gid_t real_gid,
                               const ConversionThemeStrings& themes,
                               std::vector<std::string>& localSuccessMsgs,
                               std::atomic<size_t>* completedTasks,
                               std::vector<std::string>* successfulOutputPaths,
                               std::mutex* outPathsMutex,
                               size_t batchSize,
                               const std::function<void()>& batchInsertFunc) {
    [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid);

    if (successfulOutputPaths && outPathsMutex) {
        std::lock_guard<std::mutex> lock(*outPathsMutex);
        successfulOutputPaths->push_back(outputPath);
    }

    std::string_view fileType = getFileType(fileNameOnly);
    std::string msg;
    msg.reserve(themes.okLabel.size() * 2 + themes.okPath.size() + outputPath.size() + fileType.size() + 16);
    msg.append(themes.okLabel).append("Convert2ISO: ")
       .append(themes.okPath).append("'").append(outputPath).append("'")
       .append(themes.okLabel).append(": ").append(fileType).append(" → ISO.");
    localSuccessMsgs.push_back(std::move(msg));

    completedTasks->fetch_add(1, std::memory_order_acq_rel);
    if (localSuccessMsgs.size() >= batchSize) {
        batchInsertFunc();
    }
}

/**
 * @brief Handles a failed conversion result.
 *
 * @param outputPath Destination ISO file path to clean up if partial.
 * @param displayPath Formatted display path for messages.
 * @param themes Theme strings configuration.
 * @param localFailedMsgs Local batch collection for failed messages.
 * @param failedTasks Atomic counter for failed tasks.
 * @param batchSize Threshold size for batch flushing.
 * @param batchInsertFunc Callback function to flush messages.
 */
void handleFailedConversion(const std::string& outputPath,
                           const std::string& displayPath,
                           const ConversionThemeStrings& themes,
                           std::vector<std::string>& localFailedMsgs,
                           std::atomic<size_t>* failedTasks,
                           size_t batchSize,
                           const std::function<void()>& batchInsertFunc) {
    namespace fs = std::filesystem;
    if (fs::exists(outputPath)) fs::remove(outputPath);

    bool isCancelled = GlobalState::g_operationCancelled.load(std::memory_order_relaxed);
    std::string msg;
    msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() + displayPath.size() + 24);
    msg.append(themes.errLabel).append("Convert2ISO: ")
       .append(themes.errPath).append("'").append(displayPath).append("'")
       .append(themes.errLabel).append(": ").append(isCancelled ? "Cancelled." : "Failed.");
    localFailedMsgs.push_back(std::move(msg));

    failedTasks->fetch_add(1, std::memory_order_acq_rel);
    if (localFailedMsgs.size() >= batchSize) {
        batchInsertFunc();
    }
}

} // namespace

/**
 * @brief Batch converts disk image files (BIN, IMG, MDF, NRG, CHD, CCD, DAA, GBI) to ISO format.
 *
 * Handles per-file validation (existence and basic readability), skips existing outputs,
 * selects the appropriate conversion backend based on mode flags, and aggregates
 * success/failed/skipped messages in a thread-safe manner.
 *
 * Also updates file ownership for outputs, removes invalid cache entries, and
 * collects successful output paths for later database indexing by the caller.
 *
 * @param imageFiles               Vector containing absolute or relative paths of source images.
 * @param modeMdf                  True if converting MDF/MDS images.
 * @param modeNrg                  True if converting NRG images.
 * @param modeChd                  True if converting CHD images.
 * @param modeDaa                  True if converting DAA/GBI images.
 * @param completedBytes           Atomic counter to track cumulative processed bytes.
 * @param completedTasks           Atomic counter to track successfully completed or skipped tasks.
 * @param failedTasks              Atomic counter to track failed conversion tasks.
 * @param successfulOutputPaths    Optional pointer to a vector storing successfully generated ISO paths.
 * @param outPathsMutex            Optional pointer to a mutex guarding @p successfulOutputPaths.
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
        std::string displayPath = formatDisplayPath(directory, fileNameOnly);

        ValidationResult valRes = validateImageFile(inputPath);
        if (valRes != ValidationResult::Valid) {
            handleValidationFailure(inputPath, displayPath, valRes, themes, modeMdf, modeNrg, modeChd, modeDaa,
                                    localFailedMsgs, failedTasks, BATCH_SIZE, batchInsertMessages);
            continue;
        }

        std::string outputPath = deriveOutputPath(inputPath);

        if (fileExists(outputPath)) {
            handleExistingOutput(displayPath, themes, localSkippedMsgs, completedTasks,
                                 BATCH_SIZE, batchInsertMessages);
            continue;
        }

        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

        bool conversionSuccess = runConversion(modeMdf, modeNrg, modeChd, modeDaa, inputPath, outputPath, completedBytes);

        if (conversionSuccess) {
            handleSuccessfulConversion(outputPath, fileNameOnly, real_uid, real_gid, themes,
                                       localSuccessMsgs, completedTasks, successfulOutputPaths,
                                       outPathsMutex, BATCH_SIZE, batchInsertMessages);
        } else {
            handleFailedConversion(outputPath, displayPath, themes, localFailedMsgs,
                                   failedTasks, BATCH_SIZE, batchInsertMessages);
        }
    }

    batchInsertMessages();
}
