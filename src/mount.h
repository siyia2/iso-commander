// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOUNT_H
#define MOUNT_H

#include "display.h"
#include "themes.h"


// Structure to handle formatting verbose output messages for mount
struct VerbosityFormatter {
    const ListTheme* theme;
    const bool isOriginal;

    // Original hardcoded sequences (used as fallback when isOriginal)
    static constexpr std::string_view orig_errorPrefix    = "\033[1;91m";
    static constexpr std::string_view orig_errorFilename  = "\033[1;93m";
    static constexpr std::string_view orig_successLabel   = "\033[1m";
    static constexpr std::string_view orig_successFile    = "\033[1;92m";
    static constexpr std::string_view orig_successMount   = "\033[1;94m";
    static constexpr std::string_view orig_skippedLabel   = "\033[1;93m";
    static constexpr std::string_view orig_skippedFile    = "\033[1;92m";
    static constexpr std::string_view orig_skippedMount   = "\033[1;94m";
    static constexpr std::string_view reset               = "\033[0m";
    static constexpr std::string_view bold                = "\033[0;1m";

    std::string outputBuffer;

    VerbosityFormatter()
        : theme(getActiveTheme()), isOriginal(globalTheme == "original") {
        outputBuffer.reserve(512);
    }

    std::string formatMountSuccess(const std::string& isoDirectory, const std::string& isoFilename,
                                   const std::string& mountisoDirectory, const std::string& mountisoFilename,
                                   const std::string& fsType = "") {
        outputBuffer.clear();

        std::string_view labelColor = isOriginal ? orig_successLabel   : theme->muted;
        std::string_view fileColor  = isOriginal ? orig_successFile    : theme->primary;
        std::string_view mountColor = isOriginal ? orig_successMount   : theme->accent;

        outputBuffer.append(labelColor).append("ISO: ")
                    .append(fileColor).append("'")
                    .append(displayConfig::toggleNamesOnly ? "" : isoDirectory + "/")
                    .append(isoFilename)
                    .append("'")
                    .append(reset)
                    .append(labelColor).append(" mnt@: ")
                    .append(mountColor).append("'")
                    .append(mountisoDirectory).append("/").append(mountisoFilename)
                    .append("'")
                    .append(reset).append(bold).append(".");

        if (!fsType.empty()) {
            outputBuffer.append(" {").append(fsType).append("}");
        }

        outputBuffer.append(reset);
        return outputBuffer;
    }

    std::string formatError(const std::string& isoDirectory, const std::string& isoFilename,
                            const std::string& errorCode) {
        outputBuffer.clear();

        std::string_view errLabel = isOriginal ? orig_errorPrefix   : theme->secondary;
        std::string_view errFile  = isOriginal ? orig_errorFilename : theme->warning;

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'")
                    .append(displayConfig::toggleNamesOnly ? "" : isoDirectory + "/")
                    .append(isoFilename)
                    .append("'")
                    .append(reset)
                    .append(errLabel).append(". {").append(errorCode).append("}")
                    .append(reset);
        return outputBuffer;
    }

    std::string formatDetailedError(const std::string& isoDirectory, const std::string& isoFilename,
                                    const std::string& errorDetail) {
        outputBuffer.clear();

        std::string_view errLabel = isOriginal ? orig_errorPrefix   : theme->secondary;
        std::string_view errFile  = isOriginal ? orig_errorFilename : theme->warning;

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'")
                    .append(isoDirectory).append("/").append(isoFilename)
                    .append("'")
                    .append(reset)
                    .append(errLabel).append(". ").append(errorDetail)
                    .append(reset);
        return outputBuffer;
    }

    std::string formatSkipped(const std::string& isoDirectory, const std::string& isoFilename,
                              const std::string& mountisoDirectory, const std::string& mountisoFilename) {
        outputBuffer.clear();

        std::string_view labelColor = isOriginal ? orig_skippedLabel : theme->warning;
        std::string_view fileColor  = isOriginal ? orig_skippedFile  : theme->primary;
        std::string_view mountColor = isOriginal ? orig_skippedMount : theme->accent;

        outputBuffer.append(labelColor).append("ISO: ")
                    .append(fileColor).append("'")
                    .append(displayConfig::toggleNamesOnly ? "" : isoDirectory + "/")
                    .append(isoFilename)
                    .append("'")
                    .append(labelColor).append(" alr mnt@: ")
                    .append(mountColor).append("'")
                    .append(mountisoDirectory).append("/").append(mountisoFilename)
                    .append("'")
                    .append(labelColor).append(".")
                    .append(reset);
        return outputBuffer;
    }

    std::string formatMountFailure(const std::string& isoDirectory, const std::string& isoFilename,
                                   const std::string& errorType, const std::string& mountTarget = "") {
        outputBuffer.clear();

        std::string_view errLabel = isOriginal ? orig_errorPrefix   : theme->secondary;
        std::string_view errFile  = isOriginal ? orig_errorFilename : theme->warning;

        outputBuffer.append(errLabel).append("Failed to mnt: ")
                    .append(errFile).append("'")
                    .append(isoDirectory).append("/").append(isoFilename)
                    .append("'")
                    .append(reset)
                    .append(errLabel).append(". ");

        if (errorType == "clx") {
            outputBuffer.append("Operation was cancelled");
        } else if (errorType == "needsRoot") {
            outputBuffer.append("Root privileges required for mounting");
        } else if (errorType == "missingISO") {
            outputBuffer.append("ISO file not found");
        } else if (errorType == "badFS") {
            outputBuffer.append("Failed to mount (unsupported filesystem or corrupted ISO)");
        } else if (!mountTarget.empty()) {
            outputBuffer.append("Already mounted at ").append(mountTarget);
        } else {
            outputBuffer.append(errorType);
        }

        outputBuffer.append(reset);
        return outputBuffer;
    }

    std::string formatMountSkipped(const std::string& isoDirectory, const std::string& isoFilename) {
        outputBuffer.clear();

        std::string_view labelColor = isOriginal ? orig_skippedLabel : theme->warning;
        std::string_view fileColor  = isOriginal ? orig_skippedFile  : theme->primary;

        outputBuffer.append(labelColor).append("ISO: ")
                    .append(fileColor).append("'")
                    .append(isoDirectory).append("/").append(isoFilename)
                    .append("'")
                    .append(labelColor).append(" skipped.")
                    .append(reset);
        return outputBuffer;
    }
};

#endif // MOUNT_H
