// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOUNT_H
#define MOUNT_H

#include "display.h"
#include "themes.h"
#include <libmount/libmount.h>
#include <sys/stat.h>

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Provides high-performance string formatting for ISO mount operations, 
 * utilizing a pre-allocated internal buffer to minimize heap allocations during output.
 */
struct VerbosityFormatter {
    const SemanticUIColors tc;
    std::string outputBuffer;

    VerbosityFormatter() : tc(resolveVerboseTheme()) {
        outputBuffer.reserve(512);
    }

    std::string formatMountSuccess(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& mntDir, const std::string& mntFile,
                                   const std::string& fsType = "") {
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

    std::string formatError(const std::string& isoDir, const std::string& isoFile,
                            const std::string& errorCode) {
        outputBuffer.clear();
        return outputBuffer.append(tc.error).append("Failed to mnt: ")
                    .append(tc.warning).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.error).append(". ")
                    .append(tc.label).append("{").append(errorCode).append("}")
                    .append(tc.reset);
    }

    std::string formatDetailedError(const std::string& isoDir, const std::string& isoFile,
                                    const std::string& errorDetail) {
        outputBuffer.clear();
        return outputBuffer.append(tc.error).append("Failed to mnt: ")
                    .append(tc.warning).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.error).append(". ")
                    .append(tc.label).append(errorDetail)
                    .append(tc.reset);
    }

    std::string formatSkipped(const std::string& isoDir, const std::string& isoFile,
                              const std::string& mntDir, const std::string& mntFile) {
        outputBuffer.clear();
        return outputBuffer.append(tc.warning).append("ISO: ")
                    .append(tc.path).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.warning).append(" alr mnt@: ")
                    .append(tc.highlight).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(tc.warning).append(".")
                    .append(tc.reset);
    }

    std::string formatMountFailure(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& errorType, const std::string& mountTarget = "") {
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

    std::string formatMountSkipped(const std::string& isoDir, const std::string& isoFile) {
        outputBuffer.clear();
        return outputBuffer.append(tc.warning).append("ISO: ")
                    .append(tc.path).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(tc.warning).append(" skipped.")
                    .append(tc.reset);
    }

private:
    std::string getPath(const std::string& dir, const std::string& file) const {
        return (displayConfig::toggleNamesOnly) ? file : dir + "/" + file;
    }
};

void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, 
std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);

#endif // MOUNT_H
