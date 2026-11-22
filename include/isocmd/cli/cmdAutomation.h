
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CMDAUTOMATION_H
#define CMDAUTOMATION_H

#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Stores parsed command-line arguments and action options.
 */
struct ParsedArgs {
    std::string              action;      ///< Action keyword: "mount" / "umount" / "convert" (c2iso) / "w2usb"
    std::vector<std::string> paths;       ///< Positional arguments that follow the action keyword
    bool                     silentMode = false; ///< Suppresses informational output and progress messages
    bool                     convert    = false; ///< c2iso: non-interactive conversion
    bool                     writeUsb   = false; ///< w2usb: non-interactive ISO -> USB write
    int                      maxDepth   = -1;    ///< Maximum directory scan depth; -1 means unlimited
};

namespace IsoCli {

/**
 * @brief Prints an informational message unless silent mode is enabled.
 *
 * @param silentMode Whether informational output should be suppressed.
 * @param msg        Message to print.
 */
void verboseInfo(bool silentMode, const std::string& msg);

/**
 * @brief Prints a warning message unless silent mode is enabled.
 *
 * @param silentMode Whether warning output should be suppressed.
 * @param msg        Warning message to print.
 */
void verboseWarn(bool silentMode, const std::string& msg);

/**
 * @brief Prints a warning message with an optional associated path.
 *
 * @param silentMode Whether warning output should be suppressed.
 * @param reason     Warning message or reason.
 * @param path       Optional path associated with the warning.
 */
void warnMsg(bool silentMode, std::string_view reason, std::string_view path = "");

/**
 * @brief Prints an error message.
 *
 * @param msg Error message to print.
 */
void errMsg(std::string_view msg);

} // namespace IsoCli

/**
 * @brief Executes the non-interactive mount operation.
 *
 * @param args Parsed command-line arguments.
 * @return Operation exit status.
 */
int handleMount(const ParsedArgs& args);

/**
 * @brief Executes the non-interactive unmount operation.
 *
 * @param args Parsed command-line arguments.
 * @return Operation exit status.
 */
int handleUmount(const ParsedArgs& args);

#endif // CMDAUTOMATION_H
