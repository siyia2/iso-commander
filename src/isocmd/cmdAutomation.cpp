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
#include "../write2usbUI.h"

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
                                bool* verbose, const std::string& operation,
                                const std::string& header);

// ─── Palette helpers ─────────────────────────────────────────────────────────

/**
 * @brief Print a bold/reset-coloured informational line to stdout.
 *        Applies BoldReset prefix and appends a newline; gated by silentMode.
 */
static void verboseInfo(bool silentMode, const std::string& msg) {
    if (silentMode) return;
    std::cout << UI::Palette::BoldReset << "\n" << msg;
}

/**
 * @brief Print a yellow warning notice to stdout and append a blank line.
 *        Gated by silentMode.
 */
static void verboseWarn(bool silentMode, const std::string& msg) {
    const PrintListTheme c = getListColors();
    if (silentMode) return;
    std::cout << c.num << "\n\n" << msg << UI::Palette::Reset << "\n\n";
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
static void errMsg(std::string_view msg) {
    const PrintListTheme c = getListColors();
    std::cerr << c.indexA << "\n" << "Error: " << msg
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

// ─── Argument parsing ────────────────────────────────────────────────────────

struct ParsedArgs {
    std::string              action;      ///< Action keyword: "mount" / "umount" / "convert" (c2iso) / "w2usb"
    std::vector<std::string> paths;       ///< Positional arguments that follow the action keyword
    bool                     silentMode = false;
    bool                     convert    = false;   ///< c2iso: non-interactive conversion
    bool                     writeUsb   = false;   ///< w2usb: non-interactive ISO -> USB write
    int                      maxDepth   = -1;
};

/**
 * @brief Parse argv into a ParsedArgs structure.
 *
 * Grammar:
 *
 *     isocmd <action> [flags...] [paths...]
 *
 * The action keyword ("mount", "umount"/"unmount", "c2iso", "w2usb")
 * must be the first argument. Flags (--silent, -dN) may appear only
 * after the action keyword and before the first path. A flag before the
 * action, or a flag after any path, is an error.
 *
 * "umount" and "unmount" both map to action "umount"; "c2iso" sets
 * @c convert and action "convert"; "w2usb" sets @c writeUsb and action
 * "w2usb". Every positional argument following the action keyword is
 * appended to @c paths.
 *
 * "--silent" sets @c silentMode. The "-dN" option sets @c maxDepth
 * for mount and c2iso. Negative depth values trigger
 * a warning and use unlimited depth; invalid depth values produce an
 * error. For w2usb and umount/unmount "-dN" options are ignored without parsing,
 * validation, or diagnostics.
 *
 * For w2usb, ISO paths must precede device paths (/dev/...). Once a
 * device path is encountered, any subsequent non-device path is rejected
 * with a diagnostic.
 *
 * @return false on a missing or misplaced action keyword, an invalid
 *         depth value, a flag before the action, a flag after the first
 *         path, or (w2usb only) an ISO path after a device path.
 *         Returns true on success.
 */
static bool parseArgs(int argc, char* argv[], ParsedArgs& out) {
    bool captureWarn  = false;
    bool actionSeen   = false;
    bool pathsStarted = false;

    auto isActionKeyword = [](const std::string& a) {
        return a == "mount" || a == "umount" || a == "unmount" ||
               a == "c2iso" || a == "w2usb";
    };

    auto isFlag = [](const std::string& a) {
        return a == "--silent" || a.rfind("-d", 0) == 0;
    };

    auto isDevPath = [](std::string_view p) {
        return p.rfind("/dev/", 0) == 0;
    };

    auto applyAction = [&](const std::string& a) {
        if (a == "c2iso") {
            out.convert = true;
            out.action  = "convert";
        } else if (a == "w2usb") {
            out.writeUsb = true;
            out.action   = "w2usb";
        } else if (a == "mount") {
            out.action   = "mount";
        } else { // umount / unmount
            out.action   = "umount";
        }
    };

    auto flushDeferredWarn = [&]() {
        if (captureWarn) {
            std::cout << UI::Palette::Reset << "\n";
            captureWarn = false;
        }
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // 1. The action keyword must come first.
        if (!actionSeen) {
            if (isFlag(arg)) {
                flushDeferredWarn();
                errMsg("Option '" + arg + "' must follow the action keyword.");
                return false;
            }

            if (!isActionKeyword(arg)) {
                flushDeferredWarn();
                errMsg("Unknown action: '" + arg + "'.");
                return false;
            }

            actionSeen = true;
            applyAction(arg);
            continue;
        }

        // 2. Flags must appear before the first path.
        if (isFlag(arg)) {
            if (pathsStarted) {
                flushDeferredWarn();
                errMsg("Option '" + arg + "' must precede all paths.");
                return false;
            }

            if (arg == "--silent") {
                out.silentMode = true;
            } else if (arg.rfind("-d", 0) == 0) {
                // w2usb and umount/unmount ignore -d options.
                if (!out.writeUsb && out.action != "umount") {
                    const std::string depthStr = arg.substr(2);

                    try {
                        size_t parsed = 0;
                        const int depth = std::stoi(depthStr, &parsed);

                        if (parsed != depthStr.size()) {
                            throw std::invalid_argument("invalid depth");
                        }

                        if (depth < 0) {
                            flushDeferredWarn();
                            warnMsg(out.silentMode,
                                    "Negative depth; using unlimited depth.", "");
                            out.maxDepth = -1;
                        } else {
                            out.maxDepth = depth;
                        }
                    } catch (const std::exception&) {
                        flushDeferredWarn();
                        errMsg("Invalid depth option: '" + arg + "'.");
                        return false;
                    }
                }
            }

            continue;
        }

        // 3. Everything else is a path.
        //
        // For w2usb, ISO paths must precede device paths.
        if (out.writeUsb && pathsStarted &&
            !isDevPath(arg) && isDevPath(out.paths.back())) {
            flushDeferredWarn();
            errMsg("w2usb: ISO paths must precede device paths.");
            return false;
        }

        pathsStarted = true;
        out.paths.push_back(arg);
    }

    flushDeferredWarn();

    if (!actionSeen) {
        errMsg("No action provided.");
        return false;
    }
    return true;
}

// ─── Mount branch ───────────────────────────────────────────────────────────

/**
 * @brief Mounts ISO files supplied as command-line arguments.
 *
 * Validates the provided paths, recursively scans directories according
 * to the configured maximum depth, and collects unique ISO files.
 * Files are sorted for deterministic task ordering.
 *
 * Mounting is performed concurrently using the static thread pool,
 * respecting GlobalConcurrency::MOUNT_THREAD_CAP and the pool size.
 * Workers dynamically claim tasks from a shared atomic index and mount
 * one ISO at a time.
 *
 * Progress is shown through the progress-bar interface unless silent
 * mode is enabled. Mount operations have no byte-level progress, so the
 * bar is task-based: the "total" and the "completed" counter are both
 * expressed in number of ISO files. Detailed results and the operation
 * summary are displayed only when requested through the progress prompt.
 *
 * @param args Parsed command-line arguments, including input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if every ISO mounted successfully (or there was nothing to
 *         mount and no path errors occurred);
 *         1 if any mount failed, a path-validation error occurred,
 *         root privileges were missing, or the operation was cancelled.
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

    // completedBytes is task-based here: it advances by one per ISO.
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

    // Bound worker count by task count, configured cap, and pool size.
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        totalTasks,
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
             silentMode = args.silentMode]() {

                // Stop claiming new items on Ctrl+C so the bar ends at once;
                // unclaimed items are marked as cancelled after the bar.
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

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // After a cancel, mark the items no worker claimed as cancelled. This
    // runs after the bar has finished so they don't inflate its progress.
    if (GlobalState::g_operationCancelled.load()) {
        const size_t first = std::min(nextTask.load(), files.size());
        if (first < files.size())
            mountIsoFiles(
                std::vector<std::string>(files.begin() + first, files.end()),
                &completedTasks, &failedTasks, args.silentMode);
    }

    // Show detailed output only if requested through the progress prompt.
    if (!args.silentMode && showVerbose) {
        SyncOutput sync;
        clearScrollBuffer();
        std::cout << "\n";

        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationSkipped)
            std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        std::cout << "\nMount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }   // ?2026l + flush — one atomic frame

    return (failedTasks.load() == 0 &&
            !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

// ─── Umount branch ───────────────────────────────────────────────────────────

/**
 * @brief Unmounts ISO mount points supplied as command-line arguments.
 *
 * Collects mount points from the specified paths or scans /mnt/ISOs
 * when no paths are provided or the "all" argument is specified.
 * Validates mount-point locations and rejects paths outside the
 * permitted /mnt/ISOs hierarchy. Mount points are sorted for
 * deterministic task ordering.
 *
 * Unmounting is performed concurrently using the static thread pool,
 * respecting GlobalConcurrency::UMOUNT_THREAD_CAP and the pool size.
 * Workers dynamically claim tasks from a shared atomic index and
 * unmount one mount point at a time.
 *
 * Progress is shown through the progress-bar interface unless silent
 * mode is enabled. Unmount operations have no byte-level progress, so
 * the bar is task-based: the "total" and the "completed" counter are
 * both expressed in number of mount points. Detailed results and the
 * operation summary are displayed only when requested through the
 * progress prompt.
 *
 * Attempts to remove the parent /mnt/ISOs directory after all workers
 * finish.
 *
 * @param args Parsed command-line arguments, including input paths
 *             and silent-mode settings.
 *
 * @return 0 if no unmount failures or path-validation errors occurred
 *         (including when there was nothing to unmount);
 *         1 if an unmount failed, a path-validation error occurred,
 *         root privileges were missing, or the operation was cancelled.
 */
static int handleUmount(const ParsedArgs& args) {
    if (geteuid() != 0) {
        errMsg("Root privileges required for unmounting ISOs.");
        return 1;
    }

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
        } catch (const fs::filesystem_error& e) {
            if (e.code() != std::errc::no_such_file_or_directory)
                hasErrors = true;
        }
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
        if (GlobalState::g_operationCancelled.load())
            return 1;
        verboseWarn(args.silentMode,
                    "No ISO mount points found to unmount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("Located ")
                .append(std::to_string(mountPoints.size()))
                .append(" mount point")
                .append(mountPoints.size() == 1 ? "" : "s")
                .append("; Attempting to unmount...\n"));

    // completedBytes is task-based here: it advances by one per mount point.
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
        std::ostringstream hdr;
        hdr << "\n" << color << " Processing "
            << (totalTasks == 1 ? "task for " : "tasks for ")
            << UI::Palette::Yellow << "umount" << color << "... ("
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

    // Bound worker count by task count, configured cap, and pool size.
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::min({
        totalTasks,
        GlobalConcurrency::UMOUNT_THREAD_CAP,
        poolSize
    });

    std::atomic<size_t> nextTask{0};

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    for (size_t t = 0; t < numThreads; ++t) {
        futures.emplace_back(pool.enqueue(
            [&paths, &nextTask,
             &completedBytes, &completedTasks, &failedTasks,
             silentMode = args.silentMode]() {

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

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // After a cancel, mark the mount points no worker claimed as cancelled.
    // This runs after the bar has finished so they don't inflate its progress.
    if (GlobalState::g_operationCancelled.load()) {
        const size_t first = std::min(nextTask.load(), paths.size());
        if (first < paths.size())
            unmountISO(
                std::vector<std::string>(paths.begin() + first, paths.end()),
                &completedTasks, &failedTasks, args.silentMode);
    }

    // Clean up the parent directory after all unmount workers finish.
    {
        std::error_code ec;
        fs::remove("/mnt/ISOs", ec);
    }

    // Show detailed output only if requested through the progress prompt.
    if (!args.silentMode && showVerbose) {
        SyncOutput sync;
        clearScrollBuffer();
        std::cout << "\n";

        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << UI::Palette::BoldReset << "\n";

        std::cout << "\nUnmount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }   // ?2026l + flush

    return (failedTasks.load() == 0 && !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

// ─── Convert branch (c2iso) ─────────────────────────────────────────────────────

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
 * @return 0 if all conversions completed without errors (including when
 *         no paths were given or no convertible images were found and no
 *         path errors occurred);
 *         1 if any conversion failed, path validation failed, or the
 *         operation was cancelled.
 */
static int handleConvert(const ParsedArgs& args) {
    if (args.paths.empty()) {
        verboseWarn(args.silentMode, "No files to c2iso.");
        return 0;
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
        if (GlobalState::g_operationCancelled.load())
            return 1;
        verboseWarn(args.silentMode,
                    "No convertible image files found.");
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
    const std::string coloredProcess =
            std::string(UI::Palette::Orange) + "c2iso" + std::string(UI::Palette::BoldReset);

    if (!args.silentMode) {
        std::ostringstream hdr;
        hdr << "\n" << color << " Processing "
            << (images.size() == 1 ? "task for " : "tasks for ")
            << UI::Palette::Orange << "c2iso" << color << "... ("
            << UI::Palette::Red << "Ctrl+c"
            << color << ":cancel)\n";
        const std::string header = hdr.str();

        clearScrollBuffer();
        std::cout << header;

        barThread = std::thread(
            displayProgressBarWithSize,
            &completedBytes, totalBytes,
            &completedTasks, &failedTasks, images.size(),
            &isComplete, &showVerbose,
            coloredProcess, header);
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
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    // Non-zero on any failure so scripts can detect unsuccessful operations.
    return (failedTasks.load() == 0 &&
            !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

// ─── Write-to-USB branch (w2usb) ────────────────────────────────────────────

/**
 * @brief Non-interactive ISO -> USB writer.
 *
 * Arguments starting with /dev/ are target devices; all others are ISOs.
 * One ISO is written to every device; otherwise ISO and device counts must
 * match and are paired in argument order.
 *
 * Every validation error is collected and reported (using the same theme as
 * the interactive write2usb UI) before any device is touched. Per device,
 * this includes the checks in @ref WriteValidation::checkDevice, among them
 * rejecting a device that is still flushing data from an earlier write, and
 * the ISO capacity check in @ref WriteValidation::checkCapacity.
 *
 * With --silent, the live progress UI is skipped.
 *
 * @param args Parsed command-line arguments (target devices, ISO paths and
 *             the silent flag).
 * @return 0 if every write completed; 1 on validation failure, write
 *         failure, or cancellation.
 */
static int handleWriteUsb(const ParsedArgs& args) {
    using namespace WriteValidation;
    const WriteTheme wt = getWriteTheme();

    if (geteuid() != 0) {
        printValidationErrors(wt, {plainErr(wt, "Root privileges are required")});
        return 1;
    }

    std::vector<std::string> isoArgs, devArgs;
    for (const auto& a : args.paths)
        (a.rfind("/dev/", 0) == 0 ? devArgs : isoArgs).push_back(a);

    if (isoArgs.empty()) {
        printValidationErrors(wt, {plainErr(wt, "No ISO files specified")});
        return 1;
    }
    if (devArgs.empty()) {
        printValidationErrors(wt, {plainErr(wt, "No target devices specified")});
        return 1;
    }
    if (isoArgs.size() != 1 && isoArgs.size() != devArgs.size()) {
        printValidationErrors(wt, {plainErr(wt,
            "Got " + std::to_string(isoArgs.size()) + " ISOs and " +
            std::to_string(devArgs.size()) +
            " devices; use 1 ISO for all devices, or one device per ISO")});
        return 1;
    }

    std::vector<std::string> errors;

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

    std::vector<std::string> devices;
    std::vector<uint64_t> deviceSizes;
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

    if (errors.empty()) {
        for (size_t d = 0; d < devices.size(); ++d) {
            const IsoInfo& iso = (isos.size() == 1) ? isos[0] : isos[d];
            if (auto err = checkCapacity(wt, iso, devices[d], deviceSizes[d]))
                errors.push_back(std::move(*err));
        }
    }

    if (!errors.empty()) {
        printValidationErrors(wt, errors);
        return 1;
    }

    std::vector<std::pair<IsoInfo, std::string>> pairs;
    for (size_t d = 0; d < devices.size(); ++d)
        pairs.emplace_back(isos.size() == 1 ? isos[0] : isos[d], devices[d]);

    ThreadPool& pool = getStaticThreadPool();
    if (pairs.size() > pool.threadCount()) {
        std::cerr << "\n" << wt.colorFailure << "Error: "
                  << UI::Palette::Yellow << "write2usb"
                  << wt.colorFailure << " selection limit is "
                  << wt.colorWarning << pool.threadCount()
                  << wt.colorFailure << " (thread pool size)."
                  << wt.rl_resetCol << "\n\n";
        return 1;
    }

    auto pairsPtr =
        std::make_shared<std::vector<std::pair<IsoInfo, std::string>>>(
            std::move(pairs));

    auto progressDataPtr = std::make_shared<std::vector<ProgressInfo>>();
    progressDataPtr->reserve(pairsPtr->size());

    for (const auto& [iso, device] : *pairsPtr) {
        progressDataPtr->push_back(
            ProgressInfo{iso.filename, device, iso.sizeStr, iso.path});
    }

    if (!args.silentMode) {
        performWriteOperation(*pairsPtr);
    } else {
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

    g_drainingManager.joinAll();

    size_t ok = 0;
    for (const auto& progress : *progressDataPtr) {
        if (progress.completed.load())
            ++ok;
    }

    if (!args.silentMode)
        std::cout << "\n";

    return (ok == pairsPtr->size() &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
}

// ─── Entry point ─────────────────────────────────────────────────────────────

/**
 * @brief Primary entry point for handling mount/umount/convert/write CLI commands.
 *
 * Parses arguments via parseArgs(), then dispatches to handleMount(),
 * handleUmount(), or (with c2iso / w2usb) handleConvert() / handleWriteUsb().
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return int 0 on success, 1 on error.
 */
int handleMountUmountConvertWriteCommands(int argc, char* argv[]) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);
    GlobalState::isNonInteractive.store(true);

    if (argc < 2) {
        errMsg("No arguments provided.");
        return 1;
    }

    ParsedArgs args;
    if (!parseArgs(argc, argv, args) || args.action.empty()) {
        return 1;
    }

    if (args.convert)
        return handleConvert(args);

    if (args.writeUsb)
        return handleWriteUsb(args);

    if (args.action == "mount")
        return handleMount(args);

    if (args.action == "umount" || args.action == "unmount")
        return handleUmount(args);

    errMsg(std::string("Unknown action '") + args.action + "'");
    return 1;
}
