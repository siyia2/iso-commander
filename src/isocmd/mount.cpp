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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode) {
    // Create mount context once
    struct libmnt_context *ctx = mnt_new_context();
    if (!ctx) {
        if (!silentMode) {
            std::string errorMsg = "\033[1;91mFailed to create mount context. Cannot proceed with mounting operations.\033[0m";
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            mountedFails.insert(errorMsg);
        }
        return;
    }

    // Only allocate buffers and formatter if not in quiet mode
    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    VerbosityFormatter formatter;
    
    if (!silentMode) {
        // Pre-allocate temporary containers with batch capacity
        const size_t BATCH_SIZE = 1000;
        tempMountedFiles.reserve(BATCH_SIZE);
        tempSkippedMessages.reserve(BATCH_SIZE);
        tempMountedFails.reserve(BATCH_SIZE);
    }

    // Function to flush temporary buffers (only in verbose mode)
    auto flushBuffers = [&]() {
        if (silentMode) return;
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
    };

    for (const auto& isoFile : isoFiles) {
        fs::path isoPath(isoFile);
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");

        // Check for cancellation
        if (g_operationCancelled.load()) {
            if (!silentMode) {
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "cxl"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }
        
        // Root privilege check
        if (geteuid() != 0) {
            if (!silentMode) {
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "needsRoot"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Generate unique mount point
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        char shortHash[6] = {0};
        for (int i = 0; i < 5; ++i) {
            shortHash[i] = base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        std::string uniqueId = isoPath.stem().string() + "~" + shortHash;
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        // Check if already mounted
        if (isAlreadyMounted(mountPoint)) {
            if (!silentMode) {
                tempSkippedMessages.push_back(formatter.formatSkipped(
                    isoDirectory, isoFilename, mountisoDirectory, mountisoFilename
                ));
            }
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }
        
        // Verify ISO file exists
        if (!fs::exists(isoPath)) {
            if (!silentMode) {
                tempMountedFails.push_back(formatter.formatError(isoDirectory, isoFilename, "missingISO"));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        // Create mount point directory
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error&) {
                if (!silentMode) {
                    tempMountedFails.push_back(formatter.formatDetailedError(
                        isoDirectory, isoFilename, "Failed to create mount directory"
                    ));
                }
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                continue;
            }
        }

        // Configure mount context
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        mnt_context_set_fstype(ctx, "iso9660,udf,hfsplus,rockridge,joliet,isofs");

        // Attempt to mount
        int ret = mnt_context_mount(ctx);
        if (ret == 0) {
            if (!silentMode) {
                // Get filesystem type if available
                std::string detectedFsType;
                if (const char* fstype = mnt_context_get_fstype(ctx)) {
                    detectedFsType = fstype;
                }
                tempMountedFiles.push_back(formatter.formatMountSuccess(
                    isoDirectory, isoFilename, 
                    mountisoDirectory, mountisoFilename, 
                    detectedFsType
                ));
            }
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            if (!silentMode) {
                tempMountedFails.push_back(formatter.formatError(
                    isoDirectory, isoFilename, "badFS"
                ));
            }
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            fs::remove(mountPoint);
        }

        // Batch processing check (only relevant in verbose mode)
        if (!silentMode && 
            (tempMountedFiles.size() >= 1000 || 
             tempSkippedMessages.size() >= 1000 || 
             tempMountedFails.size() >= 1000)) {
            flushBuffers();
        }
    }

    // Free the mount context
    mnt_free_context(ctx);

    // Final flush if needed
    flushBuffers();
}
