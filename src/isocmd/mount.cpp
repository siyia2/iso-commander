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
    // Temporary containers for verbose messages
    std::vector<std::string> tempMountedFiles;
    std::vector<std::string> tempSkippedMessages;
    std::vector<std::string> tempMountedFails;

    for (const auto& isoFile : isoFiles) {
        if (g_operationCancelled.load(std::memory_order_acquire)) break;

        namespace fs = std::filesystem;
        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");

        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        std::string uniqueId = isoFileName + "~" + shortHash;
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        auto logError = [&](const std::string& errorType) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" 
                         << isoDirectory + "/" + isoFilename
                         << "'\033[0m\033[1;91m.\033[0;1m " << errorType << "\033[0m";
            tempMountedFails.push_back(errorMessage.str());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);  // Ensure atomicity
        };

        if (geteuid() != 0) {
            logError("{needsRoot}");
            continue;
        }

        if (isAlreadyMounted(mountPoint)) {
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename
                           << "'\033[1;93m already mnt@: \033[1;94m'" << mountisoDirectory
                           << "/" << mountisoFilename << "\033[1;94m'\033[1;93m.\033[0m";
            tempSkippedMessages.push_back(skippedMessage.str());

            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            continue;
        }

        if (!fs::exists(isoPath)) {
            logError("{missingISO}");
            continue;
        }

        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                logError(std::string("{dirCreateFail: ") + e.what() + "}");
                continue;
            }
        }

        struct libmnt_context *ctx = mnt_new_context();
        if (!ctx) {
            logError("{mntContextFail}");
            continue;
        }

        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");
        std::string fsTypeList = "iso9660,udf,hfsplus,rockridge,joliet,isofs";
        mnt_context_set_fstype(ctx, fsTypeList.c_str());

        int ret = mnt_context_mount(ctx);
        mnt_free_context(ctx);

        if (ret == 0) {
            std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
                                        + "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
                                        + "\033[1;94m'\033[0;1m.\033[0m";

            tempMountedFiles.push_back(mountedFileInfo);
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
            logError("{badFS}");
            fs::remove(mountPoint);
        }
    }

    // Single lock at the end to update shared resources
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        mountedFiles.insert(tempMountedFiles.begin(), tempMountedFiles.end());
        skippedMessages.insert(tempSkippedMessages.begin(), tempSkippedMessages.end());
        mountedFails.insert(tempMountedFails.begin(), tempMountedFails.end());
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
