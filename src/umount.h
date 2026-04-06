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

    // --- Cached Styling ---
    std::string_view errLabel; // "Failed to unmount:"
    std::string_view errPath;  // The file path in error
    std::string_view errDesc;  // The error tag {needsRoot, etc}
    std::string_view okLabel;  // "Unmounted:"
    std::string_view okPath;   // The successful path

    VerboseMessageFormatter()
        : theme(getActiveTheme()), 
          isOriginal(globalTheme == "original"),
          errLabel(isOriginal ? originalColors::red     : theme->secondary),
          errPath (isOriginal ? originalColors::yellow  : theme->warning),
          errDesc (isOriginal ? originalColors::boldAlt : theme->muted), // Distinct color for tags
          okLabel (isOriginal ? originalColors::boldAlt : theme->muted),
          okPath  (isOriginal ? originalColors::green   : theme->primary) 
    {}

    /**
     * @brief Generates a formatted string based on the status of an unmount attempt.
     */
    std::string format(const std::string& messageType, const std::string& path) {
        std::string buf;
        buf.reserve(256);

        // Helper to append errors with the separated description color
        auto appendError = [&](std::string_view tag) {
            buf.append(errLabel).append("Failed to unmount: ")
               .append(errPath).append("'").append(path).append("'")
               .append(errLabel).append(".") // Keeps the dot in the label color
               .append(" ")
               .append(errDesc).append("{").append(tag).append("}") // Uses new desc color
               .append(originalColors::boldAlt);
        };

        if (messageType == "success") {
            buf.append(okLabel).append("Unmounted: ")
               .append(okPath).append("'").append(path).append("'")
               .append(okLabel).append(".")
               .append(originalColors::boldAlt);
        }
        else if (messageType == "root_error") {
            appendError("needsRoot");
        }
        else if (messageType == "error") {
            appendError("notAnISO");
        }
        else if (messageType == "cancel") {
            appendError("cxl");
        }

        return buf;
    }
};

#endif // UMOUNT_H
