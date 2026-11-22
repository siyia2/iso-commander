// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <string>
#include <stdexcept>

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

// ─── Argument parsing ────────────────────────────────────────────────────────

using namespace IsoCli;

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

// Implemented in cmdAutomationCovertWrite.cpp
int handleConvert(const ParsedArgs& args);
int handleWriteUsb(const ParsedArgs& args);

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
