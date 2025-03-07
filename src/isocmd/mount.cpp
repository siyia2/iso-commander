// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../mount.h"


// Function to check if a mountpoint isAlreadyMounted
bool isAlreadyMounted(const std::string& mountPoint) {
    struct statvfs vfs;
    if (statvfs(mountPoint.c_str(), &vfs) != 0) {
        return false; // Error or doesn't exist
    }

    // Check if it's a mount point
    return (vfs.f_flag & ST_NODEV) == 0;
}


// New helper function to handle error cases that lead to continue
bool handleMountFailure(const std::string& isoDirectory, const std::string& isoFilename, const std::string& errorTypeOrMountDir, VerbosityFormatter& formatter, std::vector<std::string>& tempMountedFails, std::vector<std::string>& tempSkippedMessages, std::atomic<size_t>* failedTasks, std::atomic<size_t>* completedTasks, size_t& totalProcessedEntries, const std::function<void()>& flushFunction, const size_t BATCH_SIZE, bool isSkipped = false, const std::string& mountisoFilename = "") {

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

    // Increment counter and check if we need to flush
    totalProcessedEntries++;
    if (totalProcessedEntries >= BATCH_SIZE) {
        flushFunction();
    }

    return true; // Indicates we should continue to the next iteration
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

    // Track total processed entries for batch flushing
    size_t totalProcessedEntries = 0;

    // Function to flush temporary buffers to permanent sets
    auto flushBuffers = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
        
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
        totalProcessedEntries = 0;
    };

    for (const auto& isoFile : isoFiles) {
        fs::path isoPath(isoFile);

        // Prepare path information
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");
        
        // Check for cancellation
        if (g_operationCancelled.load()) {
            if (handleMountFailure(isoDirectory, isoFilename, "CXL", formatter, tempMountedFails, 
                                  tempSkippedMessages, failedTasks, completedTasks, 
                                  totalProcessedEntries, flushBuffers, BATCH_SIZE)) {
                continue;
            }
        }
        
        // Root privilege check
        if (geteuid() != 0) {
            if (handleMountFailure(isoDirectory, isoFilename, "needsRoot", formatter, tempMountedFails, 
                                  tempSkippedMessages, failedTasks, completedTasks, 
                                  totalProcessedEntries, flushBuffers, BATCH_SIZE)) {
                continue;
            }
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
            if (handleMountFailure(isoDirectory, isoFilename, mountisoDirectory, formatter, tempMountedFails, 
                                  tempSkippedMessages, failedTasks, completedTasks, 
                                  totalProcessedEntries, flushBuffers, BATCH_SIZE, true, mountisoFilename)) {
                continue;
            }
        }
        
        // Verify ISO file exists
        if (!fs::exists(isoPath)) {
            if (handleMountFailure(isoDirectory, isoFilename, "missingISO", formatter, tempMountedFails, 
                                  tempSkippedMessages, failedTasks, completedTasks, 
                                  totalProcessedEntries, flushBuffers, BATCH_SIZE)) {
                continue;
            }
        }

        // Create mount point directory
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                std::string errorMessage = "detailedError";
                if (handleMountFailure(isoDirectory, isoFilename, errorMessage, formatter, tempMountedFails, 
                                      tempSkippedMessages, failedTasks, completedTasks, 
                                      totalProcessedEntries, flushBuffers, BATCH_SIZE)) {
                    continue;
                }
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
            
            // Increment counter and check if we need to flush
            totalProcessedEntries++;
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushBuffers();
            }
        } else {
            // Mount failed
            if (handleMountFailure(isoDirectory, isoFilename, "badFS", formatter, tempMountedFails, 
                                  tempSkippedMessages, failedTasks, completedTasks, 
                                  totalProcessedEntries, flushBuffers, BATCH_SIZE)) {
                fs::remove(mountPoint);
                continue;
            }
        }
    }

    // Free the mount context
    mnt_free_context(ctx);

    // Final flush for any remaining entries
    flushBuffers();
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& verbose) {
    std::unordered_set<int> indicesToProcess;
    
    // Setup signal handler
    setupSignalHandlerCancellations();
    
    g_operationCancelled.store(false);

    // Handle input ("00" = all files, else parse input)
    if (input == "00") {
        for (size_t i = 0; i < isoFiles.size(); ++i)
            indicesToProcess.insert(i + 1);
    } else {
        tokenizeInput(input, isoFiles, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            return;
        }
    }

    // Create ISO paths vector from selected indices
    std::vector<std::string> selectedIsoFiles;
    selectedIsoFiles.reserve(indicesToProcess.size());
    for (int index : indicesToProcess)
        selectedIsoFiles.push_back(isoFiles[index - 1]);

    std::cout << "\n\033[0;1m Processing \033[1;92mmount\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";

    // Thread pool and task setup
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedIsoFiles.size()), maxThreads);
    const size_t chunkSize = std::min(size_t(100), selectedIsoFiles.size()/numThreads + 1);
    std::vector<std::vector<std::string>> isoChunks;

    // Split work into chunks
    for (size_t i = 0; i < selectedIsoFiles.size(); i += chunkSize) {
        auto end = std::min(selectedIsoFiles.begin() + i + chunkSize, selectedIsoFiles.end());
        isoChunks.emplace_back(selectedIsoFiles.begin() + i, end);
    }

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> mountFutures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Enqueue chunk tasks
    for (const auto& chunk : isoChunks) {
        mountFutures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            mountIsoFiles(chunk, mountedFiles, skippedMessages, mountedFails, &completedTasks, &failedTasks);
        }));
    }

    // Start progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
        &failedTasks,
        selectedIsoFiles.size(),
        &isProcessingComplete,
        &verbose
    );

    // Wait for completion or cancellation
    for (auto& future : mountFutures) {
        future.wait();
        if (g_operationCancelled.load()) break;
    }

    // Cleanup
    isProcessingComplete.store(true);
    progressThread.join();
}
