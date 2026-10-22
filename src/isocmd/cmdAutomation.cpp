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
#include "../concurrency.h"
#include "../inputHandling.h"
#include "../mount.h"
#include "../state.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../umount.h"
#include "../verbose.h"

namespace fs = std::filesystem;

// --- Function Declarations ---

void convertToISO(const std::vector<std::string>& imageFiles,
                  const bool& modeMdf,
                  const bool& modeNrg,
                  const bool& modeChd,
                  const bool& modeDaa,
                  std::atomic<size_t>* completedBytes,
                  std::atomic<size_t>* completedTasks,
                  std::atomic<size_t>* failedTasks,
                  std::vector<std::string>* successfulOutputPaths,
                  std::mutex* outPathsMutex);

size_t calculateTotalBytesForConversions(const std::vector<std::string>& filesToProcess,
                                         const bool modeMdf, const bool modeNrg,
                                         const bool modeChd, const bool modeDaa);

void displayProgressBarWithSize(std::atomic<size_t>* completedBytes,
                                size_t totalBytes,
                                std::atomic<size_t>* completedTasks,
                                std::atomic<size_t>* failedTasks,
                                size_t totalTasks,
                                std::atomic<bool>* isComplete,
                                bool* verbose, const std::string& operation);

// ─── Palette helpers ─────────────────────────────────────────────────────────

/**
 * @brief Print a bold/reset-coloured informational line to stdout.
 *        Applies BoldReset prefix and appends a newline; gated by silentMode.
 */
static void verboseInfo(bool silentMode, const std::string& msg) {
    if (silentMode) return;
    std::cout << UI::Palette::BoldReset << msg << "\n";
}

/**
 * @brief Print a yellow warning notice to stdout and append a blank line.
 *        Gated by silentMode.
 */
static void verboseWarn(bool silentMode, const std::string& msg) {
    if (silentMode) return;
    std::cout << UI::Palette::Yellow << msg << UI::Palette::Reset << "\n\n";
}

/**
 * @brief Emit a coloured warning to stderr, gated by silentMode.
 *
 * Formats as:  Warning: '<red>path</red>' reason
 * Pass an empty @p path to omit the quoted-path segment entirely.
 */
static void warnMsg(bool silentMode,
                    std::string_view reason,
                    std::string_view path = "") {
    if (silentMode) return;
    std::cerr << UI::Palette::Yellow << "Warning: ";
    if (!path.empty())
        std::cerr << "'" << UI::Palette::Red << path
                  << UI::Palette::Yellow << "' ";
    std::cerr << reason << UI::Palette::Reset << "\n";
}

/**
 * @brief Emit a coloured error to stderr.
 *        Always shown regardless of silentMode.
 */
static void errMsg(std::string_view msg) {
    std::cerr << UI::Palette::Red << "Error: " << msg
              << UI::Palette::Reset << "\n\n";
}

// ─── ISO file discovery ──────────────────────────────────────────────────────

/**
 * @brief Recursively scan @p dir for ISO files up to @p maxDepth levels deep.
 *
 * Results are inserted into @p isoFiles (canonical paths).
 * Symlinks are skipped.  Filesystem errors emit a warning but do not abort.
 *
 * @param dir          Directory to scan.
 * @param maxDepth     Maximum descent depth; -1 = unlimited, 0 = surface only.
 * @param currentDepth Current recursion depth (caller passes 0).
 * @param isoFiles     Accumulator for discovered ISO paths.
 * @param hasErrors    Set to true if any non-fatal error is encountered.
 * @param silentMode   Suppress per-entry warnings when true.
 */
static void scanDirectoryForISOs(const fs::path& dir,
                                 int maxDepth,
                                 int currentDepth,
                                 std::unordered_set<std::string>& isoFiles,
                                 bool& hasErrors,
                                 bool silentMode) {
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (GlobalState::g_operationCancelled.load()) return;
            if (entry.is_symlink()) continue;

            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".iso")
                    isoFiles.insert(fs::canonical(entry.path()).string());
            } else if (entry.is_directory() &&
                       (maxDepth == -1 || currentDepth < maxDepth)) {
                scanDirectoryForISOs(entry.path(), maxDepth,
                                     currentDepth + 1,
                                     isoFiles, hasErrors, silentMode);
            }
        }
    } catch (const fs::filesystem_error& e) {
        warnMsg(silentMode,
                std::string("Error scanning directory: ") + e.what(),
                dir.string());
        hasErrors = true;
    }
}

// ─── Image file discovery (for -c) ───────────────────────────────────────────

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

// ─── Argument parsing ────────────────────────────────────────────────────────

struct ParsedArgs {
    std::string              action;      ///< Last positional arg: "mount" / "umount" / … ("convert" with -c)
    std::vector<std::string> paths;
    bool                     silentMode = false;
    bool                     convert    = false;   ///< -c: non-interactive conversion
    int                      maxDepth   = -1;
};

/**
 * @brief Parse argv into a ParsedArgs structure.
 *
 * With -c, every positional argument is a path and the action is "convert".
 * Otherwise the last positional is the action (mount/umount/unmount).
 *
 * @return false when no positional arguments were found (caller should abort).
 */
static bool parseArgs(int argc, char* argv[], ParsedArgs& out) {
    bool captureWarn = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--silent") {
            out.silentMode = true;
        } else if (arg == "-c") {
            out.convert = true;
        } else if (arg.substr(0, 2) == "-d") {
            try {
                out.maxDepth = std::stoi(arg.substr(2));
                if (out.maxDepth < 0) {
                    std::cerr << UI::Palette::Yellow
                              << "Warning: Negative depth (" << out.maxDepth
                              << ") means a full recursive scan."
                              << UI::Palette::Reset << "\n";
                    out.maxDepth = -1;
                    captureWarn = true;
                }
            } catch (...) {
                std::cerr << UI::Palette::Yellow
                          << "Warning: Invalid depth '" << arg.substr(2)
                          << "'. Using 0 (surface scan)."
                          << UI::Palette::Reset << "\n";
                out.maxDepth = 0;
                captureWarn = true;
            }
        } else {
            out.paths.push_back(arg);
        }
    }

    if (captureWarn) {
        std::cout << UI::Palette::Reset << "\n";
        captureWarn = false;
    }

    if (out.convert) {            // -c: every positional is a path
        out.action = "convert";
        return true;
    }

    if (out.paths.empty()) return false;
    out.action = out.paths.back();
    out.paths.pop_back();
    return true;
}

// ─── Mount branch ────────────────────────────────────────────────────────────

/**
 * @brief Mounts ISO files supplied as command-line arguments.
 *
 * Validates the provided paths, recursively scans directories according
 * to the configured maximum depth, and collects unique ISO files.
 * Mounting is performed concurrently using the static thread pool,
 * respecting GlobalConcurrency::MOUNT_THREAD_CAP and the pool size.
 * Files are distributed across worker threads in separate batches.
 *
 * Displays the operation summary unless silent mode is enabled.
 *
 * @param args Parsed command-line arguments, including input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if at least one ISO was mounted successfully;
 *         1 if mounting failed to produce a successful result or
 *         an earlier validation error occurred only insofar as
 *         reflected by the existing return logic.
 */
static int handleMount(const ParsedArgs& args) {
    if (geteuid() != 0) {
        errMsg("Root privileges required for mounting ISOs.");
        return 1;
    }

    bool noScanDir = true;

    std::unordered_set<std::string> isoFiles;
    bool hasErrors = false;

    for (const auto& rawPath : args.paths) {
        if (GlobalState::g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return 1;
        }

        fs::path path(rawPath);

        try {
            if (!fs::exists(path)) {
                warnMsg(args.silentMode, "does not exist, skipping.", rawPath);
                hasErrors = true;
                continue;
            }

            if (fs::is_regular_file(path)) {
                std::string ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext != ".iso") {
                    warnMsg(args.silentMode,
                            "is not an ISO file, skipping.", rawPath);
                    hasErrors = true;
                    continue;
                }

                isoFiles.insert(fs::canonical(path).string());

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

                scanDirectoryForISOs(path, args.maxDepth, 0,
                                     isoFiles, hasErrors, args.silentMode);
                noScanDir = false;

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

    if (GlobalState::g_operationCancelled.load())
        verboseWarn(args.silentMode, "Mount operation cancelled by user.");

    if (isoFiles.empty()) {
        if (!GlobalState::g_operationCancelled.load())
            verboseWarn(args.silentMode, noScanDir
                ? "\nNo ISO files to mount."
                : "\nNo ISO files found to mount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("\nLocated ")
                .append(std::to_string(isoFiles.size()))
                .append(" ISO file")
                .append(isoFiles.size() == 1 ? "" : "s")
                .append("; Attempting to mount...\n"));

    std::atomic<size_t> completedTasks{0}, failedTasks{0};

    std::vector<std::string> files(isoFiles.begin(), isoFiles.end());

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        files.size(),
        GlobalConcurrency::MOUNT_THREAD_CAP,
        poolSize
    });

    // Distribute files evenly across a bounded number of workers.
    std::vector<std::vector<std::string>> chunks(numThreads);
    for (size_t i = 0; i < files.size(); ++i)
        chunks[i % numThreads].push_back(std::move(files[i]));

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (auto& chunk : chunks) {
        futures.emplace_back(pool.enqueue(
            [chunk = std::move(chunk),
             &completedTasks, &failedTasks,
             silentMode = args.silentMode]() {
                mountIsoFiles(chunk, &completedTasks, &failedTasks,
                              silentMode);
            }));
    }

    for (auto& future : futures)
        future.wait();

    if (!args.silentMode) {
        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationSkipped)
            std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << "\n";

        std::cout << "\nMount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (completedTasks.load() > 0) ? 0 : 1;
}

// ─── Umount branch ───────────────────────────────────────────────────────────

/**
 * @brief Unmounts ISO mount points supplied as command-line arguments.
 *
 * Collects mount points from the specified paths or scans /mnt/ISOs
 * when no paths are provided or the "all" argument is specified.
 * Validates mount-point locations and rejects paths outside the
 * permitted /mnt/ISOs hierarchy.
 *
 * Unmounting is performed concurrently using the static thread pool,
 * respecting GlobalConcurrency::UMOUNT_THREAD_CAP and the pool size.
 * Mount points are distributed across worker threads in separate batches.
 *
 * Attempts to remove the parent /mnt/ISOs directory after all workers
 * finish and displays the operation summary unless silent mode is enabled.
 *
 * @param args Parsed command-line arguments, including input paths
 *             and silent-mode settings.
 *
 * @return 0 if no unmount failures or path-validation errors occurred;
 *         1 if an unmount failed or a path-validation error occurred.
 */
static int handleUmount(const ParsedArgs& args) {
    if (geteuid() != 0) {
        errMsg("Root privileges required for unmounting ISOs.");
        return 1;
    }

    bool scanDir = false;

    std::unordered_set<std::string> mountPoints;
    bool hasErrors = false;

    auto collectFromMnt = [&](const fs::path& mntPath) {
        disableInput();

        verboseInfo(args.silentMode,
                    std::string("Scanning ")
                    .append(mntPath.string())
                    .append(" for ISO mount points (surface scan)..."));

        try {
            for (const auto& entry : fs::directory_iterator(mntPath)) {
                if (GlobalState::g_operationCancelled.load())
                    return;

                if (entry.is_directory())
                    mountPoints.insert(fs::canonical(entry.path()).string());
            }
        } catch (const fs::filesystem_error&) {
            hasErrors = true;
        }

        scanDir = true;
    };

    const bool scanAllMnt = args.paths.empty() ||
        (args.paths.size() == 1 && args.paths[0] == "all");

    if (scanAllMnt) {
        collectFromMnt("/mnt/ISOs");

        if (GlobalState::g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return 1;
        }
    } else {
        for (const auto& rawPath : args.paths) {
            if (GlobalState::g_operationCancelled.load()) {
                verboseWarn(args.silentMode, "Operation cancelled by user.");
                return 1;
            }

            fs::path path(rawPath);

            try {
                if (fs::exists(path) && fs::is_directory(path)) {
                    const std::string canonical = fs::canonical(path).string();

                    if (canonical == "/mnt/ISOs") {
                        collectFromMnt(path);
                    } else if (canonical.rfind("/mnt/ISOs/", 0) == 0) {
                        mountPoints.insert(canonical);
                    } else {
                        warnMsg(args.silentMode,
                                "is not allowed. Only /mnt/ISOs or /mnt/ISOs/isoFolder are valid.",
                                rawPath);
                        hasErrors = true;
                    }
                } else {
                    fs::path candidate = path.is_relative()
                        ? fs::path("/mnt/ISOs") / path.filename()
                        : path;

                    if (fs::exists(candidate) && fs::is_directory(candidate)) {
                        mountPoints.insert(fs::canonical(candidate).string());
                    } else {
                        warnMsg(args.silentMode,
                                "does not exist or is not a valid mount point, skipping.",
                                rawPath);
                        hasErrors = true;
                    }
                }
            } catch (const fs::filesystem_error& e) {
                warnMsg(args.silentMode,
                        std::string("Error processing path: ") + e.what(), rawPath);
                hasErrors = true;
            }
        }
    }

    if (GlobalState::g_operationCancelled.load())
        verboseWarn(args.silentMode, "Umount operation cancelled by user.");

    if (mountPoints.empty()) {
        if (!GlobalState::g_operationCancelled.load())
            verboseWarn(args.silentMode,
                        "\nNo ISO mount points found to unmount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string(scanDir ? "\nLocated " : "Located ")
                .append(std::to_string(mountPoints.size()))
                .append(" mount point")
                .append(mountPoints.size() == 1 ? "" : "s")
                .append("; Attempting to unmount...\n"));

    std::atomic<size_t> completedTasks{0}, failedTasks{0};

    std::vector<std::string> paths(mountPoints.begin(), mountPoints.end());

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        paths.size(),
        GlobalConcurrency::UMOUNT_THREAD_CAP,
        poolSize
    });

    // Distribute mount points evenly across a bounded number of workers.
    std::vector<std::vector<std::string>> chunks(numThreads);
    for (size_t i = 0; i < paths.size(); ++i)
        chunks[i % numThreads].push_back(std::move(paths[i]));

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (auto& chunk : chunks) {
        futures.emplace_back(pool.enqueue(
            [chunk = std::move(chunk),
             &completedTasks, &failedTasks,
             silentMode = args.silentMode]() {
                unmountISO(chunk, &completedTasks, &failedTasks,
                           silentMode);
            }));
    }

    for (auto& future : futures)
        future.wait();

    // Clean up the parent directory after all unmount workers finish.
    {
        std::error_code ec;
        fs::remove("/mnt/ISOs", ec);
    }

    if (!args.silentMode) {
        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << "\n";

        std::cout << "\nUnmount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (failedTasks.load() == 0 && !hasErrors) ? 0 : 1;
}

// ─── Convert branch (-c) ─────────────────────────────────────────────────────

/**
 * @brief Converts supported disk images to ISO files.
 *
 * Validates the supplied file and directory paths, recursively scans
 * directories according to the configured maximum depth, and collects
 * unique supported images (BIN, IMG, CCD, MDF, NRG, CHD, DAA, and GBI).
 * Files are grouped by conversion backend and sorted for deterministic
 * task ordering.
 *
 * Calculates the estimated total output size and tracks byte-level
 * and task-level progress. Conversions are executed concurrently using
 * the static thread pool, respecting GlobalConcurrency::CONV_THREAD_CAP
 * and the pool size. Workers dynamically claim tasks from a shared
 * atomic index and dispatch each file to its appropriate backend.
 *
 * Tracks successful output paths and displays detailed operation
 * results when requested through the progress interface.
 *
 * @param args Parsed command-line arguments, including input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if all conversions completed or were skipped without errors;
 *         1 if any conversion failed, path validation failed, or the
 *         operation was cancelled.
 */
static int handleConvert(const ParsedArgs& args) {
    if (args.paths.empty()) {
        verboseWarn(args.silentMode, "\nNo files to convert2iso.");
        return 1;
    }

    std::unordered_set<std::string> images;
    bool hasErrors = false;

    for (const auto& rawPath : args.paths) {
        if (GlobalState::g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return 1;
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
                if (args.silentMode)
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

    if (GlobalState::g_operationCancelled.load())
        verboseWarn(args.silentMode, "Conversion cancelled by user.");

    if (images.empty()) {
        if (!GlobalState::g_operationCancelled.load())
            verboseWarn(args.silentMode,
                        "\nNo convertible image files found.");
        return hasErrors ? 1 : 0;
    }

    // Bucket by backend and sort for deterministic processing order.
    std::vector<std::string> binImg, mdf, nrg, chd, daa;

    for (const auto& f : images) {
        switch (classifyImage(f)) {
            case ImgKind::BinImg: binImg.push_back(f); break;
            case ImgKind::Mdf:    mdf.push_back(f);    break;
            case ImgKind::Nrg:    nrg.push_back(f);    break;
            case ImgKind::Chd:    chd.push_back(f);    break;
            case ImgKind::Daa:    daa.push_back(f);    break;
            default: break;
        }
    }

    for (auto* v : {&binImg, &mdf, &nrg, &chd, &daa})
        std::sort(v->begin(), v->end());

    verboseInfo(args.silentMode,
                std::string("\nLocated ")
                .append(std::to_string(images.size()))
                .append(" image file")
                .append(images.size() == 1 ? "" : "s")
                .append("; Attempting to convert...\n"));

    std::atomic<size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
    std::atomic<bool> isComplete{false};

    std::vector<std::string> successfulOutputs;
    std::mutex outMutex;
    bool showVerbose = false;

    // Calculate the total estimated output bytes for the progress bar.
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

    if (totalBytes == 0)
        totalBytes = 1;

    // Start the progress bar unless output is silent.
    std::thread barThread;

    if (!args.silentMode) {
        clearScrollBuffer();

        std::cout << "\n" << color << " Processing "
                  << (images.size() == 1 ? "task for " : "tasks for ")
                  << UI::Palette::Orange << "convert2iso" << color << "... ("
                  << UI::Palette::Red << "Ctrl+c"
                  << color << ":cancel)\n";

        barThread = std::thread(
            displayProgressBarWithSize,
            &completedBytes, totalBytes,
            &completedTasks, &failedTasks, images.size(),
            &isComplete, &showVerbose,
            std::string("convert2iso"));
    }

    // A task records its input path and the backend that must process it.
    struct ConversionTask {
        std::string path;
        ImgKind kind;
    };

    std::vector<ConversionTask> tasks;
    tasks.reserve(images.size());

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

    // Bound worker count by task count, configured cap, and pool size.
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

                    const bool isMdf = task.kind == ImgKind::Mdf;
                    const bool isNrg = task.kind == ImgKind::Nrg;
                    const bool isChd = task.kind == ImgKind::Chd;
                    const bool isDaa = task.kind == ImgKind::Daa;

                    convertToISO(
                        {task.path},
                        isMdf, isNrg, isChd, isDaa,
                        &completedBytes,
                        &completedTasks,
                        &failedTasks,
                        &successfulOutputs,
                        &outMutex);
                }
            }));
    }

    for (auto& future : futures)
        future.wait();

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // Show detailed output only if requested through the progress prompt.
    if (!args.silentMode && showVerbose) {
        clearScrollBuffer();
        std::cout << "\n";

        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        for (const auto& msg : verboseSets.operationSkipped)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        std::cout << "\nConversion summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    // Non-zero on any failure so scripts can detect unsuccessful operations.
    return (failedTasks.load() == 0 &&
            !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}


// ─── Entry point ─────────────────────────────────────────────────────────────

/**
 * @brief Primary entry point for handling mount/umount/convert CLI commands.
 *
 * Parses arguments via parseArgs(), then dispatches to handleMount(),
 * handleUmount() or (with -c) handleConvert().
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return int 0 on success, 1 on error.
 */
int handleMountUmountCommands(int argc, char* argv[]) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    if (argc < 2) {
        errMsg("No arguments provided.");
        return 1;
    }

    ParsedArgs args;
    if (!parseArgs(argc, argv, args) || args.action.empty()) {
        errMsg("No action provided.");
        return 1;
    }

    if (args.convert)
        return handleConvert(args);

    if (args.action == "mount")
        return handleMount(args);

    if (args.action == "umount" || args.action == "unmount")
        return handleUmount(args);

    errMsg(std::string("Unknown action '") + args.action + "'");
    return 1;
}
