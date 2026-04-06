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

		// Styling definitions
		std::string_view errLabel = isOriginal ? originalColors::red      : theme->secondary;
		std::string_view errPath  = isOriginal ? originalColors::yellow   : theme->warning;
		std::string_view okLabel  = isOriginal ? originalColors::boldAlt  : theme->muted;
		std::string_view okPath   = isOriginal ? originalColors::green    : theme->primary;
		
		std::string_view tagStyle = isOriginal ? originalColors::red      : theme->muted;
		
		auto appendError = [&](std::string_view tag) {
			buf.append(errLabel).append("Failed to unmount: ")
			   .append(errPath).append("'").append(path).append("'")
			   .append(originalColors::reset)

			   .append(tagStyle) 
			   .append(" {").append(tag).append("}")
			   .append(originalColors::reset);
		};

		if (messageType == "success") {
			buf.append(okLabel).append("Unmounted: ")
			   .append(okPath).append("'").append(path).append("'")
			   .append(originalColors::reset).append(".");
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
