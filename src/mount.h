// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOUNT_H
#define MOUNT_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Project Headers
#include "display.h"
#include "themes.h"

namespace fs = std::filesystem;

/**
 * @brief High-performance formatter for mount-related terminal output.
 * @details Utilizes a persistent internal buffer to minimize heap allocations 
 * and leverages std::string_view for zero-copy parameter passing. This ensures 
 * that verbose logging remains fast even during high-volume batch operations.
 */
struct VerbosityFormatter {
    const SemanticUIColors tc;
    std::string outputBuffer;

    VerbosityFormatter() : tc(resolveVerboseTheme()) {
        outputBuffer.reserve(512); // Reused for every call
    }

    // Use std::string_view for all parameters to avoid caller-side allocations
    std::string formatMountSuccess(std::string_view isoDir, std::string_view isoFile,
                                   std::string_view mntDir, std::string_view mntFile,
                                   std::string_view fsType = "") {
        outputBuffer.clear();
        outputBuffer.append(tc.label).append("ISO: ")
                    .append(tc.path).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.label).append(" mnt@: ")
                    .append(tc.highlight).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(tc.label).append("."); 

        if (!fsType.empty()) {
            outputBuffer.append(" ").append(tc.label).append("{").append(fsType).append("}");
        }
        return outputBuffer.append(tc.reset);
    }

    std::string formatError(std::string_view isoDir, std::string_view isoFile,
                            std::string_view errorCode) {
        outputBuffer.clear();
        return outputBuffer.append(tc.error).append("Failed to mnt: ")
                    .append(tc.warning).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.error).append(". ")
                    .append(tc.label).append("{").append(errorCode).append("}")
                    .append(tc.reset);
    }

    std::string formatDetailedError(std::string_view isoDir, std::string_view isoFile,
                                    std::string_view errorDetail) {
        outputBuffer.clear();
        return outputBuffer.append(tc.error).append("Failed to mnt: ")
                    .append(tc.warning).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.error).append(". ")
                    .append(tc.label).append(errorDetail)
                    .append(tc.reset);
    }

    std::string formatSkipped(std::string_view isoDir, std::string_view isoFile,
                              std::string_view mntDir, std::string_view mntFile) {
        outputBuffer.clear();
        return outputBuffer.append(tc.warning).append("ISO: ")
                    .append(tc.path).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.warning).append(" alr mnt@: ")
                    .append(tc.highlight).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(tc.warning).append(".")
                    .append(tc.reset);
    }

    std::string formatMountFailure(std::string_view isoDir, std::string_view isoFile,
                                   std::string_view errorType, std::string_view mountTarget = "") {
        outputBuffer.clear();
        outputBuffer.append(tc.error).append("Failed to mnt: ")
                    .append(tc.warning).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.error).append(". ").append(tc.label); 

        if (errorType == "clx")             outputBuffer.append("Operation was cancelled");
        else if (errorType == "needsRoot")  outputBuffer.append("Root privileges required");
        else if (errorType == "missingISO") outputBuffer.append("ISO file not found");
        else if (!mountTarget.empty())      outputBuffer.append("Already mounted at ").append(mountTarget);
        else                                outputBuffer.append(errorType);

        return outputBuffer.append(tc.reset);
    }

    std::string formatMountSkipped(std::string_view isoDir, std::string_view isoFile) {
        outputBuffer.clear();
        return outputBuffer.append(tc.warning).append("ISO: ")
                    .append(tc.path).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.warning).append(" skipped.")
                    .append(tc.reset);
    }

private:
    // Helper now returns std::string because it might perform concatenation
    std::string getPath(std::string_view dir, std::string_view file) const {
        if (displayConfig::toggleNamesOnly) {
            return std::string(file);
        }
        std::string fullPath;
        fullPath.reserve(dir.size() + file.size() + 1);
        fullPath.append(dir).append("/").append(file);
        return fullPath;
    }
};

void mountIsoFiles(const std::vector<std::string>& isoFiles, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);

#endif // MOUNT_H
