// SPDX-License-Identifier: GNU General Public License v2.0

#ifndef MOUNT_H
#define MOUNT_H


// Structure to handle formatting verbose output messages for mount
struct VerbosityFormatter {
    // Format strings for different message types
    const std::string mountedFormatPrefix = "\033[1mISO: \033[1;92m'";
    const std::string mountedFormatMiddle = "'\033[0m\033[1m mnt@: \033[1;94m'";
    const std::string mountedFormatSuffix = "\033[1;94m'\033[0;1m.";
    const std::string mountedFormatSuffixWithFS = "\033[1;94m'\033[0;1m. {";
    const std::string mountedFormatEnd = "\033[0m";
    const std::string errorFormatPrefix = "\033[1;91mFailed to mnt: \033[1;93m'";
    const std::string errorFormatSuffix = "'\033[0m\033[1;91m.\033[0;1m ";
    const std::string errorFormatEnd = "\033[0m";
    const std::string skippedFormatPrefix = "\033[1;93mISO: \033[1;92m'";
    const std::string skippedFormatMiddle = "'\033[1;93m alr mnt@: \033[1;94m'";
    const std::string skippedFormatSuffix = "\033[1;94m'\033[1;93m.\033[0m";
    
    // String buffer for message formatting
    std::string outputBuffer;
    
    VerbosityFormatter() {
        outputBuffer.reserve(512);  // Reserve space for a typical message
    }
    
    // Format a successful mount message
    std::string formatMountSuccess(const std::string& isoDirectory, const std::string& isoFilename,
                                  const std::string& mountisoDirectory, const std::string& mountisoFilename,
                                  const std::string& fsType = "") {
        outputBuffer.clear();
        outputBuffer.append(mountedFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append(mountedFormatMiddle)
                   .append(mountisoDirectory).append("/").append(mountisoFilename);
        
        if (!fsType.empty()) {
            outputBuffer.append(mountedFormatSuffixWithFS)
                       .append(fsType)
                       .append("}")
                       .append(mountedFormatEnd);
        } else {
            outputBuffer.append(mountedFormatSuffix)
                       .append(mountedFormatEnd);
        }
        
        return outputBuffer;
    }
    
    // Format an error message
    std::string formatError(const std::string& isoDirectory, const std::string& isoFilename, 
                           const std::string& errorCode) {
        outputBuffer.clear();
        outputBuffer.append(errorFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append(errorFormatSuffix).append("{").append(errorCode).append("}")
                   .append(errorFormatEnd);
        return outputBuffer;
    }
    
    // Format a detailed error message with custom text
    std::string formatDetailedError(const std::string& isoDirectory, const std::string& isoFilename, 
                                   const std::string& errorDetail) {
        outputBuffer.clear();
        outputBuffer.append(errorFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append(errorFormatSuffix).append(errorDetail)
                   .append(errorFormatEnd);
        return outputBuffer;
    }
    
    // Format a skipped (already mounted) message
    std::string formatSkipped(const std::string& isoDirectory, const std::string& isoFilename,
                             const std::string& mountisoDirectory, const std::string& mountisoFilename) {
        outputBuffer.clear();
        outputBuffer.append(skippedFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append(skippedFormatMiddle)
                   .append(mountisoDirectory).append("/").append(mountisoFilename)
                   .append(skippedFormatSuffix);
        return outputBuffer;
    }
    
    // Add the missing methods that were causing compilation errors
    
    // Format a mount failure message
    std::string formatMountFailure(const std::string& isoDirectory, const std::string& isoFilename, 
                                 const std::string& errorType, const std::string& mountTarget = "") {
        outputBuffer.clear();
        outputBuffer.append(errorFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append(errorFormatSuffix);
                   
        if (errorType == "clx") {
            outputBuffer.append("Operation was cancelled");
        } else if (errorType == "needsRoot") {
            outputBuffer.append("Root privileges required for mounting");
        } else if (errorType == "missingISO") {
            outputBuffer.append("ISO file not found");
        } else if (errorType == "badFS") {
            outputBuffer.append("Failed to mount (unsupported filesystem or corrupted ISO)");
        } else if (!mountTarget.empty()) {
            // This is for the case when it's already mounted
            outputBuffer.append("Already mounted at ").append(mountTarget);
        } else {
            // For other custom error messages
            outputBuffer.append(errorType);
        }
        
        outputBuffer.append(errorFormatEnd);
        return outputBuffer;
    }
    
    // Format a mount skipped message
    std::string formatMountSkipped(const std::string& isoDirectory, const std::string& isoFilename) {
        outputBuffer.clear();
        outputBuffer.append(skippedFormatPrefix)
                   .append(isoDirectory).append("/").append(isoFilename)
                   .append("'\033[1;93m skipped.\033[0m");
        return outputBuffer;
    }
};

#endif // MOUNT_H
