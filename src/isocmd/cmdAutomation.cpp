// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

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
            if (g_operationCancelled.load()) return;
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

// ─── Argument parsing ────────────────────────────────────────────────────────

struct ParsedArgs {
    std::string              action;      ///< Last positional arg: "mount" / "umount" / …
    std::vector<std::string> paths;
    bool                     silentMode = false;
    int                      maxDepth   = -1;
};

/**
 * @brief Parse argv into a ParsedArgs structure.
 * @return false when no positional arguments were found (caller should abort).
 */
static bool parseArgs(int argc, char* argv[], ParsedArgs& out) {
	bool captureWarn = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--silent") {
            out.silentMode = true;
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

    std::unordered_set<std::string> isoFiles;
    bool hasErrors = false;

    for (const auto& rawPath : args.paths) {
        if (g_operationCancelled.load()) {
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

    if (g_operationCancelled.load()) {
        verboseWarn(args.silentMode, "Mount operation cancelled by user.");
    }

    if (isoFiles.empty()) {
        if (!g_operationCancelled.load())
            verboseWarn(args.silentMode, "\nNo ISO files found to mount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("\nLocated ")
                .append(std::to_string(isoFiles.size()))
                .append(" ISO file")
                .append(isoFiles.size() == 1 ? "" : "s")
                .append("; Attempting to mount...\n"));

    std::unordered_set<std::string> mountedFiles, skippedMessages, mountedFails;
    std::atomic<size_t> completedTasks{0}, failedTasks{0};

    mountIsoFiles(std::vector<std::string>(isoFiles.begin(), isoFiles.end()),
                  mountedFiles, skippedMessages, mountedFails,
                  &completedTasks, &failedTasks, args.silentMode);

    if (!args.silentMode) {
        for (const auto& msg : mountedFiles)    std::cout << msg << "\n";
        for (const auto& msg : skippedMessages) std::cout << msg << "\n";
        for (const auto& msg : mountedFails)    std::cout << msg << "\n";
        std::cout << "\nMount summary:\n"
                  << " Successful: " << completedTasks.load() << "\n"
                  << " Failed:     " << failedTasks.load()
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

    std::unordered_set<std::string> mountPoints;
    bool hasErrors = false;

    // Collect every iso_* subdirectory under a given /mnt path.
    auto collectFromMnt = [&](const fs::path& mntPath) {
        disableInput();
        verboseInfo(args.silentMode,
                    std::string("Scanning ")
                    .append(mntPath.string())
                    .append(" for ISO mount points (surface scan)..."));
        try {
            for (const auto& entry : fs::directory_iterator(mntPath)) {
                if (g_operationCancelled.load()) return;
                if (entry.is_directory()) {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("iso_", 0) == 0)
                        mountPoints.insert(fs::canonical(entry.path()).string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            errMsg(std::string("Error scanning /mnt: ") + e.what());
            hasErrors = true;
        }
    };

    // No paths supplied (or only "all") → scan all of /mnt.
    const bool scanAllMnt = args.paths.empty() ||
                            (args.paths.size() == 1 && args.paths[0] == "all");

    if (scanAllMnt) {
        collectFromMnt("/mnt");
        if (g_operationCancelled.load()) {
            verboseWarn(args.silentMode, "Operation cancelled by user.");
            return 1;
        }
    } else {
        for (const auto& rawPath : args.paths) {
            if (g_operationCancelled.load()) {
                verboseWarn(args.silentMode, "Operation cancelled by user.");
                return 1;
            }

            fs::path path(rawPath);

            try {
                if (fs::exists(path) && fs::is_directory(path)) {
                    const std::string canonical = fs::canonical(path).string();

                    if (canonical == "/mnt") {
                        collectFromMnt(path);
                    } else if (canonical.rfind("/mnt/iso_", 0) == 0) {
                        mountPoints.insert(canonical);
                    } else {
                        warnMsg(args.silentMode,
                                "is not allowed. Only /mnt or /mnt/iso_* are valid.",
                                rawPath);
                        hasErrors = true;
                    }
                } else {
                    // Accept bare names: "mydisc" → /mnt/iso_mydisc
                    fs::path candidate = path.is_relative()
                        ? fs::path("/mnt") / ("iso_" + path.filename().string())
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

    if (g_operationCancelled.load()) {
        verboseWarn(args.silentMode, "Umount operation cancelled by user.");
    }

    if (mountPoints.empty()) {
        if (!g_operationCancelled.load())
            verboseWarn(args.silentMode, "\nNo ISO mount points found to unmount.");
        return hasErrors ? 1 : 0;
    }

    verboseInfo(args.silentMode,
                std::string("\nLocated ")
                .append(std::to_string(mountPoints.size()))
                .append(" mount point")
                .append(mountPoints.size() == 1 ? "" : "s")
                .append("; Attempting to unmount...\n"));

    std::unordered_set<std::string> unmountedFiles, unmountedErrors;
    std::atomic<size_t> completedTasks{0}, failedTasks{0};

    unmountISO(std::vector<std::string>(mountPoints.begin(), mountPoints.end()),
               unmountedFiles, unmountedErrors,
               &completedTasks, &failedTasks, args.silentMode);

    if (!args.silentMode) {
        for (const auto& msg : unmountedFiles)  std::cout << msg << "\n";
        for (const auto& msg : unmountedErrors) std::cout << msg << "\n";
        std::cout << "\nUnmount summary:\n"
                  << " Successful: " << completedTasks.load() << "\n"
                  << " Failed:     " << failedTasks.load()
                  << UI::Palette::Reset << "\n\n";
    }

    return (failedTasks.load() == 0 && !hasErrors) ? 0 : 1;
}

// ─── Entry point ─────────────────────────────────────────────────────────────

/**
 * @brief Primary entry point for handling mount/umount CLI commands.
 *
 * Parses arguments via parseArgs(), then dispatches to handleMount() or
 * handleUmount().
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return int 0 on success, 1 on error.
 */
int handleMountUmountCommands(int argc, char* argv[]) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    if (argc < 2) {
        errMsg("No arguments provided.");
        return 1;
    }

    ParsedArgs args;
    if (!parseArgs(argc, argv, args) || args.action.empty()) {
        errMsg("No action provided.");
        return 1;
    }

    if (args.action == "mount")
        return handleMount(args);

    if (args.action == "umount" || args.action == "unmount")
        return handleUmount(args);

    errMsg(std::string("Unknown action '") + args.action + "'");
    return 1;
}
