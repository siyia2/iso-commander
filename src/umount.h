// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef UMOUNT_H
#define UMOUNT_H


// Structure to handle verbose messages based on message type for umount
struct VerboseMessageFormatter {
    // Store format strings as member variables
    const std::string rootErrorPrefix = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string rootErrorSuffix = "\033[1;93m'\033[1;91m.\033[0;1m {needsRoot}";
    const std::string successPrefix = "\033[0;1mUnmounted: \033[1;92m'";
    const std::string successSuffix = "\033[1;92m'\033[0m.";
    const std::string errorPrefix = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string errorSuffix = "'\033[1;91m.\033[0;1m {notAnISO}";
    const std::string cancelPrefix = "\033[1;91mFailed to unmount: \033[1;93m'";
    const std::string cancelSuffix = "'\033[1;91m.\033[0;1m {cxl}";
    
    // Format message based on message type
    std::string format(const std::string& messageType, const std::string& path) {
        // Create a reusable string buffer
        std::string outputBuffer;
        outputBuffer.reserve(256);  // Reserve space for a typical message
        
        if (messageType == "root_error") {
            outputBuffer.append(rootErrorPrefix)
                      .append(path)
                      .append(rootErrorSuffix);
        }
        else if (messageType == "success") {
            outputBuffer.append(successPrefix)
                      .append(path)
                      .append(successSuffix);
        }
        else if (messageType == "error") {
            outputBuffer.append(errorPrefix)
                      .append(path)
                      .append(errorSuffix);
        }
        else if (messageType == "cancel") {
            outputBuffer.append(cancelPrefix)
                      .append(path)
                      .append(cancelSuffix);
        }
        
        return outputBuffer;
    }
};

#endif // UMOUNT_H
