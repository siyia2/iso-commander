// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"


// Function to check if a mountpoint isAlreadyMounted
bool isAlreadyMounted(const std::string& mountPoint) {
    // Create a new table and directly find target
    struct libmnt_table* tb = mnt_new_table_from_file("/proc/mounts");
    if (!tb) {
        return false;
    }
    
    // Look for our mount point directly without using a cache
    struct libmnt_fs* fs = mnt_table_find_target(tb, mountPoint.c_str(), MNT_ITER_BACKWARD);
    bool isMounted = (fs != NULL);
    
    // Clean up
    mnt_unref_table(tb);
    
    return isMounted;
}


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks) {
    // Pre-allocate temporary containers with batch capacity
    const size_t BATCH_SIZE = 1000;
    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    tempMountedFiles.reserve(BATCH_SIZE);
    tempSkippedMessages.reserve(BATCH_SIZE);
    tempMountedFails.reserve(BATCH_SIZE);
    
    // Create formatter for verbose output
    VerbosityFormatter formatter;
    
    // Create mount context once
    struct libmnt_context *ctx = mnt_new_context();
    if (!ctx) {
        // Handle context creation failure globally
        std::string errorMsg = "\033[1;91mFailed to create mount context. Cannot proceed with mounting operations.\033[0m";
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFails.insert(errorMsg);
        return;
    }

    // Function to flush temporary buffers to permanent sets
    auto flushBuffers = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
        
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
    };

    // Merged error/skip handling lambda to replace the separate function
    auto handleFailureOrSkip = [&](const std::string& isoDirectory, const std::string& isoFilename, 
                                 const std::string& errorTypeOrMountDir, bool isSkipped = false, 
                                 const std::string& mountisoFilename = "") {
        if (isSkipped) {
            // Handle skipped cases (already mounted)
            // In this case, errorTypeOrMountDir contains the mount directory
            tempSkippedMessages.push_back(formatter.formatSkipped(
                isoDirectory, 
                isoFilename, 
                errorTypeOrMountDir,  // mountisoDirectory 
                mountisoFilename
            ));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            // Handle error cases
            if (errorTypeOrMountDir == "detailedError") {
                tempMountedFails.push_back(formatter.formatDetailedError(isoDirectory, isoFilename, errorTypeOrMountDir));
            } else {
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, errorTypeOrMountDir));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }

        // Check if we need to flush
        if (tempMountedFiles.size() >= BATCH_SIZE || tempSkippedMessages.size() >= BATCH_SIZE || tempMountedFails.size() >= BATCH_SIZE) {
            flushBuffers();
        }
    };

    for (const auto& isoFile : isoFiles) {
        fs::path isoPath(isoFile);

        // Prepare path information
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");
        
        // Check for cancellation
        if (g_operationCancelled.load()) {
            handleFailureOrSkip(isoDirectory, isoFilename, "cxl");
            continue;
        }
        
        // Root privilege check
        if (geteuid() != 0) {
            handleFailureOrSkip(isoDirectory, isoFilename, "needsRoot");
            continue;
        }

        // Generate unique hash for mount point
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        char shortHash[6] = {0};
        for (int i = 0; i < 5; ++i) {
            shortHash[i] = base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        // Create unique mount point
        std::string uniqueId = isoFileName + "~" + shortHash;
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        // Check if already mounted
        if (isAlreadyMounted(mountPoint)) {
            handleFailureOrSkip(isoDirectory, isoFilename, mountisoDirectory, true, mountisoFilename);
            continue;
        }
        
        // Verify ISO file exists
        if (!fs::exists(isoPath)) {
            handleFailureOrSkip(isoDirectory, isoFilename, "missingISO");
            continue;
        }

        // Create mount point directory
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                handleFailureOrSkip(isoDirectory, isoFilename, "detailedError");
                continue;
            }
        }

        // Reuse the mount context
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype(ctx, "iso9660,udf,hfsplus,rockridge,joliet,isofs");

        // Attempt to mount
        int ret = mnt_context_mount(ctx);
        bool mountSuccess = (ret == 0);

        if (mountSuccess) {
            // Get filesystem type if available
            std::string detectedFsType;
            const char* fstype = mnt_context_get_fstype(ctx);
            if (fstype) {
                detectedFsType = fstype;
            }

            // Add successful mount message
            tempMountedFiles.push_back(formatter.formatMountSuccess(
                isoDirectory, isoFilename, 
                mountisoDirectory, mountisoFilename, 
                detectedFsType
            ));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to flush after success
            if (tempMountedFiles.size() >= BATCH_SIZE) {
                flushBuffers();
            }
        } else {
            // Mount failed
            handleFailureOrSkip(isoDirectory, isoFilename, "badFS");
            fs::remove(mountPoint);
        }
    }

    // Free the mount context
    mnt_free_context(ctx);

    // Final flush for any remaining entries
    flushBuffers();
}
