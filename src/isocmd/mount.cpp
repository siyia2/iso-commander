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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails) {
	
	std::atomic<bool> g_CancelledMessageAdded{false};

    for (const auto& isoFile : isoFiles) {
		
		// Check for cancellation before processing each ISO
        if (g_operationCancelled) {
            if (!g_CancelledMessageAdded.exchange(true)) {
				mountedFails.clear();
                mountedFails.insert("\033[1;33mMount operation cancelled by user - partial mounts cleaned up.\n\033[0m");
            }
            break;
        }
		
		namespace fs = std::filesystem;
        fs::path isoPath(isoFile);

        // Prepare path and naming information
        std::string isoFileName = isoPath.stem().string();
        auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFile);

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
        auto [mountisoDirectory, mountisoFilename] = extractDirectoryAndFilename(mountPoint);

        // Validation checks with centralized error handling
        auto logError = [&](const std::string& errorType, bool useFullPath = false) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" 
                         << (useFullPath ? isoDirectory : (isoDirectory + "/" + isoFilename))
                         << "'\033[0m\033[1;91m.\033[0;1m " << errorType << "\033[0m";
            
            mountedFails.insert(errorMessage.str());
        };

        // Root privilege check
        if (geteuid() != 0) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to mnt: \033[1;93m'" << isoDirectory << "/" << isoFilename
                         << "'\033[0m\033[1;91m.\033[0;1m {needsRoot}\033[0m";
            {
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
                
                mountedFails.insert(errorMessage.str());
                continue;
            }
        }

        // Create libmount context
        struct libmnt_context *ctx = mnt_new_context();
        if (!ctx) {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to create mount context for: \033[1;93m'" << isoFile << "'\033[0m";
            
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
    std::set<int> indicesToProcess; // To store indices parsed from the input
   
   // Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
        
    // Reset cancellation flag
    g_operationCancelled = false;
    
    
    if (input == "00") {
        // If input is "00", create indices for all ISO files
        for (size_t i = 0; i < isoFiles.size(); ++i) {
		indicesToProcess.insert(i + 1);  // Insert elements 1, 2, 3, ...
		}
    } else {
        // Existing tokenization logic for specific inputs
        tokenizeInput(input, isoFiles, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            std::cout << "\033[1;91mNo valid input provided for mount.\033[0;1m";
            return; // Exit if no valid indices are provided
        }
    }
    
    std::cout << "\n\033[0;1m Processing \033[1;92mmount\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";
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
        
			// Use an iterator to iterate over the set
			auto it = std::next(indicesToProcess.begin(), i); // Get the iterator to the i-th element in the set
			for (size_t j = i; j < end; ++j) {
				int index = *it; // Dereference the iterator to get the value in the set
				filesToMount.push_back(isoFiles[index - 1]); // Collect files for this chunk
            
				++it; // Move the iterator to the next element
			}
        
			mountIsoFiles(filesToMount, mountedFiles, skippedMessages, mountedFails); // Mount ISO files        
			completedTasks.fetch_add(end - i, std::memory_order_relaxed); // Update completed tasks count
        
			// Notify if all tasks are done
			if (activeTaskCount.fetch_sub(1, std::memory_order_release) == 1) {
				taskCompletionCV.notify_one();
			}
		});
	}

    // Create a thread to display progress
	std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,       // Pass as raw pointer
        static_cast<size_t>(0), // Pass as size_t
        &completedTasks,       // Pass as raw pointer
        totalTasks,            // Pass as size_t
        &isProcessingComplete, // Pass as raw pointer
        &verbose               // Pass as raw pointer
    );
    // Wait for all tasks to complete
    {
        std::unique_lock<std::mutex> lock(taskCompletionMutex);
        taskCompletionCV.wait(lock, [&]() { return activeTaskCount.load(std::memory_order_acquire) == 0; });
    }
    isProcessingComplete.store(true, std::memory_order_release); // Set processing completion flag
    progressThread.join(); // Wait for the progress thread to finish
}

