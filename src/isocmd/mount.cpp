// SPDX-License-Identifier: GNU General Public License v3.0 or later

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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::mutex& Mutex4Low) {
    for (const auto& isoFile : isoFiles) {
        namespace fs = std::filesystem;

        fs::path isoPath(isoFile);
        std::string isoFileName = isoPath.stem().string();

        std::hash<std::string> hasher;
        size_t hashValue = hasher(isoFile);

        const std::string base36Chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string shortHash;
        for (int i = 0; i < 5; ++i) {
            shortHash += base36Chars[hashValue % 36];
            hashValue /= 36;
        }

        std::string uniqueId = isoFileName + "\033[38;5;245m~" + shortHash + "\033[0;1m";
        std::string mountPoint = "/mnt/iso_" + uniqueId;

        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);

        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[0m\033[1;91m.\033[0;1m {needsRoot}\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
            continue;
        }

        if (isAlreadyMounted(mountPoint)) {
            std::stringstream skippedMessage;
            skippedMessage << "\033[1;93mISO: \033[1;92m'" << isoDirectory << "/" << isoFilename
                           << "'\033[1;93m already mnt@: \033[1;94m'" << mountisoDirectory
                           << "/" << mountisoFilename << "\033[1;94m'\033[1;93m.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                skippedMessages.insert(skippedMessage.str());
            }
            continue;
        }
        
        // New check: Verify if the ISO file exists
        if (!fs::exists(isoPath)) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory 
                         << "'\033[0m\033[1;91m.\033[0;1m {missingISO}";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
            continue;
        }

        if (!fs::exists(mountPoint)) {
            try {
                fs::create_directory(mountPoint);
            } catch (const fs::filesystem_error& e) {
                std::stringstream errorMessage;
                errorMessage << "\033[1;91mFailed to create mount point: \033[1;93m'" << mountPoint
                             << "'\033[0m\033[1;91m. Error: " << e.what() << "\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    mountedFails.insert(errorMessage.str());
                }
                continue;
            }
        }

        // Create a libmount context
        struct libmnt_context *ctx = mnt_new_context();
        if (!ctx) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to create mount context for: \033[1;93m'" << isoFile << "'\033[0m";
            mountedFails.insert(errorMessage.str());
            continue;
        }

        // Set common mount options
        mnt_context_set_source(ctx, isoFile.c_str());
        mnt_context_set_target(ctx, mountPoint.c_str());
        mnt_context_set_options(ctx, "loop,ro");

        // Set filesystem types to try
        std::string fsTypeList = "iso9660,udf,hfsplus,rockridge,joliet,isofs";
        mnt_context_set_fstype(ctx, fsTypeList.c_str());

        // Attempt to mount
        int ret = mnt_context_mount(ctx);

        bool mountSuccess = (ret == 0);
        std::string successfulFsType;

        if (mountSuccess) {
            // If successful, we can get the used filesystem type
            const char* usedFsType = mnt_context_get_fstype(ctx);
            if (usedFsType) {
                successfulFsType = usedFsType;
            }
        }

        // Clean up the context
        mnt_free_context(ctx);

        if (mountSuccess) {
            std::string mountedFileInfo = "\033[1mISO: \033[1;92m'" + isoDirectory + "/" + isoFilename + "'\033[0m"
                                        + "\033[1m mnt@: \033[1;94m'" + mountisoDirectory + "/" + mountisoFilename
                                        + "\033[1;94m'\033[0;1m.";
            if (successfulFsType != "auto") {
                mountedFileInfo += " {" + successfulFsType + "}";
            }
            mountedFileInfo += "\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFiles.insert(mountedFileInfo);
            }
        } else {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[1;91m.\033[0;1m {badFS}";
            fs::remove(mountPoint);
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                mountedFails.insert(errorMessage.str());
            }
        }
    }
}


// Function to process input and mount ISO files asynchronously
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages, bool& verbose, std::mutex& Mutex4Low) {
    std::vector<int> indicesToProcess; // To store indices parsed from the input
    indicesToProcess.reserve(100); // Reserve space for indices to improve performance
    std::atomic<bool> invalidInput(false); // Flag to track if there's any invalid input
    std::set<std::string> processedRanges; // To keep track of processed ranges
    std::mutex errorMutex; // Mutex to protect access to the error messages

    if (input == "00") {
        // If input is "00", create indices for all ISO files
        indicesToProcess.resize(isoFiles.size());
        std::iota(indicesToProcess.begin(), indicesToProcess.end(), 1); // Fill with 1, 2, 3, ...
    } else {
        // Existing tokenization logic for specific inputs
        tokenizeInput(input, isoFiles, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            std::cout << "\033[1;91mNo valid input provided for mount.\033[0;1m";
            return; // Exit if no valid indices are provided
        }
    }
    
    // Remove duplicate indices and sort them
    std::set<int> uniqueIndices(indicesToProcess.begin(), indicesToProcess.end());
    indicesToProcess.assign(uniqueIndices.begin(), uniqueIndices.end());
    
    std::atomic<size_t> completedTasks(0); // Number of completed tasks
    std::atomic<bool> isProcessingComplete(false); // Flag to indicate processing completion
    unsigned int numThreads = std::min(static_cast<unsigned int>(indicesToProcess.size()), static_cast<unsigned int>(maxThreads));
    ThreadPool pool(numThreads); // Create a thread pool with the determined number of threads
    
    size_t totalTasks = indicesToProcess.size();
    size_t chunkSize = std::max(size_t(1), std::min(size_t(50), (totalTasks + numThreads - 1) / numThreads)); // Determine chunk size for tasks
    
    std::atomic<size_t> activeTaskCount(0); // Track the number of active tasks
    std::condition_variable taskCompletionCV; // Condition variable to notify when all tasks are done
    std::mutex taskCompletionMutex; // Mutex to protect the condition variable

    for (size_t i = 0; i < totalTasks; i += chunkSize) {
        size_t end = std::min(i + chunkSize, totalTasks); // Determine the end index for this chunk
        activeTaskCount.fetch_add(1, std::memory_order_relaxed); // Increment active task count
        // Enqueue a task to the thread pool
        pool.enqueue([&, i, end]() {
            std::vector<std::string> filesToMount;
            filesToMount.reserve(end - i);
            for (size_t j = i; j < end; ++j) {
                filesToMount.push_back(isoFiles[indicesToProcess[j] - 1]); // Collect files for this chunk
            }
            
            mountIsoFiles(filesToMount, mountedFiles, skippedMessages, mountedFails, Mutex4Low); // Mount ISO files            
            completedTasks.fetch_add(end - i, std::memory_order_relaxed); // Update completed tasks count
            // Notify if all tasks are done
            if (activeTaskCount.fetch_sub(1, std::memory_order_release) == 1) {
                taskCompletionCV.notify_one();
            }
        });
    }
    // Create a thread to display progress
    std::thread progressThread(displayProgressBar, std::ref(completedTasks), totalTasks, std::ref(isProcessingComplete), std::ref(verbose));
    // Wait for all tasks to complete
    {
        std::unique_lock<std::mutex> lock(taskCompletionMutex);
        taskCompletionCV.wait(lock, [&]() { return activeTaskCount.load(std::memory_order_acquire) == 0; });
    }
    isProcessingComplete.store(true, std::memory_order_release); // Set processing completion flag
    progressThread.join(); // Wait for the progress thread to finish
}

