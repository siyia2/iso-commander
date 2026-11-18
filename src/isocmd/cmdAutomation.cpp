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
#include "../rendering.h"
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
 * @brief Determines whether a string represents a supported action keyword.
 *
 * @param action Argument to inspect.
 * @return true if the argument is a recognized action keyword;
 *         otherwise false.
 */
static bool isActionKeyword(const std::string& action) {
    return action == "mount" || action == "umount" ||
           action == "unmount" || action == "c2iso" ||
           action == "w2usb";
}

/**
 * @brief Determines whether an argument represents a supported option.
 *
 * Recognizes the "--silent" option and arguments beginning with "-d".
 * Depth-option validation is performed separately.
 *
 * @param arg Argument to inspect.
 * @return true if the argument is recognized as an option;
 *         otherwise false.
 */
static bool isFlag(const std::string& arg) {
    return arg == "--silent" || arg.rfind("-d", 0) == 0;
}

/**
 * @brief Determines whether a path identifies a device under /dev.
 *
 * @param path Path to inspect.
 * @return true if the path begins with "/dev/"; otherwise false.
 */
static bool isDevPath(std::string_view path) {
    return path.rfind("/dev/", 0) == 0;
}

/**
 * @brief Flushes any deferred warning formatting.
 *
 * Prints the terminal palette reset sequence followed by a newline
 * when a warning has been captured, then clears the capture state.
 *
 * @param captureWarn Indicates whether deferred warning formatting
 *                    needs to be flushed.
 */
static void flushDeferredWarn(bool& captureWarn) {
    if (!captureWarn)
        return;

    std::cout << UI::Palette::Reset << "\n";
    captureWarn = false;
}

/**
 * @brief Applies an action keyword to the parsed arguments.
 *
 * Maps "c2iso" to the internal "convert" action and enables the
 * conversion flag. Maps "w2usb" to the USB-writing action and enables
 * its corresponding flag. Both "umount" and "unmount" map to "umount".
 *
 * @param action Recognized action keyword.
 * @param out Parsed argument structure to update.
 */
static void applyAction(const std::string& action, ParsedArgs& out) {
    if (action == "c2iso") {
        out.convert = true;
        out.action  = "convert";
    } else if (action == "w2usb") {
        out.writeUsb = true;
        out.action   = "w2usb";
    } else if (action == "mount") {
        out.action = "mount";
    } else {
        out.action = "umount";
    }
}

/**
 * @brief Parses and applies a depth option.
 *
 * Depth options are ignored for w2usb and umount actions without
 * parsing or validating their values. For other actions, the numeric
 * suffix is parsed as an integer and assigned to @c maxDepth.
 *
 * Negative values produce a warning and select unlimited depth.
 * Invalid or partially parsed values produce an error.
 *
 * @param arg Complete depth option, including its "-d" prefix.
 * @param out Parsed argument structure to update.
 * @param captureWarn Deferred warning-formatting state.
 * @return true if the option is accepted or ignored; false if its
 *         depth value is invalid.
 */
static bool parseDepthOption(
    const std::string& arg,
    ParsedArgs& out,
    bool& captureWarn
) {
    if (out.writeUsb || out.action == "umount")
        return true;

    const std::string depthStr = arg.substr(2);

    try {
        size_t parsed = 0;
        const int depth = std::stoi(depthStr, &parsed);

        if (parsed != depthStr.size())
            throw std::invalid_argument("invalid depth");

        if (depth < 0) {
            flushDeferredWarn(captureWarn);
            warnMsg(out.silentMode,
                    "Negative depth; using unlimited depth.", "");
            out.maxDepth = -1;
        } else {
            out.maxDepth = depth;
        }
    } catch (const std::exception&) {
        flushDeferredWarn(captureWarn);
        errMsg("Invalid depth option: '" + arg + "'.");
        return false;
    }

    return true;
}

/**
 * @brief Processes a recognized command-line option.
 *
 * Enables silent mode for "--silent" or delegates depth-option
 * processing to @ref parseDepthOption.
 *
 * @param arg Option to process.
 * @param out Parsed argument structure to update.
 * @param captureWarn Deferred warning-formatting state.
 * @return true if the option was processed successfully; false if
 *         depth-option validation failed.
 */
static bool handleFlag(
    const std::string& arg,
    ParsedArgs& out,
    bool& captureWarn
) {
    if (arg == "--silent") {
        out.silentMode = true;
        return true;
    }

    return parseDepthOption(arg, out, captureWarn);
}

/**
 * @brief Validates and appends a positional path argument.
 *
 * For w2usb operations, ISO paths must precede device paths.
 * Once a device path has been encountered, subsequent non-device
 * paths are rejected.
 *
 * @param arg Positional path argument.
 * @param out Parsed argument structure to update.
 * @param pathsStarted Indicates whether a positional argument has
 *                     already been encountered.
 * @param captureWarn Deferred warning-formatting state.
 * @return true if the path was accepted; false if w2usb path ordering
 *         was violated.
 */
static bool appendPath(
    const std::string& arg,
    ParsedArgs& out,
    bool& pathsStarted,
    bool& captureWarn
) {
    if (out.writeUsb && !out.paths.empty() &&
        !isDevPath(arg) && isDevPath(out.paths.back())) {
        flushDeferredWarn(captureWarn);
        errMsg("w2usb: ISO paths must precede device paths.");
        return false;
    }

    out.paths.push_back(arg);
    pathsStarted = true;

    return true;
}

/**
 * @brief Validates and applies the initial command-line action.
 *
 * Ensures that the first argument is an action keyword rather than
 * an option or an unknown value. Applies the recognized action and
 * updates the action-seen state.
 *
 * @param arg First command-line argument to validate.
 * @param out Parsed argument structure to update.
 * @param actionSeen Indicates whether an action has been recognized.
 * @param captureWarn Deferred warning-formatting state.
 * @return true if the action was recognized; false otherwise.
 */
static bool parseInitialAction(
    const std::string& arg,
    ParsedArgs& out,
    bool& actionSeen,
    bool& captureWarn
) {
    if (isFlag(arg)) {
        flushDeferredWarn(captureWarn);
        errMsg("Option '" + arg +
               "' must follow the action keyword.");
        return false;
    }

    if (!isActionKeyword(arg)) {
        flushDeferredWarn(captureWarn);
        errMsg("Unknown action: '" + arg + "'.");
        return false;
    }

    actionSeen = true;
    applyAction(arg, out);

    return true;
}

/**
 * @brief Parses command-line arguments into a ParsedArgs structure.
 *
 * Expected grammar:
 *
 *     isocmd <action> [flags...] [paths...]
 *
 * The action keyword must be the first argument. Recognized options
 * may appear after the action but before the first positional path.
 * Once positional arguments begin, subsequent options are rejected.
 *
 * Supported actions:
 * - @c mount
 * - @c umount / @c unmount
 * - @c c2iso
 * - @c w2usb
 *
 * Supported options:
 * - @c --silent enables silent mode.
 * - @c -dN sets the maximum directory traversal depth for mount
 *   and c2iso operations.
 *
 * Negative depth values produce a warning and select unlimited depth.
 * Invalid depth values produce an error. Depth options are ignored
 * entirely for umount and w2usb operations.
 *
 * For w2usb operations, ISO paths must precede device paths. Once a
 * device path is encountered, subsequent non-device paths are rejected.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 * @param out Parsed argument structure to populate.
 *
 * @return true if parsing succeeds; false if the action is missing
 *         or invalid, an option is misplaced, a depth value is invalid,
 *         or w2usb path ordering is violated.
 */
static bool parseArgs(int argc, char* argv[], ParsedArgs& out) {
    bool captureWarn  = false;
    bool actionSeen   = false;
    bool pathsStarted = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        // 1. The action keyword must come first.
        if (!actionSeen) {
            if (!parseInitialAction(
                    arg, out, actionSeen, captureWarn))
                return false;

            continue;
        }

        // 2. Flags must appear before the first path.
        if (isFlag(arg)) {
            if (pathsStarted) {
                flushDeferredWarn(captureWarn);
                errMsg("Option '" + arg +
                       "' must precede all paths.");
                return false;
            }

            if (!handleFlag(arg, out, captureWarn))
                return false;

            continue;
        }

        // 3. Everything else is a path.
        if (!appendPath(arg, out, pathsStarted, captureWarn))
            return false;
    }

    flushDeferredWarn(captureWarn);

    if (!actionSeen) {
        errMsg("No action provided.");
        return false;
    }

    return true;
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
            return true; // Cancelled
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
 * @param silentMode Whether silent output mode is enabled.
 * @param showVerbose Flag indicating whether the user requested detailed logs.
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

        for (const auto& msg : verboseSets.operationCompleted)
            std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationSkipped)
            std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationFailed)
            std::cout << msg << UI::Palette::BoldReset << "\n";

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
static int handleMount(const ParsedArgs& args) {
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

// ─── Umount branch ───────────────────────────────────────────────────────────

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
    std::unordered_set<std::string> mountPoints; ///< Set of validated, canonicalized absolute paths to target mount points.
    bool hasErrors = false;                      ///< Flag indicating whether any path resolution or validation errors occurred.
    bool cancelled = false;                      ///< Flag indicating if an asynchronous user cancellation signal was detected.
    bool abortImmediately = false;               ///< Flag indicating whether to immediately abort execution without attempting unmounts.
};

/**
 * @brief Checks if the global operation cancellation flag has been set.
 *
 * @param[in] args Parsed command-line arguments for silent mode configuration.
 * @param[out] res Collection result structure to update cancellation metadata.
 * @param[in] message Custom warning message to display if cancelled.
 * @return true if the operation was cancelled, false otherwise.
 */
static bool checkCancellation(const ParsedArgs& args, CollectionResult& res, const char* message = "Operation cancelled by user.") {
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
static void collectFromMnt(const fs::path& mntPath, const ParsedArgs& args, CollectionResult& res) {
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
static void processUserPath(const std::string& rawPath, const ParsedArgs& args, CollectionResult& res) {
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
 * @param[in] args        Parsed command-line arguments controlling verbosity and silent settings.
 * @param[in] hasErrors   Flag indicating whether path validation errors were encountered prior to execution.
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

    std::atomic<size_t> nextTask{0};
    runUnmountWorkers(paths, args.silentMode, completedBytes, completedTasks, failedTasks, nextTask);

    isComplete.store(true, std::memory_order_release);

    if (barThread.joinable())
        barThread.join();

    // After a cancel, mark the mount points no worker claimed as cancelled.
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
                  << " Successful: " << completedTasks.load() << "\n"
                  << " Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (failedTasks.load() == 0 && !hasErrors &&
            !GlobalState::g_operationCancelled.load()) ? 0 : 1;
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
static int handleUmount(const ParsedArgs& args) {
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
static int handleConvert(const ParsedArgs& args) {
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
static int handleWriteUsb(const ParsedArgs& args) {
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

    auto pairsPtr = std::make_shared<std::vector<std::pair<IsoInfo, std::string>>>(std::move(pairs));
    auto progressDataPtr = std::make_shared<std::vector<ProgressInfo>>();
    progressDataPtr->reserve(pairsPtr->size());

    for (const auto& [iso, device] : *pairsPtr) {
        progressDataPtr->push_back(
            ProgressInfo{iso.filename, device, iso.sizeStr, iso.path});
    }

    executeWriteOperations(args.silentMode, pairsPtr, progressDataPtr, pool);

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
