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
    std::string_view labelColor;  ///< Color for static labels like "ISO:" or "mnt@:"
    std::string_view fileColor;   ///< Color for the source ISO file path
    std::string_view mountColor;  ///< Color for the destination mount point
    std::string_view errLabel;    ///< Color for error prefixes (e.g., "Failed to mnt:")
    std::string_view errFile;     ///< Color for the filename within an error message
    std::string_view errDesc;     ///< Color for the error reason or bracketed error codes
    std::string_view metaColor;   ///< Color for metadata like {filesystem_type}

    /**
     * @brief Constructs the formatter and caches colors based on the active theme.
     */
    VerbosityFormatter()
        : theme(getActiveTheme()), 
          isOriginal(globalTheme == "original"),
          labelColor(isOriginal ? originalColors::boldAlt  : theme->muted),
          fileColor (isOriginal ? originalColors::green    : theme->primary),
          mountColor(isOriginal ? originalColors::blue     : theme->accent),
          errLabel  (isOriginal ? originalColors::red      : theme->secondary),
          errFile   (isOriginal ? originalColors::yellow   : theme->warning),
          errDesc   (isOriginal ? originalColors::boldAlt  : theme->muted),
          metaColor (isOriginal ? originalColors::boldAlt  : theme->muted) 
    {
        outputBuffer.reserve(512);
    }

    /**
     * @brief Formats a success message: ISO: 'path' mnt@: 'target'. {fs}
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @param mntDir The base mount directory.
     * @param mntFile The specific mount folder/file name.
     * @param fsType Optional string indicating the filesystem (e.g., "iso9660").
     * @return A formatted ANSI string.
     */
    std::string formatMountSuccess(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& mntDir, const std::string& mntFile,
                                   const std::string& fsType = "") {
        outputBuffer.clear();

        outputBuffer.append(labelColor).append("ISO: ")
                    .append(fileColor).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(labelColor).append(" mnt@: ")
                    .append(mountColor).append("'").append(mntDir).append("/").append(mntFile).append("'")
                    .append(labelColor).append("."); 

        if (!fsType.empty()) {
            outputBuffer.append(" ").append(metaColor).append("{").append(fsType).append("}");
        }

        outputBuffer.append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a standard error with a bracketed code: Failed to mnt: 'path'. {code}
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @param errorCode The short error tag (e.g., "EPERM").
     * @return A formatted ANSI string.
     */
    std::string formatError(const std::string& isoDir, const std::string& isoFile,
                            const std::string& errorCode) {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(errLabel).append(".") 
                    .append(" ")
                    .append(errDesc).append("{").append(errorCode).append("}")
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a detailed error with a custom description: Failed to mnt: 'path'. detail
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @param errorDetail A human-readable string explaining the failure.
     * @return A formatted ANSI string.
     */
    std::string formatDetailedError(const std::string& isoDir, const std::string& isoFile,
                                    const std::string& errorDetail) {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(errLabel).append(".") 
                    .append(" ")
                    .append(errDesc).append(errorDetail)
                    .append(originalColors::boldAlt);
        return outputBuffer;
    }

    /**
     * @brief Formats a message for an already mounted ISO: ISO: 'path' alr mnt@: 'target'.
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @param mntDir The base mount directory.
     * @param mntFile The existing mount folder/file name.
     * @return A formatted ANSI string.
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
     * @brief Formats failures based on specific internal error types.
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @param errorType A string key identifying the error (e.g., "needsRoot", "clx").
     * @param mountTarget Optional path if the error is due to an existing mount elsewhere.
     * @return A formatted ANSI string.
     */
    std::string formatMountFailure(const std::string& isoDir, const std::string& isoFile,
                                   const std::string& errorType, const std::string& mountTarget = "") {
        outputBuffer.clear();

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'").append(getPath(isoDir, isoFile)).append("'")
                    .append(errLabel).append(".") 
                    .append(" ")
                    .append(errDesc); 

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
     * @brief Formats a generic skip message: ISO: 'path' skipped.
     * @param isoDir Directory containing the ISO.
     * @param isoFile The ISO filename.
     * @return A formatted ANSI string.
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
    /**
     * @brief Helper to resolve the path display format based on user configuration.
     * @return Either the filename or the full path string.
     */
    std::string getPath(const std::string& dir, const std::string& file) const {
        return (displayConfig::toggleNamesOnly) ? file : dir + "/" + file;
    }
};

#endif // MOUNT_H
