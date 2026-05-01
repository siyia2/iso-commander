// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UMOUNT_H
#define UMOUNT_H

#include "themes.h"

// C++ Standard Library Headers
#include <filesystem>
#include <unordered_set>

// Third-Party Library Headers
#include <libmount/libmount.h>

namespace fs = std::filesystem;

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Formats terminal output messages for unmounting operations using 
 * ANSI color codes and theme-aware styling.
 */
struct VerboseMessageFormatter {
    const SemanticUIColors tc;

    VerboseMessageFormatter() : tc(resolveVerboseTheme()) {}

    std::string format(const std::string& messageType, const std::string& path) {
        std::string buf;
        buf.reserve(256);

        auto appendError = [&](std::string_view tag) {
            buf.append(tc.error).append("Failed to unmount: ")
               .append(tc.warning).append("'").append(path).append("'")
               .append(tc.error).append(". ")
               .append(tc.label).append("{").append(tag).append("}") 
               .append(tc.reset);
        };

        if (messageType == "success") {
            buf.append(tc.label).append("Unmounted: ")
               .append(tc.path).append("'").append(path).append(tc.path).append("'")
               .append(tc.label).append(".")
               .append(tc.reset);
        }
        else if (messageType == "root_error") appendError("needsRoot");
        else if (messageType == "error")      appendError("notAnISO");
        else if (messageType == "cancel")     appendError("cxl");

        return buf;
    }
};

void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, 
std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);

#endif // UMOUNT_H
