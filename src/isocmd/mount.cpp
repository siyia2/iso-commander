// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Function to check if a mountpoint isAlreadyMounted
bool isAlreadyMounted(const std::string& mountPoint) {
    struct statvfs vfs;
    if (statvfs(mountPoint.c_str(), &vfs) != 0) {
        return false; // Error or doesn't exist
    }

    // Check if it's a mount point
    return (vfs.f_flag & ST_NODEV) == 0;
}


// Function to mount selected ISO files called from processAndMountIsoFiles
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks) {

    // Pre-allocate temporary containers with batch capacity
    const size_t BATCH_SIZE = 1000;
    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;
    tempMountedFiles.reserve(BATCH_SIZE);
    tempSkippedMessages.reserve(BATCH_SIZE);
    tempMountedFails.reserve(BATCH_SIZE);

    // Track total processed entries for batch insertions
    size_t totalProcessedEntries = 0;

    // Pre-calculate mount info format strings to avoid repeated concatenations
    const std::string mountedFormatPrefix = "\033[1mISO: \033[1;92m'";
    const std::string mountedFormatMiddle = "'\033[0m\033[1m mnt@: \033[1;94m'";
    const std::string mountedFormatSuffix = "\033[1;94m'\033[0;1m.";
    const std::string mountedFormatSuffixWithFS = "\033[1;94m'\033[0;1m. {";
    const std::string mountedFormatEnd = "\033[0m";

    const std::string errorFormatPrefix = "\033[1;91mFailed to mnt: \033[1;93m'";
    const std::string errorFormatSuffix = "'\033[0m\033[1;91m.\033[0;1m ";
    const std::string errorFormatEnd = "\033[0m";

    const std::string skippedFormatPrefix = "\033[1;93mISO: \033[1;92m'";
    const std::string skippedFormatMiddle = "'\033[1;93m already mnt@: \033[1;94m'";
    const std::string skippedFormatSuffix = "\033[1;94m'\033[1;93m.\033[0m";

    // Reuse mount context if possible
    struct libmnt_context *ctx = mnt_new_context();
    if (!ctx) {
        // Handle context creation failure globally
        std::string errorMsg = "\033[1;91mFailed to create mount context. Cannot proceed with mounting operations.\033[0m";
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFails.insert(errorMsg);
        return;
    }

    // Create a single string buffer to reuse for formatting
    std::string outputBuffer;
    outputBuffer.reserve(512);  // Reserve space for a typical message

    // Function to flush temporary vectors to sets
    auto flushTemporaryBuffers = [&]() {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
        
        // Clear temporary vectors but maintain capacity
        tempMountedFiles.clear();
        tempSkippedMessages.clear();
        tempMountedFails.clear();
        
        // Reset counter after flush
        totalProcessedEntries = 0;
    };

    namespace fs = std::filesystem;
    for (const auto& isoFile : isoFiles) {
        // Check for cancellation before processing each ISO
        if (g_operationCancelled.load()) break;

        fs::path isoPath(isoFile);

        // Prepare path and naming information - minimize string operations
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");

        // Generate unique hash for mount point
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        char shortHash[6] = {0};  // Use char array instead of string concatenation
        for (int i = 0; i < 5; ++i) {
            shortHash[i] = base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        // Create unique mount point and identifiers - minimize allocations
        std::string uniqueId = isoFileName + "~" + shortHash;
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        // Root privilege check
        if (geteuid() != 0) {
            outputBuffer.clear();
            outputBuffer.append(errorFormatPrefix)
                       .append(isoDirectory).append("/").append(isoFilename)
                       .append(errorFormatSuffix).append("{needsRoot}")
                       .append(errorFormatEnd);
            tempMountedFails.push_back(outputBuffer);
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            totalProcessedEntries++;
            
            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
            continue;
        }

        // Check if already mounted
        if (isAlreadyMounted(mountPoint)) {
            outputBuffer.clear();
            outputBuffer.append(skippedFormatPrefix)
                       .append(isoDirectory).append("/").append(isoFilename)
                       .append(skippedFormatMiddle)
                       .append(mountisoDirectory).append("/").append(mountisoFilename)
                       .append(skippedFormatSuffix);
            tempSkippedMessages.push_back(outputBuffer);
            // Already mounted is considered a successful state
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            totalProcessedEntries++;
            
            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
            continue;
        }
        
        // Verify ISO file exists
        if (!fs::exists(isoPath)) {
            outputBuffer.clear();
            outputBuffer.append(errorFormatPrefix)
                       .append(isoDirectory).append("/").append(isoFilename)
                       .append(errorFormatSuffix).append("{missingISO}")
                       .append(errorFormatEnd);
            tempMountedFails.push_back(outputBuffer);
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            totalProcessedEntries++;
            
            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
            continue;
        }

        // Create mount point directory if it doesn't exist
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                outputBuffer.clear();
                outputBuffer.append(errorFormatPrefix)
                           .append(isoDirectory).append("/").append(isoFilename)
                           .append(errorFormatSuffix).append("Failed to create mount point: ")
                           .append(e.what()).append(errorFormatEnd);
                tempMountedFails.push_back(outputBuffer);
                failedTasks->fetch_add(1, std::memory_order_acq_rel);
                totalProcessedEntries++;
                
                // Check if we need to flush
                if (totalProcessedEntries >= BATCH_SIZE) {
                    flushTemporaryBuffers();
                }
                continue;
            }
        }

        // Reuse the mount context instead of creating a new one each time
        mnt_reset_context(ctx);
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");

        // Set filesystem types to try
        mnt_context_set_fstype(ctx, "iso9660,udf,hfsplus,rockridge,joliet,isofs");

        // Attempt to mount
        int ret = mnt_context_mount(ctx);
        bool mountSuccess = (ret == 0);

        // Filesystem type detection
        std::string detectedFsType;
        if (mountSuccess) {
            struct stat st;
            if (stat(mountPoint.c_str(), &st) == 0) {
                // Reuse the context's filesystem type if available instead of parsing /proc/mounts
                const char* fstype = mnt_context_get_fstype(ctx);
                if (fstype) {
                    detectedFsType = fstype;
                } 
            }

            // Prepare mounted file information with minimal allocations
            outputBuffer.clear();
            outputBuffer.append(mountedFormatPrefix)
                       .append(isoDirectory).append("/").append(isoFilename)
                       .append(mountedFormatMiddle)
                       .append(mountisoDirectory).append("/").append(mountisoFilename);
            
            // Add filesystem type if detected
            if (!detectedFsType.empty()) {
                outputBuffer.append(mountedFormatSuffixWithFS)
                           .append(detectedFsType)
                           .append("}")
                           .append(mountedFormatEnd);
            } else {
                outputBuffer.append(mountedFormatSuffix)
                           .append(mountedFormatEnd);
            }
            
            tempMountedFiles.push_back(outputBuffer);
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            totalProcessedEntries++;
            
            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
        } else {
            // Mount failed
            outputBuffer.clear();
            outputBuffer.append(errorFormatPrefix)
                       .append(isoDirectory).append("/").append(isoFilename)
                       .append(errorFormatSuffix).append("{badFS}")
                       .append(errorFormatEnd);
            tempMountedFails.push_back(outputBuffer);
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            fs::remove(mountPoint);
            totalProcessedEntries++;
            
            // Check if we need to flush
            if (totalProcessedEntries >= BATCH_SIZE) {
                flushTemporaryBuffers();
            }
        }
    }

    // Free the mount context
    mnt_free_context(ctx);

    // Final flush for any remaining entries
    if (totalProcessedEntries > 0) {
        flushTemporaryBuffers();
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages, bool& verbose) {
    std::set<int> indicesToProcess;
    
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
