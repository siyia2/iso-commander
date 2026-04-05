// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UMOUNT_H
#define UMOUNT_H

#include "themes.h"

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Formats terminal output messages for unmounting operations using 
 * ANSI color codes and theme-aware styling.
 */
struct VerboseMessageFormatter {
    const ListTheme* theme;
    const bool isOriginal;

    static constexpr std::string_view orig_errorLabel   = "\033[1;91m";
    static constexpr std::string_view orig_errorPath    = "\033[1;93m";
    static constexpr std::string_view orig_successLabel = "\033[0;1m";
    static constexpr std::string_view orig_successPath  = "\033[1;92m";
    static constexpr std::string_view reset              = "\033[0m";
    static constexpr std::string_view bold               = "\033[0;1m";

    VerboseMessageFormatter()
        : theme(getActiveTheme()), isOriginal(globalTheme == "original") {}

    /**
     * @brief Generates a formatted string based on the status of an unmount attempt.
     * @param messageType The category of the result (e.g., "success", "error").
     * @param path The filesystem path being processed.
     * @return A styled string ready for terminal output.
     */
    std::string format(const std::string& messageType, const std::string& path) {
        std::string buf;
        buf.reserve(256);

        std::string_view errLabel  = isOriginal ? orig_errorLabel   : theme->secondary;
        std::string_view errPath   = isOriginal ? orig_errorPath    : theme->warning;
        std::string_view okLabel   = isOriginal ? orig_successLabel : theme->muted;
        std::string_view okPath    = isOriginal ? orig_successPath  : theme->primary;

        if (messageType == "root_error") {
            buf.append(errLabel).append("Failed to unmount: ")
               .append(errPath).append("'").append(path).append("'")
               .append(reset).append(bold).append(" {needsRoot}")
               .append(reset);
        }
        else if (messageType == "success") {
            buf.append(okLabel).append("Unmounted: ")
               .append(okPath).append("'").append(path).append("'")
               .append(reset).append(".");
        }
        else if (messageType == "error") {
            buf.append(errLabel).append("Failed to unmount: ")
               .append(errPath).append("'").append(path).append("'")
               .append(reset).append(bold).append(" {notAnISO}")
               .append(reset);
        }
        else if (messageType == "cancel") {
            buf.append(errLabel).append("Failed to unmount: ")
               .append(errPath).append("'").append(path).append("'")
               .append(reset).append(bold).append(" {cxl}")
               .append(reset);
        }

        return buf;
    }
};

#endif // UMOUNT_H
