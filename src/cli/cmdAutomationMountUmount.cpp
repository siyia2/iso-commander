
// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <ctype.h>
#include <unistd.h>

// Project Headers
#include "isocmd/cli/cmdAutomation.h"
#include "isocmd/core/concurrency.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/ops/mount.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/core/state.h"
#include "isocmd/ui/themes.h"
#include "isocmd/core/threadpool.h"
#include "isocmd/ops/umount.h"
#include "isocmd/ui/verbose.h"
#include "isocmd/usb/write2usbUI.h"

namespace fs = std::filesystem;

using namespace IsoCli;

void displayProgressBarWithSize(std::atomic<size_t>* completedBytes,
                                size_t totalBytes,
                                std::atomic<size_t>* completedTasks,
                                std::atomic<size_t>* failedTasks,
                                size_t totalTasks,
                                std::atomic<bool>* isComplete,
                                bool* verbose, const std::string& operation,
                                const std::string& header);

// ─── Palette helpers ─────────────────────────────────────────────────────────

/**
 * @brief Print a bold/reset-coloured informational line to stdout.
 *        Applies BoldReset prefix and appends a newline; gated by silentMode.
 */
void IsoCli::verboseInfo(bool silentMode, const std::string& msg) {
    if (silentMode) return;
    std::cout << UI::Palette::BoldReset << "\n" << msg;
}

/**
 * @brief Print a yellow warning notice to stdout and append a blank line.
 *        Gated by silentMode.
 */
void IsoCli::verboseWarn(bool silentMode, const std::string& msg) {
    const PrintListTheme c = getListColors();
    if (silentMode) return;
    std::cout << c.num << "\n\n" << msg << UI::Palette::Reset << "\n\n";
}

/**
 * @brief Emit a coloured warning to stderr, gated by silentMode.
 *
 * Formats as: Warning: '<red>path</red>' reason
 * Pass an empty @p path to omit the quoted-path segment entirely.
 */
void IsoCli::warnMsg(bool silentMode,
                     std::string_view reason,
                     std::string_view path) {
    const PrintListTheme c = getListColors();
    if (silentMode) return;
    std::cerr << c.num << "\n" << "Warning: ";
    if (!path.empty())
        std::cerr << "'" << c.indexA << path
                  << c.num << "' ";
    std::cerr << reason << UI::Palette::Reset;
}

/**
 * @brief Emit a coloured error to stderr.
 *        Always shown regardless of silentMode.
 */
void IsoCli::errMsg(std::string_view msg) {
    const PrintListTheme c = getListColors();
    std::cerr << c.indexA << "\n" << "Error: " << msg
              << UI::Palette::Reset << "\n\n";
}

// ─── ISO file discovery ──────────────────────────────────────────────────────

/**
 * @brief Recursively scan @p dir for ISO files up to @p maxDepth levels deep.
 *
 * Results are inserted into @p isoFiles (canonical paths).
 * Symlinks are skipped. Filesystem errors emit a warning but do not abort.
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

// ─── Mount branch ───────────────────────────────────────────────────────────

/**
 * @brief Verifies that the current process possesses root privileges.
 *
 * @return true if the effective user ID is 0 (root); false otherwise.
 */
static bool checkRootPrivileges() {
    if (geteuid() != 0) {
        errMsg("Root privileges required for mounting ISOs.");
        return false;
    }
    return true;
}

/**
 * @brief Validates input paths and recursively scans directories for unique ISO files.
 *
 * Iterates through the input paths, verifying existence, regular file types, and valid
 * ".iso" extensions, or recursively scanning directories according to the maximum depth constraint.
 *
 * @param args Parsed command-line arguments containing input paths and depth configuration.
 * @param isoFiles Set storing unique, canonical paths of successfully discovered ISO files.
 * @param hasErrors Reference flag set to true if any path validation or file error occurs.
 * @param noScanDir Reference flag tracking whether a directory scan was initiated.
 * @return true if the operation was cancelled by the user during scanning; false otherwise.
 */
static bool collectIsosAndValidatePaths(const ParsedArgs& args,
                                       std::unordered_set<std::string>& isoFiles,
                                       bool& hasErrors,
                                       bool& noScanDir) {
    for (const auto& rawPath : args.paths) {
        if (GlobalState::g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return true;
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
    return false;
}

/**
 * @brief Dispatches and executes ISO mount tasks concurrently across the static thread pool.
 *
 * Workers dynamically claim tasks from a shared atomic index to mount ISO files
 * up to the configured concurrency cap and pool size limits.
 *
 * @param files Sorted list of canonical ISO file paths to process.
 * @param silentMode Whether silent output mode is enabled.
 * @param completedBytes Atomic counter tracking task progress for the progress bar.
 * @param completedTasks Atomic counter tracking successful mount operations.
 * @param failedTasks Atomic counter tracking failed mount operations.
 * @return The final value of the atomic task index upon thread completion.
 */
static size_t runMountThreadPool(const std::vector<std::string>& files,
                                 bool silentMode,
                                 std::atomic<size_t>& completedBytes,
                                 std::atomic<size_t>& completedTasks,
                                 std::atomic<size_t>& failedTasks) {
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        files.size(),
        GlobalConcurrency::MOUNT_THREAD_CAP,
        poolSize
    });

    std::atomic<size_t> nextTask{0};
    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (size_t t = 0; t < numThreads; ++t) {
        futures.emplace_back(pool.enqueue(
            [&files, &nextTask,
             &completedBytes, &completedTasks, &failedTasks,
             silentMode]() {
                while (!GlobalState::g_operationCancelled.load()) {
                    const size_t i = nextTask.fetch_add(1);

                    if (i >= files.size())
                        break;

                    mountIsoFiles(std::vector<std::string>{files[i]},
                                  &completedTasks, &failedTasks,
                                  silentMode);

                    completedBytes.fetch_add(1);
                }
            }));
    }

    for (auto& future : futures)
        future.wait();

    return nextTask.load();
}

/**
 * @brief Processes any remaining unclaimed ISO files if the operation was cancelled mid-execution.
 *
 * Ensures cleanup handling for leftover tasks after the main worker pool and progress bar terminate.
 *
 * @param files Sorted list of all target ISO file paths.
 * @param nextTaskVal The task index up to which workers successfully claimed tasks.
 * @param completedTasks Atomic counter tracking successful mount operations.
 * @param failedTasks Atomic counter tracking failed mount operations.
 * @param silentMode Whether silent output mode is enabled.
 */
static void handleCancelledLeftovers(const std::vector<std::string>& files,
                                     size_t nextTaskVal,
                                     std::atomic<size_t>& completedTasks,
                                     std::atomic<size_t>& failedTasks,
                                     bool silentMode) {
    if (GlobalState::g_operationCancelled.load()) {
        const size_t first = std::min(nextTaskVal, files.size());
        if (first < files.size())
            mountIsoFiles(
                std::vector<std::string>(files.begin() + first, files.end()),
                &completedTasks, &failedTasks, silentMode);
    }
}

/**
 * @brief Displays detailed operation logs and the final execution summary.
 *
 * Outputs completed, skipped, and failed logs alongside the final success/failure counts
 * if requested via the progress interface.
 *
 * @param silentMode Whether output is silent.
 * @param showVerbose Whether detailed logs were requested.
 * @param completedTasks Atomic counter tracking successful mount operations.
 * @param failedTasks Atomic counter tracking failed mount operations.
 */
static void showMountSummary(bool silentMode, bool showVerbose,
                             std::atomic<size_t>& completedTasks,
                             std::atomic<size_t>& failedTasks) {
    if (!silentMode && showVerbose) {
        SyncOutput sync;
        clearScrollBuffer();
        std::cout << "\n";

        auto printLine = [](std::string_view msg) {
            std::cout << msg << UI::Palette::BoldReset << "\n";
        };
        for (const auto& msg : verboseSets.operationCompleted) printLine(msg);
        forEachMountLine(MountRecKind::Completed, printLine);
        for (const auto& msg : verboseSets.operationSkipped) printLine(msg);
        forEachMountLine(MountRecKind::Skipped, printLine);
        for (const auto& msg : verboseSets.operationFailed) printLine(msg);
        forEachMountLine(MountRecKind::Failed, printLine);

        std::cout << "\nMount summary:\n"
                  << " Successful: " << completedTasks.load() << "\n"
                  << " Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }
}

/**
 * @brief Mounts ISO files supplied as command-line arguments.
 *
 * Validates provided paths, recursively scans directories according
 * to configured maximum depth limits, and collects unique ISO files.
 * Files are sorted for deterministic task ordering.
 *
 * Mounting is performed concurrently using the static thread pool,
 * respecting GlobalConcurrency::MOUNT_THREAD_CAP and pool bounds.
 * Workers dynamically claim tasks from a shared atomic index and mount
 * one ISO at a time.
 *
 * Progress is shown through the progress-bar interface unless silent
 * mode is enabled. Mount operations have no byte-level progress, so the
 * bar is task-based: both "total" and "completed" counters are expressed
 * in number of ISO files. Detailed results and the operation summary
 * are displayed only when requested through the progress prompt.
 *
 * @param args Parsed command-line arguments, including input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if every ISO mounted successfully (or there was nothing to
 *         mount and no path errors occurred);
 *         1 if any mount failed, a path-validation error occurred,
 *         root privileges were missing, or the operation was cancelled.
 */
int handleMount(const ParsedArgs& args) {
    if (!checkRootPrivileges()) {
        return 1;
    }

    bool noScanDir = true;
    std::unordered_set<std::string> isoFiles;
    bool hasErrors = false;

    if (collectIsosAndValidatePaths(args, isoFiles, hasErrors, noScanDir)) {
        return 1;
    }

    if (GlobalState::g_operationCancelled.load())
        verboseWarn(args.silentMode, "Mount operation cancelled by user.");

    if (isoFiles.empty()) {
        if (GlobalState::g_operationCancelled.load())
            return 1;
        verboseWarn(args.silentMode, noScanDir
            ? "No ISO files to mount."
            : "No ISO files found to mount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("Located ")
                .append(std::to_string(isoFiles.size()))
                .append(" ISO file")
                .append(isoFiles.size() == 1 ? "" : "s")
                .append("; Attempting to mount...\n"));

    std::atomic<size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
    std::atomic<bool> isComplete{false};
    bool showVerbose = false;

    // Sort for deterministic processing order.
    std::vector<std::string> files(isoFiles.begin(), isoFiles.end());
    std::sort(files.begin(), files.end());
    const size_t totalTasks = files.size();

    // Start the progress bar unless output is silent.
    std::thread barThread;
    const std::string coloredProcess =
            std::string(UI::Palette::Green) + "mount" + std::string(UI::Palette::BoldReset);

    if (!args.silentMode) {
        std::ostringstream hdr;
        hdr << "\n" << color << " Processing "
            << (totalTasks == 1 ? "task for " : "tasks for ")
            << UI::Palette::Green << "mount" << color << "... ("
            << UI::Palette::Red << "Ctrl+c"
            << color << ":cancel)\n";
        const std::string header = hdr.str();

        clearScrollBuffer();
        std::cout << header;

        barThread = std::thread(
            displayProgressBarWithSize,
            &completedBytes, totalTasks,
            &completedTasks, &failedTasks, totalTasks,
            &isComplete, &showVerbose,
            coloredProcess, header);
    }

    size_t nextTaskVal = runMountThreadPool(files, args.silentMode, completedBytes, completedTasks, failedTasks);

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    handleCancelledLeftovers(files, nextTaskVal, completedTasks, failedTasks, args.silentMode);

    showMountSummary(args.silentMode, showVerbose, completedTasks, failedTasks);

    return (failedTasks.load() == 0 &&
            !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

// ─── Umount branch ──────────────────────────────────────────────────────────

/**
 * @brief Executes concurrent unmount workers across sorted mount paths.
 */
static void runUnmountWorkers(const std::vector<std::string>& paths,
                              bool silentMode,
                              std::atomic<size_t>& completedBytes,
                              std::atomic<size_t>& completedTasks,
                              std::atomic<size_t>& failedTasks,
                              std::atomic<size_t>& nextTask) {
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        paths.size(),
        GlobalConcurrency::UMOUNT_THREAD_CAP,
        poolSize
    });

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (size_t t = 0; t < numThreads; ++t) {
        futures.emplace_back(pool.enqueue(
            [&paths, &nextTask,
             &completedBytes, &completedTasks, &failedTasks,
             silentMode]() {
                // Stop claiming new items on Ctrl+C so the bar ends at once;
                // unclaimed items are marked as cancelled after the bar.
                while (!GlobalState::g_operationCancelled.load()) {
                    const size_t i = nextTask.fetch_add(1);

                    if (i >= paths.size())
                        break;

                    unmountISO(std::vector<std::string>{paths[i]},
                               &completedTasks, &failedTasks,
                               silentMode);

                    completedBytes.fetch_add(1);
                }
            }));
    }

    for (auto& future : futures)
        future.wait();
}

/**
 * @brief Holds the aggregated results and execution state from collecting and validating ISO mount points.
 */
struct CollectionResult {
    std::unordered_set<std::string> mountPoints;
    bool hasErrors = false;
    bool cancelled = false;
    bool abortImmediately = false;
};

/**
 * @brief Checks if the global operation cancellation flag has been set.
 *
 * @param[in] args Parsed command-line arguments for silent mode configuration.
 * @param[out] res Collection result structure to update cancellation metadata.
 * @param[in] message Custom warning message to display if cancelled.
 * @return true if the operation was cancelled, false otherwise.
 */
static bool checkCancellation(const ParsedArgs& args,
                              CollectionResult& res,
                              const char* message = "Operation cancelled by user.") {
    if (GlobalState::g_operationCancelled.load()) {
        verboseWarn(args.silentMode, message);
        res.cancelled = true;
        res.abortImmediately = true;
        return true;
    }
    return false;
}

/**
 * @brief Performs a surface scan of the specified mount directory to collect ISO mount points.
 *
 * @param[in] mntPath The directory path to scan.
 * @param[in] args Parsed command-line arguments containing silent mode configuration.
 * @param[out] res Collection result structure where discovered mount points and error flags are recorded.
 */
static void collectFromMnt(const fs::path& mntPath,
                           const ParsedArgs& args,
                           CollectionResult& res) {
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
                res.mountPoints.insert(fs::canonical(entry.path()).string());
        }
    } catch (const fs::filesystem_error& e) {
        if (e.code() != std::errc::no_such_file_or_directory)
            res.hasErrors = true;
    }
}

/**
 * @brief Validates and processes a single user-specified input path.
 *
 * @param[in] rawPath The raw path string from command-line arguments.
 * @param[in] args Parsed command-line arguments containing silent mode configuration.
 * @param[out] res Collection result structure to update mount points and error flags.
 */
static void processUserPath(const std::string& rawPath,
                            const ParsedArgs& args,
                            CollectionResult& res) {
    fs::path path(rawPath);
    try {
        if (fs::exists(path) && fs::is_directory(path)) {
            const std::string canonical = fs::canonical(path).string();

            if (canonical == "/mnt/ISOs") {
                collectFromMnt(path, args, res);
            } else if (canonical.rfind("/mnt/ISOs/", 0) == 0) {
                res.mountPoints.insert(canonical);
            } else {
                warnMsg(args.silentMode,
                        "is not allowed. Only /mnt/ISOs or /mnt/ISOs/isoFolder are valid.",
                        rawPath);
                res.hasErrors = true;
            }
        } else {
            fs::path candidate = path.is_relative()
                ? fs::path("/mnt/ISOs") / path.filename()
                : path;

            if (fs::exists(candidate) && fs::is_directory(candidate)) {
                res.mountPoints.insert(fs::canonical(candidate).string());
            } else {
                warnMsg(args.silentMode,
                        "does not exist or is not a valid mount point, skipping.",
                        rawPath);
                res.hasErrors = true;
            }
        }
    } catch (const fs::filesystem_error& e) {
        warnMsg(args.silentMode,
                std::string("Error processing path: ") + e.what(), rawPath);
        res.hasErrors = true;
    }
}

/**
 * @brief Collects, sanitizes, and validates ISO mount points based on command-line arguments or surface scans.
 *
 * @details This function examines the provided `ParsedArgs`:
 * - If no paths or the `"all"` argument is specified, it performs a surface scan of `/mnt/ISOs`.
 * - If specific paths are provided, it validates each path, enforcing that they strictly reside within
 *   the permitted `/mnt/ISOs` directory hierarchy.
 * - It monitors `GlobalState::g_operationCancelled` asynchronously during directory iterations to support
 *   responsive user cancellation.
 *
 * @param[in] args Parsed command-line arguments containing target input paths and silent-mode configurations.
 * @return CollectionResult A structure containing the validated set of mount points, error flags, and cancellation metadata.
 *
 * @throws fs::filesystem_error If underlying filesystem canonicalization or iteration fails unrecoverably.
 */
static CollectionResult collectMountPoints(const ParsedArgs& args) {
    CollectionResult res;

    const bool scanAllMnt = args.paths.empty() ||
        (args.paths.size() == 1 && args.paths[0] == "all");

    if (scanAllMnt) {
        collectFromMnt("/mnt/ISOs", args, res);
        checkCancellation(args, res);
        return res;
    }

    for (const auto& rawPath : args.paths) {
        if (checkCancellation(args, res)) {
            break;
        }
        processUserPath(rawPath, args, res);
    }

    if (GlobalState::g_operationCancelled.load() && !res.cancelled) {
        verboseWarn(args.silentMode, "Umount operation cancelled by user.");
        res.cancelled = true;
    }

    return res;
}

/**
 * @brief Starts the progress reporting thread when output is not silent.
 * @param[in]  totalTasks     Total number of unmount tasks to report on.
 * @param[in]  coloredProcess Formatted process label passed to the progress bar.
 * @param[in]  completedBytes Atomic counter of completed byte progress.
 * @param[in]  completedTasks Atomic counter of completed unmount tasks.
 * @param[in]  failedTasks    Atomic counter of failed unmount tasks.
 * @param[out] isComplete     Atomic signal set when worker execution finishes.
 * @param[out] showVerbose    Flag toggled if verbose summary output is requested.
 * @return std::thread Joinable progress bar thread, or a default-constructed thread in silent mode.
 */
static std::thread startProgressThread(
    size_t totalTasks,
    const std::string& coloredProcess,
    std::atomic<size_t>& completedBytes,
    std::atomic<size_t>& completedTasks,
    std::atomic<size_t>& failedTasks,
    std::atomic<bool>& isComplete,
    bool& showVerbose
) {
    std::ostringstream hdr;
    hdr << "\n" << color << " Processing "
        << (totalTasks == 1 ? "task for " : "tasks for ")
        << UI::Palette::Yellow << "umount" << color << "... ("
        << UI::Palette::Red << "Ctrl+c"
        << color << ":cancel)\n";
    const std::string header = hdr.str();

    clearScrollBuffer();
    std::cout << header;

    return std::thread(
        displayProgressBarWithSize,
        &completedBytes, totalTasks,
        &completedTasks, &failedTasks, totalTasks,
        &isComplete, &showVerbose,
        coloredProcess, header);
}

/**
 * @brief Processes any remaining mount points left unclaimed after a cancellation.
 * @param[in]     paths          Sorted mount point paths across all tasks.
 * @param[in]     nextTask       Atomic index of the next unclaimed task.
 * @param[in,out] completedTasks Atomic counter of completed unmount tasks.
 * @param[in,out] failedTasks    Atomic counter of failed unmount tasks.
 * @param[in]     silentMode     Suppresses non-essential output when true.
 */
static void handleCancelledRemainder(
    const std::vector<std::string>& paths,
    const std::atomic<size_t>& nextTask,
    std::atomic<size_t>& completedTasks,
    std::atomic<size_t>& failedTasks,
    bool silentMode
) {
    const size_t first = std::min(nextTask.load(), paths.size());
    if (first < paths.size()) {
        unmountISO(
            std::vector<std::string>(paths.begin() + first, paths.end()),
            &completedTasks, &failedTasks, silentMode);
    }
}

/**
 * @brief Renders detailed per-item results and final task counts in verbose mode.
 * @param[in] completedTasks Atomic counter of completed unmount tasks.
 * @param[in] failedTasks Atomic counter of failed unmount tasks.
 */
static void printVerboseUnmountSummary(
    const std::atomic<size_t>& completedTasks,
    const std::atomic<size_t>& failedTasks
) {
    SyncOutput sync;
    clearScrollBuffer();
    std::cout << "\n";

    auto printUmountLine = [](std::string_view msg) {
        std::cout << msg << UI::Palette::BoldReset << "\n";
    };
    for (const auto& msg : verboseSets.operationCompleted) printUmountLine(msg);
    forEachUmountLine(true, printUmountLine);

    for (const auto& msg : verboseSets.operationFailed) printUmountLine(msg);
    forEachUmountLine(false, printUmountLine);

    std::cout << "\nUnmount summary:\n"
              << " Successful: " << completedTasks.load() << "\n"
              << " Failed:     " << failedTasks.load()
              << UI::Palette::Reset << "\n\n";
}

/**
 * @brief Computes the exit status from task failure counts, prior errors, and cancellation state.
 * @param[in] failedTasks Number of unmount tasks that failed.
 * @param[in] hasErrors Flag indicating prior path validation errors.
 * @return int Returns `0` when no failures, validation errors, or cancellations occurred; `1` otherwise.
 */
static int computeUnmountExitCode(size_t failedTasks, bool hasErrors) {
    return (failedTasks == 0 && !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

/**
 * @brief Executes concurrent unmounting of collected ISO mount points with real-time progress reporting.
 *
 * @details This function orchestrates the unmount lifecycle:
 * - Sorts the target mount point paths alphabetically for deterministic execution order.
 * - Spawns and manages a dedicated progress bar reporting thread (unless running in silent mode).
 * - Dispatches worker threads via `runUnmountWorkers` to claim tasks from a shared atomic index.
 * - Handles post-cancellation catch-up tasks for any unclaimed mount points.
 * - Performs cleanup of the parent `/mnt/ISOs` directory upon worker completion.
 * - Renders detailed operation success/failure summaries if verbose mode was requested.
 *
 * @param[in] mountPoints Unordered set of canonicalized absolute mount point paths to be unmounted.
 * @param[in] args Parsed command-line arguments controlling verbosity and silent settings.
 * @param[in] hasErrors Flag indicating whether path validation errors were encountered prior to execution.
 * @return int Returns `0` if all tasks succeed without errors or cancellation; returns `1` otherwise.
 */
static int executeUnmounts(const std::unordered_set<std::string>& mountPoints,
                           const ParsedArgs& args,
                           bool hasErrors) {
    if (mountPoints.empty()) {
        if (GlobalState::g_operationCancelled.load())
            return 1;
        verboseWarn(args.silentMode, "No ISO mount points found to unmount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("Located ")
                .append(std::to_string(mountPoints.size()))
                .append(" mount point")
                .append(mountPoints.size() == 1 ? "" : "s")
                .append("; Attempting to unmount...\n"));

    std::atomic<size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
    std::atomic<bool> isComplete{false};
    bool showVerbose = false;

    // Sort for deterministic processing order.
    std::vector<std::string> paths(mountPoints.begin(), mountPoints.end());
    std::sort(paths.begin(), paths.end());
    const size_t totalTasks = paths.size();

    // Start the progress bar unless output is silent.
    std::thread barThread;
    const std::string coloredProcess =
            std::string(UI::Palette::Yellow) + "umount" + std::string(UI::Palette::BoldReset);

    if (!args.silentMode) {
        barThread = startProgressThread(
            totalTasks, coloredProcess,
            completedBytes, completedTasks, failedTasks,
            isComplete, showVerbose);
    }

    std::atomic<size_t> nextTask{0};
    runUnmountWorkers(paths, args.silentMode, completedBytes, completedTasks, failedTasks, nextTask);

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // After a cancel, mark the mount points no worker claimed as cancelled.
    if (GlobalState::g_operationCancelled.load()) {
        handleCancelledRemainder(paths, nextTask, completedTasks, failedTasks, args.silentMode);
    }

    // Clean up the parent directory after all unmount workers finish.
    {
        std::error_code ec;
        fs::remove("/mnt/ISOs", ec);
    }

    // Show detailed output only if requested through the progress prompt.
    if (!args.silentMode && showVerbose) {
        printVerboseUnmountSummary(completedTasks, failedTasks);
    }

    return computeUnmountExitCode(failedTasks.load(), hasErrors);
}

/**
 * @brief Top-level control handler for unmounting ISO mount points supplied via command-line arguments.
 *
 * @details Performs necessary privilege verification (`geteuid() == 0`), delegates path collection
 * and boundary validation to `collectMountPoints()`, and hands off validated targets to `executeUnmounts()`
 * for concurrent processing.
 *
 * @param[in] args Parsed command-line arguments structure containing input paths and execution flags.
 * @return int Exit status code:
 *             - `0` on complete success (including when no targets were found and no errors occurred).
 *             - `1` if root privileges are missing, path validation fails, unmount operations error out,
 *               or the task is cancelled by the user.
 */
int handleUmount(const ParsedArgs& args) {
    if (geteuid() != 0) {
        errMsg("Root privileges required for unmounting ISOs.");
        return 1;
    }

    CollectionResult res = collectMountPoints(args);
    if (res.abortImmediately) {
        return 1;
    }

    return executeUnmounts(res.mountPoints, args, res.hasErrors);
}
