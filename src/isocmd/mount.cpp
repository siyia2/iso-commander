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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {

    for (const auto& isoFile : isoFiles) {
        // Check for cancellation before processing each ISO
        if (g_operationCancelled.load()) {
            break;
        }

        namespace fs = std::filesystem;
        fs::path isoPath(isoFile);

        // Prepare path and naming information
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile, "mount");

        // Generate unique hash for mount point
        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);
        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        // Create unique mount point and identifiers
        std::string uniqueId = isoFileName + "~" + shortHash;
        std::string mountPoint = "/mnt/iso_" + uniqueId;
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint, "mount");

        // Validation checks with centralized error handling
        auto logError = [&](const std::string& errorType, bool useFullPath = false) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" 
                         << (useFullPath ? isoDirectory : (isoDirectory + "/" + isoFilename))
                         << "'\033[0m\033[1;91m.\033[0;1m " << errorType << "\033[0m";
            
            std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
            mountedFails.insert(errorMessage.str());
        };

        // Root privilege check
        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[0m\033[1;91m.\033[0;1m {needsRoot}\033[0m";
            {
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
                mountedFails.insert(errorMessage.str());
            }
            continue;
        }

        // Check if already mounted
        if (isAlreadyMounted(mountPoint)) {
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename
                           << "'\033[1;93m already mnt@: \033[1;94m'" << mountisoDirectory
                           << "/" << mountisoFilename << "\033[1;94m'\033[1;93m.\033[0m";
            
            std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
            skippedMessages.insert(skippedMessage.str());
            continue;
        }
        
        // Verify ISO file exists
        if (!fs::exists(isoPath)) {
            logError("{missingISO}", true);
            continue;
        }

        // Create mount point directory if it doesn't exist
        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to create mount point: \033[1;93m'" << mountPoint
                             << "'\033[0m\033[1;91m. Error: " << e.what() << "\033[0m";
                
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
                mountedFails.insert(errorMessage.str());
                continue;
            }
        }

        // Create libmount context
        struct libmnt_context *ctx = mnt_new_context();
        if (!ctx) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to create mount context for: \033[1;93m'" << isoFile << "'\033[0m";
            
            std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
            mountedFails.insert(errorMessage.str());
            continue;
        }

        // Configure mount options
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");

        // Set filesystem types to try
        std::string fsTypeList = "iso9660,udf,hfsplus,rockridge,joliet,isofs";
        mnt_context_set_fstype(ctx, fsTypeList.c_str());

        // Attempt to mount
        int ret = mnt_context_mount(ctx);
        bool mountSuccess = (ret == 0);

        // Cleanup mount context
        mnt_free_context(ctx);

        // Filesystem type detection
        std::string detectedFsType;
        if (mountSuccess) {
            struct stat st;
            if (stat(mountPoint.c_str(), &st) == 0) {
                FILE* mountFile = fopen("/proc/mounts", "r");
                if (mountFile) {
                    char line[1024];
                    while (fgets(line, sizeof(line), mountFile)) {
                        char sourcePath[PATH_MAX];
                        char mountPath[PATH_MAX];
                        char fsType[256];
                        
                        // Parse /proc/mounts line
                        if (sscanf(line, "%s %s %s", sourcePath, mountPath, fsType) == 3) {
                            if (strcmp(mountPath, mountPoint.c_str()) == 0) {
                                detectedFsType = fsType;
                                break;
                            }
                        }
                    }
                    fclose(mountFile);
                }
            }

            // Prepare mounted file information
            std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
                                        + "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
                                        + "\033[1;94m'\033[0;1m.";
            
            // Add filesystem type if detected
            if (!detectedFsType.empty()) {
                mountedFileInfo += " {" + detectedFsType + "}";
            }
            
            mountedFileInfo += "\033[0m";

            // Thread-safe insertion of mounted file info
            {
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Lock the mutex
                mountedFiles.insert(mountedFileInfo);
            }
        } else {
            // Mount failed
            logError("{badFS}");
            fs::remove(mountPoint);
        }
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages, bool& verbose) {
    std::set<int> indicesToProcess;
    
    // Setup signal handler
    setupSignalHandlerCancellations();

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
    std::atomic<bool> isProcessingComplete(false);

    // Enqueue chunk tasks
    for (const auto& chunk : isoChunks) {
        mountFutures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            mountIsoFiles(chunk, mountedFiles, skippedMessages, mountedFails);
            completedTasks.fetch_add(chunk.size(), std::memory_order_relaxed);
        }));
    }

    // Start progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
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
