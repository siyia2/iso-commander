// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOUNT_H
#define MOUNT_H

#include "display.h"
#include "themes.h"

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Provides high-performance string formatting for ISO mount operations, 
 * utilizing a pre-allocated internal buffer to minimize heap allocations during output.
 */
struct VerbosityFormatter {
    const ListTheme* theme;
    const bool isOriginal;
    std::string outputBuffer;

    // --- Consolidated Theme Colors ---
    std::string_view labelColor;
    std::string_view fileColor;
    std::string_view mountColor;
    std::string_view errLabel;
    std::string_view errFile;
    std::string_view metaColor; // For fsType and metadata

    VerbosityFormatter()
        : theme(getActiveTheme()), 
          isOriginal(globalTheme == "original"),
          labelColor(isOriginal ? originalColors::bold   : theme->muted),
          fileColor (isOriginal ? originalColors::green  : theme->primary),
          mountColor(isOriginal ? originalColors::blue   : theme->accent),
          errLabel  (isOriginal ? originalColors::red    : theme->secondary),
          errFile   (isOriginal ? originalColors::yellow : theme->warning),
          metaColor (isOriginal ? originalColors::boldAlt : theme->muted) 
    {
        outputBuffer.reserve(512);
    }

    /**
     * @brief Formats a message indicating a successful ISO mount.
     */
    std::string formatMountSuccess(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& mntDir, const std::string& mntFile,
                                   const std::string& fsType = "") {
        outputBuffer.clear();

        outputBuffer.append(labelColor).append("ISO: ")
                    .append(fileColor).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(originalColors::boldAlt)
                    .append(labelColor).append(" mnt@: ")
                    .append(mountColor).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(originalColors::boldAlt).append(originalColors::boldAlt).append(".");

        if (!fsType.empty()) {
            outputBuffer.append(" ").append(metaColor).append("{").append(fsType).append("}");
        }

        outputBuffer.append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a standard error message for failed mount attempts.
     */
    std::string formatError(const std::string& isoDir, const std::string& isoFile,
                            const std::string& errorCode) {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(originalColors::boldAlt)
                    .append(errLabel).append(". {").append(errorCode).append("}")
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a detailed error message including specific failure descriptions.
     */
    std::string formatDetailedError(const std::string& isoDir, const std::string& isoFile,
                                    const std::string& errorDetail) {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(originalColors::boldAlt)
                    .append(errLabel).append(". ").append(errorDetail)
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a message for an ISO that is already mounted.
     */
    std::string formatSkipped(const std::string& isoDir, const std::string& isoFile,
                              const std::string& mntDir, const std::string& mntFile) {
        outputBuffer.clear();

        std::string_view skipLabel = isOriginal ? originalColors::yellow : theme->warning;

        outputBuffer.append(skipLabel).append("ISO: ")
                    .append(fileColor).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(skipLabel).append(" alr mnt@: ")
                    .append(mountColor).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(skipLabel).append(".")
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a failure message categorized by specific error types.
     */
    std::string formatMountFailure(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& errorType, const std::string& mountTarget = "") {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(originalColors::boldAlt)
                    .append(errLabel).append(". ");

        if (errorType == "clx")             outputBuffer.append("Operation was cancelled");
        else if (errorType == "needsRoot")  outputBuffer.append("Root privileges required for mounting");
        else if (errorType == "missingISO") outputBuffer.append("ISO file not found");
        else if (errorType == "badFS")      outputBuffer.append("Failed to mount (unsupported filesystem or corrupted ISO)");
        else if (!mountTarget.empty())      outputBuffer.append("Already mounted at ").append(mountTarget);
        else                                outputBuffer.append(errorType);

        outputBuffer.append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a generic skip message for an ISO.
     */
    std::string formatMountSkipped(const std::string& isoDir, const std::string& isoFile) {
        outputBuffer.clear();

        std::string_view skipLabel = isOriginal ? originalColors::yellow : theme->warning;

        outputBuffer.append(skipLabel).append("ISO: ")
                    .append(fileColor).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(skipLabel).append(" skipped.")
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

private:
    std::string getPath(const std::string& dir, const std::string& file) const {
        return (displayConfig::toggleNamesOnly) ? file : dir + "/" + file;
    }
};

#endif // MOUNT_H
