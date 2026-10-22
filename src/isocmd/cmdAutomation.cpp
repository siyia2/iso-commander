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
#include "../inputHandling.h"
#include "../mount.h"
#include "../state.h"
#include "../themes.h"
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
                    : std::string("max depth: ") + (args.maxDepth < 0
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

    if (GlobalState::g_operationCancelled.load()) {
        verboseWarn(args.silentMode, "Mount operation cancelled by user.");
    }

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

    mountIsoFiles(std::vector<std::string>(isoFiles.begin(), isoFiles.end()),
                  &completedTasks, &failedTasks, args.silentMode);

    if (!args.silentMode) {
        for (const auto& msg : verboseSets.operationCompleted)    std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationSkipped) std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationFailed)    std::cout << msg << "\n";
        std::cout << "\nMount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (completedTasks.load() > 0) ? 0 : 1;
}

// ─── Umount branch ───────────────────────────────────────────────────────────

static int handleUmount(const ParsedArgs& args) {
    if (geteuid() != 0) {
        errMsg("Root privileges required for unmounting ISOs.");
        return 1;
    }

    bool scanDir = false;

    std::unordered_set<std::string> mountPoints;
    bool hasErrors = false;

    // Collect every iso subdirectory under a given /mnt/ISOs path.
    auto collectFromMnt = [&](const fs::path& mntPath) {
        disableInput();
        verboseInfo(args.silentMode,
                    std::string("Scanning ")
                    .append(mntPath.string())
                    .append(" for ISO mount points (surface scan)..."));
        try {
            for (const auto& entry : fs::directory_iterator(mntPath)) {
                if (GlobalState::g_operationCancelled.load()) return;
                if (entry.is_directory()) {
                    const std::string name = entry.path().filename().string();
                    mountPoints.insert(fs::canonical(entry.path()).string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            if (!args.silentMode) {
               // errMsg(std::string("Error scanning /mnt/ISOs: ") + e.what());
            }
            hasErrors = true;
        }
        scanDir = true;
    };

    // No paths supplied (or only "all") → scan all of /mnt/ISOs.
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
                    // Accept bare names: "mydisc" → /mnt/ISOs/iso_mydisc
                    fs::path candidate = path.is_relative()
                        ? fs::path("/mnt/ISOs") / (path.filename().string())
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

    if (GlobalState::g_operationCancelled.load()) {
        verboseWarn(args.silentMode, "Umount operation cancelled by user.");
    }

    if (mountPoints.empty()) {
        if (!GlobalState::g_operationCancelled.load())
            verboseWarn(args.silentMode, "\nNo ISO mount points found to unmount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string(scanDir ? "\nLocated " : "Located ")
                .append(std::to_string(mountPoints.size()))
                .append(" mount point")
                .append(mountPoints.size() == 1 ? "" : "s")
                .append("; Attempting to unmount...\n"));

    std::atomic<size_t> completedTasks{0}, failedTasks{0};

    unmountISO(std::vector<std::string>(mountPoints.begin(), mountPoints.end()),
               &completedTasks, &failedTasks, args.silentMode);

    // Clean up the parent /mnt/ISOs directory if it's now empty after all processing is done.
    {
        std::error_code ec;
        fs::remove("/mnt/ISOs", ec);
    }

    if (!args.silentMode) {
        for (const auto& msg : verboseSets.operationCompleted)  std::cout << msg << "\n";
        for (const auto& msg : verboseSets.operationFailed) std::cout << msg << "\n";
        std::cout << "\nUnmount summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (failedTasks.load() == 0 && !hasErrors) ? 0 : 1;
}

// ─── Convert branch (-c) ─────────────────────────────────────────────────────

/**
 * @brief Image → ISO conversion with a byte-tracking progress bar.
 *
 * Accepts image files and/or directories (scanned per -dN). Files are batched
 * by type because convertToISO() selects its backend from mode flags.
 * Existing .iso outputs are skipped (counted as completed), so re-running is safe.
 * Root is not required; getRealUserId() inside convertToISO handles ownership.
 *
 * Progress is shown with displayProgressBarWithSize() on a background thread
 * (skipped with --silent). The bar ends with a "Display verbose output?" prompt;
 * the detailed per-file messages and summary are printed only if the answer is "y".
 *
 * @return 0 if everything converted/skipped cleanly, 1 on any failure.
 */
static int handleConvert(const ParsedArgs& args) {
    if (args.paths.empty()) {
        errMsg("No files or directories given to convert.");
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
                if (args.silentMode) disableInput();
                std::string depthDesc = (args.maxDepth == 0)
                    ? std::string("surface scan")
                    : std::string("max depth: ") + (args.maxDepth < 0
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
            verboseWarn(args.silentMode, "\nNo convertible image files found.");
        return hasErrors ? 1 : 0;
    }

    // Bucket by backend (sorted for deterministic order).
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
    std::atomic<bool>   isComplete{false};
    std::vector<std::string> successfulOutputs;   // TODO: hand to database indexer if desired
    std::mutex outMutex;
    bool showVerbose = false;

    // Total estimated output ISO bytes for the byte-tracking progress bar.
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

    if (totalBytes == 0) totalBytes = 1;  // Avoid division by zero.

    // The bar runs on its own thread until isComplete is set; silent mode skips it.
    // "convert2iso" contains "2iso", which selects the convert bar layout.
    std::thread barThread;
    if (!args.silentMode) {
        clearScrollBuffer();
        std::cout << "\n" << color << " Processing "
                  << (images.size() == 1 ? "task for " : "tasks for ")
                  << UI::Palette::Orange << "convert2iso" << color << "... ("
                  << UI::Palette::Red << "Ctrl+c"
                  << color << ":cancel)\n";

        barThread = std::thread(displayProgressBarWithSize,
                                &completedBytes, totalBytes,
                                &completedTasks, &failedTasks, images.size(),
                                &isComplete, &showVerbose,
                                std::string("convert2iso"));
    }

    auto run = [&](const std::vector<std::string>& files,
                   bool isMdf, bool isNrg, bool isChd, bool isDaa) {
        if (files.empty() || GlobalState::g_operationCancelled.load()) return;

        convertToISO(files, isMdf, isNrg, isChd, isDaa,
                     &completedBytes, &completedTasks, &failedTasks,
                     &successfulOutputs, &outMutex);
    };

    run(binImg, false, false, false, false);
    run(mdf,    true,  false, false, false);
    run(nrg,    false, true,  false, false);
    run(chd,    false, false, true,  false);
    run(daa,    false, false, false, true);

    isComplete.store(true, std::memory_order_release);
    if (barThread.joinable()) barThread.join();

    // The bar already shows status; details only if the user answered "y".
    if (!args.silentMode && showVerbose) {
        clearScrollBuffer();
        std::cout << "\n";
        for (const auto& msg : verboseSets.operationCompleted) std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationSkipped)   std::cout << msg << UI::Palette::BoldReset << "\n";
        for (const auto& msg : verboseSets.operationFailed)    std::cout << msg << UI::Palette::BoldReset <<"\n";
        std::cout << "\nConversion summary:\n"
                  << "  Successful: " << completedTasks.load() << "\n"
                  << "  Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    // Non-zero on any failure so scripts/cron can detect problems.
    return (failedTasks.load() == 0 && !hasErrors &&
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
