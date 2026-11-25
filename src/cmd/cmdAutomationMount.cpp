// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <ctype.h>
#include <unistd.h>

import std;
import isocmd.cmd.cmdAutomation;
import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.inputHandling;
import isocmd.ops.mount;
import isocmd.proc.process;
import isocmd.threadpool.threadpool;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

namespace fs = std::filesystem;

using namespace IsoCli;

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
        GlobalThreadCaps::MOUNT_THREAD_CAP,
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
 * respecting GlobalThreadCaps::MOUNT_THREAD_CAP and pool bounds.
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

        barThread = launchProgressBar(
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
