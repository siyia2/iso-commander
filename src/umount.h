// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UMOUNT_H
#define UMOUNT_H

// C++ Standard Library Headers
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

// Project Headers
#include "themes.h"

/**
 * @brief Formatter for unmount-specific verbose terminal messages.
 * @details Generates ANSI color-coded strings for unmounting operations,
 * utilizing std::string_view and buffer reservation to ensure high performance
 * and minimal heap allocations during batch processing.
 */
struct VerboseMessageFormatter {
    const SemanticUIColors tc;

    VerboseMessageFormatter() : tc(resolveVerboseTheme()) {}

    // Change parameters to std::string_view to avoid caller-side allocations
    std::string format(std::string_view messageType, std::string_view path) const {
        std::string buf;
        // Pre-calculating a safe minimum size prevents multiple reallocations during append
        buf.reserve(128 + path.size());

        auto appendError = [&](std::string_view tag) {
            buf.append(tc.error).append("Failed to unmount: ")
               .append(tc.warning).append("'").append(path).append(tc.warning).append("'")
               .append(tc.error).append(". ")
               .append(tc.label).append("{").append(tag).append("}")
               .append(tc.reset);
        };

        // Comparison of string_view with "literals" is highly optimized in C++20
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

void unmountISO(const std::vector<std::string>& isoDirs, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);

#endif // UMOUNT_H
