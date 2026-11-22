// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <ctype.h>
#include <stddef.h>
#include <unistd.h>

// Project Headers
#include "isocmd/cli/cmdAutomation.h"
#include "isocmd/core/concurrency.h"
#include "isocmd/ops/convert.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/ops/mount.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/core/state.h"
#include "isocmd/ui/themes.h"
#include "isocmd/core/threadpool.h"
#include "isocmd/ops/umount.h"
#include "isocmd/ui/verbose.h"
#include "isocmd/usb/write2usbUI.h"

#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

void convertToISO(
    const std::vector<std::string>& imageFiles,
    const ConversionMode& mode,
    const ConversionBatchData& batchData);

size_t calculateTotalBytesForConversions(const std::vector<std::string>& filesToProcess,
                                         const bool modeMdf, const bool modeNrg,
                                         const bool modeChd, const bool modeDaa);

void displayProgressBarWithSize(std::atomic<size_t>* completedBytes,
                                size_t totalBytes,
                                std::atomic<size_t>* completedTasks,
                                std::atomic<size_t>* failedTasks,
                                size_t totalTasks,
                                std::atomic<bool>* isComplete,
                                bool* verbose, const std::string& operation,
                                const std::string& header);

// Shared helpers defined in cmdAutomationMountUmount.cpp
namespace IsoCli {
void verboseInfo(bool silentMode, const std::string& msg);
void verboseWarn(bool silentMode, const std::string& msg);
void warnMsg(bool silentMode, std::string_view reason, std::string_view path);
void errMsg(std::string_view msg);
} // namespace IsoCli
using namespace IsoCli;

// ─── Image file discovery (for c2iso) ───────────────────────────────────────────

enum class ImgKind { None, BinImg, Mdf, Nrg, Chd, Daa };

/**
 * @brief Map a file extension to the conversion backend that handles it.
 *        BIN/IMG/CCD share the default backend (convertCcdToIso).
 */
static ImgKind classifyImage(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".bin" || ext == ".img" || ext == ".ccd") return ImgKind::BinImg;
    if (ext == ".mdf") return ImgKind::Mdf;
    if (ext == ".nrg") return ImgKind::Nrg;
    if (ext == ".chd") return ImgKind::Chd;
    if (ext == ".daa" || ext == ".gbi") return ImgKind::Daa;
    return ImgKind::None;
}

/**
 * @brief Recursively scan @p dir for convertible disk images.
 *        Same depth/symlink/error semantics as scanDirectoryForISOs().
 */
static void scanDirectoryForImages(const fs::path& dir,
                                   int maxDepth,
                                   int currentDepth,
                                   std::unordered_set<std::string>& out,
                                   bool& hasErrors,
                                   bool silentMode) {
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (GlobalState::g_operationCancelled.load()) return;
            if (entry.is_symlink()) continue;

            if (entry.is_regular_file()) {
                if (classifyImage(entry.path()) != ImgKind::None)
                    out.insert(fs::canonical(entry.path()).string());
            } else if (entry.is_directory() &&
                       (maxDepth == -1 || currentDepth < maxDepth)) {
                scanDirectoryForImages(entry.path(), maxDepth,
                                       currentDepth + 1,
                                       out, hasErrors, silentMode);
            }
        }
    } catch (const fs::filesystem_error& e) {
        warnMsg(silentMode,
                std::string("Error scanning directory: ") + e.what(),
                dir.string());
        hasErrors = true;
    }
}

// ─── Convert branch (c2iso) ─────────────────────────────────────────────────────

/**
 * @brief Represents a conversion task with its target file path and backend kind.
 */
struct ConversionTask {
    std::string path;
    ImgKind kind;
};

/**
 * @brief Collects and validates disk image paths from command-line arguments and directories.
 */
static bool collectAndValidateImages(const ParsedArgs& args,
                                      std::unordered_set<std::string>& images,
                                      bool& hasErrors) {

    for (const auto& rawPath : args.paths) {
        if (GlobalState::g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return true; // Cancelled
        }

        fs::path path(rawPath);

        try {
            if (!fs::exists(path)) {
                warnMsg(args.silentMode, "does not exist, skipping.", rawPath);
                hasErrors = true;

            } else if (fs::is_regular_file(path)) {
                if (classifyImage(path) == ImgKind::None) {
                    warnMsg(args.silentMode,
                            "is not a supported image (bin/img/ccd/mdf/nrg/chd/daa/gbi), skipping.",
                            rawPath);
                    hasErrors = true;
                } else {
                    images.insert(fs::canonical(path).string());
                }

            } else if (fs::is_directory(path)) {
                disableInput();

                std::string depthDesc = (args.maxDepth == 0)
                    ? std::string("surface scan")
                    : std::string("max depth: ") +
                      (args.maxDepth < 0
                           ? "unlimited"
                           : std::to_string(args.maxDepth));

                verboseInfo(args.silentMode,
                            std::string("Scanning directory ")
                            .append(path.string())
                            .append(" (").append(depthDesc).append(")..."));

                scanDirectoryForImages(path, args.maxDepth, 0,
                                       images, hasErrors, args.silentMode);

            } else {
                warnMsg(args.silentMode,
                        "is not a valid file or directory, skipping.", rawPath);
                hasErrors = true;
            }

        } catch (const fs::filesystem_error& e) {
            warnMsg(args.silentMode,
                    std::string("Error processing path: ") + e.what(), rawPath);
            hasErrors = true;
        }
    }
    return false;
}

/**
 * @brief Executes concurrent conversion workers across prepared tasks.
 */
static void runConvertWorkers(const std::vector<ConversionTask>& tasks,
                              std::atomic<size_t>& completedBytes,
                              std::atomic<size_t>& completedTasks,
                              std::atomic<size_t>& failedTasks,
                              std::vector<std::string>& successfulOutputs,
                              std::mutex& outMutex) {
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();

    const size_t numThreads = std::min({
        tasks.size(),
        GlobalConcurrency::CONV_THREAD_CAP,
        poolSize
    });

    std::atomic<size_t> nextTask{0};

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (size_t t = 0; t < numThreads; ++t) {
        futures.emplace_back(pool.enqueue(
            [&tasks, &nextTask,
             &completedBytes, &completedTasks, &failedTasks,
             &successfulOutputs, &outMutex]() {

                while (!GlobalState::g_operationCancelled.load()) {
                    const size_t i = nextTask.fetch_add(1);

                    if (i >= tasks.size())
                        break;

                    const auto& task = tasks[i];

                    const ConversionMode mode{
                        .mdf = task.kind == ImgKind::Mdf,
                        .nrg = task.kind == ImgKind::Nrg,
                        .chd = task.kind == ImgKind::Chd,
                        .daa = task.kind == ImgKind::Daa
                    };

                    const ConversionBatchData batchData{
                        .completedBytes = &completedBytes,
                        .completedTasks = &completedTasks,
                        .failedTasks = &failedTasks,
                        .successfulOutputPaths = &successfulOutputs,
                        .outPathsMutex = &outMutex
                    };

                    convertToISO({task.path}, mode, batchData);
                }
            }));
    }

    for (auto& future : futures)
        future.wait();
}

// ------------------------------------------------------------------------- //
// Conversion helpers                                                        //
// ------------------------------------------------------------------------- //

/**
 * @brief Groups supported disk images by conversion backend.
 *
 * Classifies each image according to its format, appends it to the
 * corresponding backend collection, and sorts every collection
 * alphabetically to ensure deterministic processing order.
 *
 * Images that do not match a supported conversion backend are ignored.
 *
 * @param images Set of validated image paths to classify.
 * @param binImg Receives BIN/IMG image paths.
 * @param mdf    Receives MDF image paths.
 * @param nrg    Receives NRG image paths.
 * @param chd    Receives CHD image paths.
 * @param daa    Receives DAA image paths.
 */
static void groupConversionImages(
    const std::unordered_set<std::string>& images,
    std::vector<std::string>& binImg,
    std::vector<std::string>& mdf,
    std::vector<std::string>& nrg,
    std::vector<std::string>& chd,
    std::vector<std::string>& daa) {

    for (const auto& file : images) {
        switch (classifyImage(file)) {
            case ImgKind::BinImg: binImg.push_back(file); break;
            case ImgKind::Mdf:    mdf.push_back(file); break;
            case ImgKind::Nrg:    nrg.push_back(file); break;
            case ImgKind::Chd:    chd.push_back(file); break;
            case ImgKind::Daa:    daa.push_back(file); break;
            default: break;
        }
    }

    for (auto* files : {&binImg, &mdf, &nrg, &chd, &daa})
        std::sort(files->begin(), files->end());
}

/**
 * @brief Calculates the estimated total output size of conversion tasks.
 *
 * Uses the format-specific size estimation logic for each backend and
 * accumulates the results to determine the total byte count required
 * by the conversion progress display.
 *
 * Returns a minimum value of one byte when the estimated total is zero
 * to prevent division-by-zero conditions in progress calculations.
 *
 * @param binImg BIN/IMG image paths.
 * @param mdf    MDF image paths.
 * @param nrg    NRG image paths.
 * @param chd    CHD image paths.
 * @param daa    DAA image paths.
 *
 * @return Estimated total output size in bytes, or 1 if the estimate
 *         is zero.
 */
static size_t calculateConversionTotalBytes(
    const std::vector<std::string>& binImg,
    const std::vector<std::string>& mdf,
    const std::vector<std::string>& nrg,
    const std::vector<std::string>& chd,
    const std::vector<std::string>& daa) {

    size_t totalBytes = 0;

    totalBytes += calculateTotalBytesForConversions(
        binImg, false, false, false, false);
    totalBytes += calculateTotalBytesForConversions(
        mdf, true, false, false, false);
    totalBytes += calculateTotalBytesForConversions(
        nrg, false, true, false, false);
    totalBytes += calculateTotalBytesForConversions(
        chd, false, false, true, false);
    totalBytes += calculateTotalBytesForConversions(
        daa, false, false, false, true);

    return totalBytes == 0 ? 1 : totalBytes;
}

/**
 * @brief Initializes and starts the conversion progress display.
 *
 * Constructs the operation header, clears the terminal scroll buffer,
 * and launches the progress-monitoring thread when silent mode is
 * disabled.
 *
 * The progress thread monitors the supplied atomic counters and
 * completion flag while updating the conversion progress display.
 * The verbose-output flag is shared with the caller so that the
 * progress interface can communicate whether detailed results
 * should be displayed after conversion.
 *
 * In silent mode, no terminal output or background thread is created.
 *
 * @param args          Parsed command-line arguments controlling output.
 * @param completedBytes Atomic counter tracking completed output bytes.
 * @param totalBytes     Estimated total output size in bytes.
 * @param completedTasks Atomic counter tracking successful conversions.
 * @param failedTasks    Atomic counter tracking failed conversions.
 * @param totalTasks     Total number of conversion tasks.
 * @param isComplete     Atomic flag indicating completion of all workers.
 * @param showVerbose    Shared flag indicating whether detailed results
 *                       should be displayed.
 *
 * @return A joinable progress-monitoring thread when output is enabled,
 *         or a non-joinable thread in silent mode.
 */
static std::thread startConversionProgress(
    const ParsedArgs& args,
    std::atomic<size_t>& completedBytes,
    size_t totalBytes,
    std::atomic<size_t>& completedTasks,
    std::atomic<size_t>& failedTasks,
    size_t totalTasks,
    std::atomic<bool>& isComplete,
    bool& showVerbose) {

    if (args.silentMode)
        return {};

    const std::string coloredProcess =
        std::string(UI::Palette::Orange) + "c2iso" +
        std::string(UI::Palette::BoldReset);

    std::ostringstream hdr;
    hdr << "\n" << color << " Processing "
        << (totalTasks == 1 ? "task for " : "tasks for ")
        << UI::Palette::Orange << "c2iso" << color << "... ("
        << UI::Palette::Red << "Ctrl+c"
        << color << ":cancel)\n";

    const std::string header = hdr.str();

    clearScrollBuffer();
    std::cout << header;

    return std::thread(
        displayProgressBarWithSize,
        &completedBytes, totalBytes,
        &completedTasks, &failedTasks, totalTasks,
        &isComplete, &showVerbose,
        coloredProcess, header);
}

/**
 * @brief Constructs conversion tasks in deterministic processing order.
 *
 * Converts the format-specific image collections into a unified task
 * list, preserving the established backend ordering and the alphabetical
 * ordering of files within each backend.
 *
 * Reserves sufficient capacity before appending tasks to avoid
 * unnecessary vector reallocations.
 *
 * @param binImg BIN/IMG image paths.
 * @param mdf    MDF image paths.
 * @param nrg    NRG image paths.
 * @param chd    CHD image paths.
 * @param daa    DAA image paths.
 *
 * @return A vector containing all conversion tasks in processing order.
 */
static std::vector<ConversionTask> buildConversionTasks(
    const std::vector<std::string>& binImg,
    const std::vector<std::string>& mdf,
    const std::vector<std::string>& nrg,
    const std::vector<std::string>& chd,
    const std::vector<std::string>& daa) {

    std::vector<ConversionTask> tasks;

    const size_t totalTasks =
        binImg.size() + mdf.size() + nrg.size() +
        chd.size() + daa.size();

    tasks.reserve(totalTasks);

    auto appendTasks = [&](const std::vector<std::string>& files,
                           ImgKind kind) {
        for (const auto& file : files)
            tasks.push_back({file, kind});
    };

    appendTasks(binImg, ImgKind::BinImg);
    appendTasks(mdf,    ImgKind::Mdf);
    appendTasks(nrg,    ImgKind::Nrg);
    appendTasks(chd,    ImgKind::Chd);
    appendTasks(daa,    ImgKind::Daa);

    return tasks;
}

/**
 * @brief Displays detailed results of the conversion operation.
 *
 * Outputs completed, skipped, and failed operation messages, followed
 * by a summary of successful and failed conversions.
 *
 * Detailed output is displayed only when silent mode is disabled and
 * the progress interface has explicitly requested verbose results.
 *
 * Synchronizes terminal output through SyncOutput to prevent
 * interference with other terminal-writing components.
 *
 * @param silentMode     Whether terminal output is suppressed.
 * @param showVerbose    Whether detailed operation results were requested.
 * @param completedTasks Atomic counter containing successful conversions.
 * @param failedTasks    Atomic counter containing failed conversions.
 */
static void displayConversionResults(
    bool silentMode,
    bool showVerbose,
    const std::atomic<size_t>& completedTasks,
    const std::atomic<size_t>& failedTasks) {

    if (silentMode || !showVerbose)
        return;

    SyncOutput sync;
    clearScrollBuffer();
    std::cout << "\n";

    for (const auto& msg : verboseSets.operationCompleted)
        std::cout << msg << UI::Palette::BoldReset << "\n";

    for (const auto& msg : verboseSets.operationSkipped)
        std::cout << msg << UI::Palette::BoldReset << "\n";

    for (const auto& msg : verboseSets.operationFailed)
        std::cout << msg << UI::Palette::BoldReset << "\n";

    std::cout << "\nConversion summary:\n"
              << " Successful: " << completedTasks.load() << "\n"
              << " Failed:     " << failedTasks.load()
              << UI::Palette::Reset << "\n\n";
}

/**
 * @brief Determines the final exit status of the conversion operation.
 *
 * Treats the operation as unsuccessful if any conversion failed,
 * input-path validation reported errors, or cancellation was requested.
 *
 * @param failedTasks Number of failed conversion tasks.
 * @param hasErrors   Whether input-path validation encountered errors.
 * @param cancelled   Whether the operation was cancelled.
 *
 * @return 0 if the operation completed successfully; otherwise 1.
 */
static int getConversionExitStatus(
    size_t failedTasks,
    bool hasErrors,
    bool cancelled) {

    return (failedTasks == 0 && !hasErrors && !cancelled) ? 0 : 1;
}

// ------------------------------------------------------------------------- //
// handleConvert                                                              //
// ------------------------------------------------------------------------- //

/**
 * @brief Converts supported disk images into ISO files.
 *
 * Returns immediately with a notice if no paths were supplied.
 * Otherwise validates the supplied input paths, recursively discovers
 * supported disk images according to the configured scan depth, and
 * prepares conversion tasks grouped by their respective backends.
 *
 * Calculates the estimated total output size and initializes the
 * progress display when silent mode is disabled. Conversion tasks
 * are executed concurrently through the shared worker infrastructure,
 * which manages task distribution, conversion progress, and failures.
 *
 * Waits for all conversion workers and the progress-monitoring thread
 * to finish before optionally displaying detailed operation results.
 *
 * @param args Parsed command-line arguments containing input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if all conversions completed successfully, or if no paths
 *         were given, or if no convertible images were found and no
 *         path errors occurred.
 *         Returns 1 if any input path was invalid, any conversion
 *         failed, or the operation was cancelled.
 */
int handleConvert(const ParsedArgs& args) {
    std::unordered_set<std::string> images;
    bool hasErrors = false;

    if (args.paths.empty()) {
        verboseWarn(args.silentMode, "No files to c2iso.");
        return 0;
    }

    if (collectAndValidateImages(args, images, hasErrors))
        return 1;

    if (GlobalState::g_operationCancelled.load())
        verboseWarn(args.silentMode, "Conversion cancelled by user.");

    if (images.empty()) {
        if (GlobalState::g_operationCancelled.load())
            return 1;

        verboseWarn(args.silentMode,
                    "No convertible image files found.");

        return hasErrors ? 1 : 0;
    }

    // Group and sort images by conversion backend.
    std::vector<std::string> binImg, mdf, nrg, chd, daa;

    groupConversionImages(
        images, binImg, mdf, nrg, chd, daa);

    verboseInfo(args.silentMode,
                std::string("Located ")
                .append(std::to_string(images.size()))
                .append(" image file")
                .append(images.size() == 1 ? "" : "s")
                .append("; Attempting to convert...\n"));

    std::atomic<size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
    std::atomic<bool> isComplete{false};

    std::vector<std::string> successfulOutputs;
    std::mutex outMutex;
    bool showVerbose = false;

    // Calculate estimated output size.
    const size_t totalBytes = calculateConversionTotalBytes(
        binImg, mdf, nrg, chd, daa);

    // Start progress display.
    std::thread barThread = startConversionProgress(
        args,
        completedBytes,
        totalBytes,
        completedTasks,
        failedTasks,
        images.size(),
        isComplete,
        showVerbose);

    // Build and execute conversion tasks.
    const auto tasks = buildConversionTasks(
        binImg, mdf, nrg, chd, daa);

    runConvertWorkers(
        tasks,
        completedBytes,
        completedTasks,
        failedTasks,
        successfulOutputs,
        outMutex);

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // Display detailed results if requested.
    displayConversionResults(
        args.silentMode,
        showVerbose,
        completedTasks,
        failedTasks);

    return getConversionExitStatus(
        failedTasks.load(),
        hasErrors,
        GlobalState::g_operationCancelled.load());
}

// ─── Write-to-USB branch (w2usb) ────────────────────────────────────────────

/**
 * @brief Validates root privileges and partitions command-line arguments into ISOs and devices.
 *
 * Checks that the current process has root privileges (`geteuid() == 0`) and separates
 * input paths starting with `/dev/` into device arguments and all other paths into ISO arguments.
 * Validates that at least one ISO and one target device are specified, and enforces
 * that either a single ISO is used for all devices or counts match one-to-one.
 *
 * @param args Parsed command-line arguments containing paths and execution flags.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] isoArgs Output vector populated with extracted ISO file paths.
 * @param[out] devArgs Output vector populated with extracted target device paths.
 * @return true if all prerequisite and argument count checks pass; false otherwise.
 */
static bool validatePrerequisitesAndArgs(const ParsedArgs& args, const WriteTheme& wt,
                                       std::vector<std::string>& isoArgs,
                                       std::vector<std::string>& devArgs) {
    using namespace WriteValidation;

    if (geteuid() != 0) {
        printValidationErrors(wt, {plainErr(wt, "Root privileges are required")});
        return false;
    }

    for (const auto& a : args.paths) {
        (a.rfind("/dev/", 0) == 0 ? devArgs : isoArgs).push_back(a);
    }

    if (isoArgs.empty()) {
        printValidationErrors(wt, {plainErr(wt, "No ISO files specified")});
        return false;
    }
    if (devArgs.empty()) {
        printValidationErrors(wt, {plainErr(wt, "No target devices specified")});
        return false;
    }
    if (isoArgs.size() != 1 && isoArgs.size() != devArgs.size()) {
        printValidationErrors(wt, {plainErr(wt,
            "Got " + std::to_string(isoArgs.size()) + " ISOs and " +
            std::to_string(devArgs.size()) +
            " devices; use 1 ISO for all devices, or one device per ISO")});
        return false;
    }
    return true;
}

/**
 * @brief Validates a collection of ISO file paths and populates their metadata.
 *
 * Iterates through raw ISO path arguments, verifying that each entry points to a
 * valid regular file with a `.iso` extension, is non-empty, and is readable.
 * Successfully validated files are converted into structured `IsoInfo` objects.
 *
 * @param isoArgs Vector of raw ISO file path strings from command-line arguments.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] errors Collection accumulating formatted validation error strings.
 * @return std::vector<IsoInfo> A vector containing metadata for all successfully validated ISOs.
 */
static std::vector<IsoInfo> validateIsos(const std::vector<std::string>& isoArgs,
                                          const WriteTheme& wt,
                                          std::vector<std::string>& errors) {
    using namespace WriteValidation;

    std::vector<IsoInfo> isos;
    for (const auto& raw : isoArgs) {
        std::error_code ec;
        fs::path p(raw);
        if (!fs::is_regular_file(p, ec)) {
            errors.push_back(pathErr(wt, raw, " is not a regular file"));
            continue;
        }
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".iso") {
            errors.push_back(pathErr(wt, raw, " is not an ISO file"));
            continue;
        }
        const auto size = fs::file_size(p, ec);
        if (ec || size == 0) {
            errors.push_back(pathErr(wt, raw, " is empty or unreadable"));
            continue;
        }
        const fs::path canon = fs::canonical(p, ec);
        const std::string full = ec ? p.string() : canon.string();
        isos.push_back(IsoInfo{full, fs::path(full).filename().string(), size,
                               formatFileSize(size), isos.size() + 1});
    }
    return isos;
}

/**
 * @brief Validates target storage devices for write operations.
 *
 * Resolves canonical device paths, checks for duplicate device entries in the argument list,
 * and delegates device-specific checks (such as active flushing states) via
 * @ref WriteValidation::checkDevice.
 *
 * @param devArgs Vector of raw target device path strings.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] errors Collection accumulating formatted validation error strings.
 * @param[out] devices Output vector populated with validated canonical device paths.
 * @param[out] deviceSizes Output vector populated with the capacities (in bytes) of validated devices.
 */
static void validateDevices(const std::vector<std::string>& devArgs,
                            const WriteTheme& wt,
                            std::vector<std::string>& errors,
                            std::vector<std::string>& devices,
                            std::vector<uint64_t>& deviceSizes) {
    using namespace WriteValidation;
    std::unordered_set<std::string> seen;
    for (const auto& raw : devArgs) {
        std::error_code ec;
        const fs::path canon = fs::canonical(raw, ec);
        const std::string dev = ec ? raw : canon.string();

        if (!seen.insert(dev).second) {
            errors.push_back(pathErr(wt, raw, " is listed more than once"));
            continue;
        }
        uint64_t size = 0;
        if (auto err = checkDevice(wt, dev, raw, size)) {
            errors.push_back(std::move(*err));
            continue;
        }
        devices.push_back(dev);
        deviceSizes.push_back(size);
    }
}

/**
 * @brief Executes write operations in either interactive mode or silent asynchronous threads.
 *
 * If silent mode is disabled, delegates execution to the interactive UI writer.
 * If silent mode is enabled, disables standard input and dispatches parallel tasks
 * to the static thread pool, managing futures and tracking completion/failure statuses.
 *
 * @param silentMode Flag indicating whether to skip the live progress UI.
 * @param pairsPtr Shared pointer containing paired ISO metadata and target device paths.
 * @param progressDataPtr Shared pointer to the progress state tracking vector.
 * @param pool Reference to the static thread pool used for concurrent writes.
 */
static void executeWriteOperations(bool silentMode,
                                    std::shared_ptr<std::vector<std::pair<IsoInfo, std::string>>> pairsPtr,
                                    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr,
                                    ThreadPool& pool) {
    if (!silentMode) {
        performWriteOperation(*pairsPtr);
        return;
    }

    disableInput();

    std::vector<std::future<void>> futures;
    futures.reserve(pairsPtr->size());

    for (size_t i = 0; i < pairsPtr->size(); ++i) {
        futures.emplace_back(pool.enqueue(
            [pairsPtr, progressDataPtr, i]() {
                const auto& [iso, device] = (*pairsPtr)[i];

                const bool success = writeIsoToDevice(
                    iso.path, device, i, *progressDataPtr);

                auto& progress = (*progressDataPtr)[i];

                if (success) {
                    progress.completed.store(true);
                } else if (!GlobalState::g_operationCancelled.load()) {
                    progress.failed.store(true);
                }
            }));
    }

    for (auto& future : futures) {
        if (future.valid())
            future.wait();
    }
}

/**
 * @brief Collects capacity validation errors across all target devices.
 *
 * Pairs each target device with either the single supplied ISO or the corresponding
 * positional ISO, then appends any error returned by `WriteValidation::checkCapacity`.
 *
 * @param wt Active `write2usb` UI theme used for formatting error messages.
 * @param isos Validated ISO metadata entries (must have size 1 or match `devices.size()`).
 * @param devices Validated target device paths.
 * @param deviceSizes Capacity in bytes corresponding to each entry in `devices`.
 * @param[in,out] errors Error list to which capacity check failures are appended.
 */
static void collectCapacityErrors(const WriteTheme& wt,
                                  const std::vector<IsoInfo>& isos,
                                  const std::vector<std::string>& devices,
                                  const std::vector<uint64_t>& deviceSizes,
                                  std::vector<std::string>& errors) {
    for (size_t d = 0; d < devices.size(); ++d) {
        const IsoInfo& iso = (isos.size() == 1) ? isos[0] : isos[d];
        if (auto err = WriteValidation::checkCapacity(wt, iso, devices[d], deviceSizes[d])) {
            errors.push_back(std::move(*err));
        }
    }
}

/**
 * @brief Builds ISO-to-device write pairs from validated ISOs and devices.
 *
 * Broadcasts a single ISO across all target devices when `isos.size() == 1`;
 * otherwise pairs `isos[d]` with `devices[d]` in order.
 *
 * @param isos Validated ISO metadata entries.
 * @param devices Validated target device paths.
 * @return std::vector<std::pair<IsoInfo, std::string>> Ordered list of write pairs.
 */
static std::vector<std::pair<IsoInfo, std::string>> buildWritePairs(
    const std::vector<IsoInfo>& isos,
    const std::vector<std::string>& devices) {
    std::vector<std::pair<IsoInfo, std::string>> pairs;
    pairs.reserve(devices.size());
    for (size_t d = 0; d < devices.size(); ++d) {
        pairs.emplace_back(isos.size() == 1 ? isos[0] : isos[d], devices[d]);
    }
    return pairs;
}

/**
 * @brief Allocates and initializes progress tracking records for each write pair.
 *
 * @param pairs ISO-to-device pairs scheduled for writing.
 * @return std::shared_ptr<std::vector<ProgressInfo>> Shared progress vector passed to worker execution.
 */
static std::shared_ptr<std::vector<ProgressInfo>> buildProgressData(
    const std::vector<std::pair<IsoInfo, std::string>>& pairs) {
    auto progressDataPtr = std::make_shared<std::vector<ProgressInfo>>();
    progressDataPtr->reserve(pairs.size());
    for (const auto& [iso, device] : pairs) {
        progressDataPtr->emplace_back(iso.filename, device, iso.sizeStr, iso.path);
    }
    return progressDataPtr;
}

/**
 * @brief Verifies that requested write jobs fit within the thread pool capacity.
 *
 * Prints the formatted `write2usb` selection limit error to `std::cerr` if
 * `jobCount` exceeds `pool.threadCount()`.
 *
 * @param wt Active `write2usb` UI theme used for diagnostic formatting.
 * @param jobCount Number of write pairs to execute concurrently.
 * @param pool Worker thread pool whose capacity limits parallel jobs.
 * @return bool `true` if `jobCount` is within `pool.threadCount()`; `false` otherwise.
 */
static bool checkThreadPoolLimit(const WriteTheme& wt,
                                 size_t jobCount,
                                 const ThreadPool& pool) {
    if (jobCount <= pool.threadCount()) {
        return true;
    }

    std::cerr << "\n" << wt.colorFailure << "Error: "
              << UI::Palette::Yellow << "write2usb"
              << wt.colorFailure << " selection limit is "
              << wt.colorWarning << pool.threadCount()
              << wt.colorFailure << " (thread pool size)."
              << wt.rl_resetCol << "\n\n";
    return false;
}

/**
 * @brief Counts how many write jobs reported completion and returns the process exit code.
 *
 * @param progressData Progress records updated by worker threads.
 * @return int `0` if every job completed and operation was not cancelled; `1` otherwise.
 */
static int evaluateWriteResult(const std::vector<ProgressInfo>& progressData) {
    size_t ok = 0;
    for (const auto& progress : progressData) {
        if (progress.completed.load()) {
            ++ok;
        }
    }

    return (ok == progressData.size() &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

/**
 * @brief Non-interactive ISO -> USB writer orchestration function.
 *
 * Arguments starting with `/dev/` are treated as target devices; all others are treated as ISOs.
 * A single ISO can be written to multiple devices, or ISO and device counts must match
 * and are paired in argument order.
 *
 * Every validation error is collected and reported (using the write2usb UI theme) before
 * any hardware is touched. Per device, this includes checks in @ref WriteValidation::checkDevice
 * (such as rejecting devices still flushing data) and capacity checks in @ref WriteValidation::checkCapacity.
 *
 * @param args Parsed command-line arguments containing target devices, ISO paths, and execution flags.
 * @return int Returns `0` if all write operations completed successfully; `1` on validation failure,
 *             write failure, thread pool capacity overflow, or user cancellation.
 */
int handleWriteUsb(const ParsedArgs& args) {
    using namespace WriteValidation;
    const WriteTheme wt = getWriteTheme();

    std::vector<std::string> isoArgs, devArgs;
    if (!validatePrerequisitesAndArgs(args, wt, isoArgs, devArgs)) {
        return 1;
    }

    std::vector<std::string> errors;
    std::vector<IsoInfo> isos = validateIsos(isoArgs, wt, errors);

    std::vector<std::string> devices;
    std::vector<uint64_t> deviceSizes;
    validateDevices(devArgs, wt, errors, devices, deviceSizes);

    if (errors.empty()) {
        collectCapacityErrors(wt, isos, devices, deviceSizes, errors);
    }

    if (!errors.empty()) {
        printValidationErrors(wt, errors);
        return 1;
    }

    ThreadPool& pool = getStaticThreadPool();
    auto pairsPtr = std::make_shared<std::vector<std::pair<IsoInfo, std::string>>>(
        buildWritePairs(isos, devices));

    if (!checkThreadPoolLimit(wt, pairsPtr->size(), pool)) {
        return 1;
    }

    auto progressDataPtr = buildProgressData(*pairsPtr);

    executeWriteOperations(args.silentMode, pairsPtr, progressDataPtr, pool);

    g_drainingManager.joinAll();

    const int exitCode = evaluateWriteResult(*progressDataPtr);

    if (!args.silentMode) {
        std::cout << "\n";
    }

    return exitCode;
}
