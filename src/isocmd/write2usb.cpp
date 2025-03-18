// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../write.h"
#include "../readline.h"


// Shared progress data
std::vector<ProgressInfo> progressData;


// Function to get the size of a block device
uint64_t getBlockDeviceSize(const std::string& device) {
    // Open the device
    int fd = open(device.c_str(), O_RDONLY);
    if (fd == -1) {
        return 0;
    }

    // Try multiple approaches to get the size
    uint64_t size = 0;
    
    // First try BLKGETSIZE64 - the modern way to get size in bytes
    if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
        close(fd);
        return size;
    }
    
    // If BLKGETSIZE64 fails, try BLKGETSIZE (returns number of 512-byte sectors)
    unsigned long sectors = 0;
    if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
        close(fd);
        return sectors * 512ULL;
    }
    
    close(fd);
    return 0;
}


// Function to format fileSize
std::string formatFileSize(uint64_t size) {
    std::ostringstream oss;
    if (size < 1024 * 1024) {
        oss << std::fixed << std::setprecision(2) 
            << static_cast<double>(size) / 1024 << " KB";
    } else if (size < 1024 * 1024 * 1024) {
        oss << std::fixed << std::setprecision(2) 
            << static_cast<double>(size) / (1024 * 1024) << " MB";
    } else {
        oss << std::fixed << std::setprecision(2) 
            << static_cast<double>(size) / (1024 * 1024 * 1024) << " GB";
    }
    return oss.str();
}


// Function to format write speed
std::string formatSpeed(double mbPerSec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (mbPerSec < 0.1) {
        oss << (mbPerSec * 1024) << " KB/s";
    } else {
        oss << mbPerSec << " MB/s";
    }
    return oss.str();
}


// Function to get removable drive names
std::string getDriveName(const std::string& device) {
    // Extract device name (e.g., sdc from /dev/sdc)
    std::string deviceName = device.substr(device.find_last_of('/') + 1);
    std::string sysfsPath = "/sys/block/" + deviceName + "/device/model";
    
    std::ifstream modelFile(sysfsPath);
    std::string driveName;
    
    if (modelFile.is_open()) {
        std::getline(modelFile, driveName);
        // Trim whitespace
        driveName.erase(0, driveName.find_first_not_of(" \t"));
        driveName.erase(driveName.find_last_not_of(" \t") + 1);
    }
    
    return driveName.empty() ? "Unknown Drive" : driveName;
}


// Get removable drives to display in selection
std::vector<std::string> getRemovableDevices() {
    std::vector<std::string> devices;    
    try {
        for (const auto& entry : fs::directory_iterator("/sys/block")) {
            std::string deviceName = entry.path().filename();
            
            // Skip virtual devices, loopbacks, and drives with numbers like /dev/sr0
            if (deviceName.find("loop") == 0 || 
                deviceName.find("ram") == 0 ||
                deviceName.find("zram") == 0) {
                continue;
            }

            // Check if the device name contains any numeric characters
            bool hasNumber = false;
            for (char ch : deviceName) {
                if (std::isdigit(ch)) {
                    hasNumber = true;
                    break;
                }
            }
            if (hasNumber) {
                continue; // Skip devices with numbers (e.g., sr0, sr1)
            }

            // Check removable status
            std::ifstream removableFile(entry.path() / "removable");
            std::string removable;
            if (removableFile >> removable && removable == "1") {
                devices.push_back("/dev/" + deviceName);
            }
        }
    } catch (const fs::filesystem_error& e) {
        // Handle filesystem error if needed
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }
    
    return devices;
}


// Function used for pair (ISO>DEVICE) validation
std::vector<std::pair<IsoInfo, std::string>> validateDevices(const std::vector<std::pair<size_t, std::string>>& deviceMap, const std::vector<IsoInfo>& selectedIsos, bool& permissions) {
    
    std::vector<std::string> validationErrors;
    std::vector<std::pair<IsoInfo, std::string>> validPairs;
    
    for (const auto& devicePair : deviceMap) {
        size_t index = devicePair.first;
        const std::string& device = devicePair.second;
        const auto& iso = selectedIsos[index - 1];
        
        // Get device size before other checks
        uint64_t deviceSize = getBlockDeviceSize(device);
        std::string deviceSizeStr = formatFileSize(deviceSize);
        std::string driveName = getDriveName(device);
        
        if (!isUsbDevice(device)) {
            validationErrors.push_back("\033[1;93m'" + device + "'\033[0;1m is not a removable USB device");
            continue;
        }
        
        if (isDeviceMounted(device)) {
            validationErrors.push_back("\033[1;93m'" + device + "'\033[0;1m or its partitions are mounted");
            continue;
        }
        
        if (deviceSize == 0) {
            validationErrors.push_back("\033[0;1mFailed to get size for \033[1;93m'" + device + "'\033[0;1m check permissions ");
            permissions = true;
            continue;
        }
        
        if (iso.size > deviceSize) {
            validationErrors.push_back("\033[1;92m'" + iso.filename + "'\033[0;1m (\033[1;95m" + iso.sizeStr + "\033[0;1m) is too large for \033[1;93m'" + 
                device + " <" + driveName  +">' \033[0;1m(\033[1;95m" + deviceSizeStr + "\033[0;1m)");
            continue;
        }
        
        validPairs.emplace_back(iso, device);
    }
    
    if (!validationErrors.empty()) {
        std::cerr << "\n\033[1;91mValidation errors:\033[0;1m\n";
        for (const auto& err : validationErrors) {
            std::cerr << "  • " << err << "\033[0;1m\n";
        }
        
        signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
		disable_ctrl_d();
		std::cout << "\n\033[1;92m↵ to " << (!permissions ? "try again..." : "continue...") << "\033[0;1m";
		if (permissions) permissions = false;
        
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        // Return an empty vector if any validation errors were found
        return {};
    }
    
    return validPairs;
}


// Function to parse device mappings for errors
std::vector<std::pair<size_t, std::string>> parseDeviceMappings(const std::string& pairString, const std::vector<std::string>& selectedIsos, std::vector<std::string>& errors) {
    
    std::vector<std::pair<size_t, std::string>> deviceMap;
    std::unordered_set<std::string> usedDevices;
    std::istringstream pairStream(pairString);
    std::string pair;
    
    errors.clear();
    
    while (std::getline(pairStream, pair, ';')) {
        // Trim whitespace
        pair.erase(pair.find_last_not_of(" \t\n\r\f\v") + 1);
        pair.erase(0, pair.find_first_not_of(" \t\n\r\f\v"));
        if (pair.empty()) continue;
        
        size_t sepPos = pair.find('>');
        if (sepPos == std::string::npos) {
            errors.push_back("Invalid pair format: '" + pair + "'");
            continue;
        }
        
        std::string indexStr = pair.substr(0, sepPos);
        std::string device = pair.substr(sepPos + 1);
        
        try {
            size_t index = std::stoul(indexStr);
            if (index < 1 || index > selectedIsos.size()) {
                errors.push_back("Invalid index " + indexStr);
                continue;
            }
            
            if (usedDevices.count(device)) {
                errors.push_back("Device " + device + " used multiple times");
                continue;
            }
            
            deviceMap.emplace_back(index, device);
            usedDevices.insert(device);
        } catch (...) {
            errors.push_back("Invalid index: '" + indexStr + "'");
        }
    }
    
    // Validate all selected ISOs have at least one mapping
    std::unordered_set<size_t> mappedIndices;
    for (const auto& [index, device] : deviceMap) {
        mappedIndices.insert(index);
    }
    
    for (size_t i = 1; i <= selectedIsos.size(); ++i) {
        if (mappedIndices.find(i) == mappedIndices.end()) {
            errors.push_back("Missing mapping for ISO " + std::to_string(i));
        }
    }
    
    return deviceMap;
}

// Function to display selectedIsos and devices for write
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos, std::unordered_set<std::string>& uniqueErrorMessages) {
    // Helper function to set up readline
    auto setupReadline = []() {
        // Disable readline completion list display for more than one items
        rl_completion_display_matches_hook = [](char **matches, int num_matches, int max_length) {
            // Mark parameters as unused to suppress warnings
            (void)matches;
            (void)num_matches;
            (void)max_length;
            // Do nothing so no list is printed
        };
        
        rl_attempted_completion_function = completion_cb;
        rl_bind_key('\t', rl_complete);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_keyseq("\033[A", rl_get_previous_history);
        rl_bind_keyseq("\033[B", rl_get_next_history);
    };
    
    

    while (true) {
        setupReadline();
        signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
        disable_ctrl_d();
        clearScrollBuffer();
        
        if ((selectedIsos.size() > ITEMS_PER_PAGE) && !(ITEMS_PER_PAGE <= 0)) {
			std::cout << "\n\033[1;91mISO selections for \033[1;93mwrite\033[1;91m cannot exceed the current pagination limit of \033[1;93m" << ITEMS_PER_PAGE << "\033[1;91m!\033[0;1m\n";
			std::cout << "\n\033[1;92m↵ to try again...\033[0;1m";
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			return {};
		}

        displayErrors(uniqueErrorMessages);

        // Sort ISOs by size (descending)
        std::vector<IsoInfo> sortedIsos = selectedIsos;
        std::sort(sortedIsos.begin(), sortedIsos.end(), [](const IsoInfo& a, const IsoInfo& b) {
            return a.size > b.size; // Compare numeric sizes directly
        });

        // Build device prompt with sorted ISOs
        std::ostringstream devicePromptStream;
        devicePromptStream << "\n\033[0;1mSelected \033[1;92mISO\033[0;1m:\n\n";
        for (size_t i = 0; i < sortedIsos.size(); ++i) {
            auto [shortDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write");
            devicePromptStream << "  \033[1;93m" << (i+1) << ">\033[0;1m " 
                               << shortDir << "/\033[1;95m" << filename 
                               << "\033[0;1m (\033[1;35m" << sortedIsos[i].sizeStr 
                               << "\033[0;1m)\n";
        }

        // Process and sort USB devices by capacity
        devicePromptStream << "\n\033[0;1mRemovable USB Devices:\033[0;1m\n\n";
        std::vector<std::string> usbDevices = getRemovableDevices();
        
        // Struct to hold device information
        struct DeviceInfo {
            std::string path;
            uint64_t size;
            std::string driveName;
            std::string sizeStr;
            bool mounted;
            bool error;
        };

        std::vector<DeviceInfo> deviceInfos;
        for (const auto& device : usbDevices) {
            try {
                std::string driveName = getDriveName(device);
                uint64_t deviceSize = getBlockDeviceSize(device);
                std::string sizeStr = formatFileSize(deviceSize);
                bool mounted = isDeviceMounted(device);
                deviceInfos.push_back({device, deviceSize, driveName, sizeStr, mounted, false});
            } catch (...) {
                deviceInfos.push_back({device, 0, "", "", false, true});
            }
        }

        // Sort devices by capacity (descending), errors last
        std::sort(deviceInfos.begin(), deviceInfos.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
            if (a.error != b.error) return !a.error; // Non-errors first
            return a.size > b.size; // Descending order by size
        });

        if (deviceInfos.empty()) {
            devicePromptStream << "  \033[1;91mNo removable USB devices detected!\033[0;1m\n";
        } else {
            for (const auto& dev : deviceInfos) {
                if (dev.error) {
                    devicePromptStream << "  \033[1;91m" << dev.path << " (error)\033[0;1m\n";
                } else {
                    devicePromptStream << "  \033[1;93m" << dev.path 
                                       << "\033[0;1m <" << dev.driveName 
                                       << "> (\033[1;35m" << dev.sizeStr 
                                       << "\033[0;1m)"
                                       << (dev.mounted ? " \033[1;91m(mounted)\033[0;1m" : "") 
                                       << "\n";
                }
            }
        }

        // Prepare completion data using sorted ISO list
        g_completerData.sortedIsos = &sortedIsos;
        g_completerData.usbDevices = &usbDevices;

        // Finalize prompt with usage instructions
        devicePromptStream << "\n\001\033[1;92m\002Mappings\001\033[1;94m\002 ↵ as \001\033[1;93m\002INDEX>DEVICE\001\033[1;94m\002, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";
        std::string devicePrompt = devicePromptStream.str();

        // Get user input
        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );
        
        // Handle empty input
        if (!deviceInput || deviceInput.get()[0] == '\0') {
            restoreReadline();
            return {};
        }
        
        // Process input
        std::string mainInputString(deviceInput.get());
        if (mainInputString == "?") {
            helpMappings();
            continue;
        }
        
        if (deviceInput && *deviceInput) add_history(deviceInput.get());

        // Parse mappings using sorted ISO list
        std::vector<std::string> errors;
        std::vector<std::string> isoFilenames;
        for (const auto& iso : sortedIsos) {
            isoFilenames.push_back(iso.path);
        }
        
        auto deviceMap = parseDeviceMappings(deviceInput.get(), isoFilenames, errors);

        // Handle parsing errors
        if (!errors.empty()) {
            std::cerr << "\n\033[1;91mErrors:\033[0;1m\n";
            for (const auto& err : errors) {
                std::cerr << "  • " << err << "\n";
            }
            
            std::cout << "\n\033[1;92m↵ to try again...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        // Validate device mappings using sortedIsos so the mapping indices match what was displayed
        bool permissions = false;
        auto validPairs = validateDevices(deviceMap, sortedIsos, permissions);
        if (validPairs.empty()) {
            continue;
        }

        // Final confirmation dialog
        std::cout << "\n\033[1;93mWARNING: This will \033[1;91m*ERASE ALL DATA*\033[1;93m on:\033[0;1m\n\n";
        for (const auto& [iso, device] : validPairs) {
            uint64_t deviceSize = getBlockDeviceSize(device);
            std::string deviceSizeStr = formatFileSize(deviceSize);
            std::string driveName = getDriveName(device);
            
            std::cout << "  {\033[1;93m" << device << " \033[0;1m<" << driveName << "> (\033[1;35m" 
                      << deviceSizeStr << "\033[0;1m)} ← \033[1;92m" 
                      << iso.filename << "\033[0;1m\n";
        }
        
        disableReadlineForConfirmation();

        // Get confirmation
        std::unique_ptr<char, decltype(&std::free)> confirmation(
            readline("\n\001\033[1;94m\002Proceed? (y/n): \001\033[0;1m\002"), &std::free
        );

        // Process confirmation
        if (confirmation && (confirmation.get()[0] == 'y' || confirmation.get()[0] == 'Y')) {
            restoreReadline();
            setupSignalHandlerCancellations();
            g_operationCancelled.store(false);
            return validPairs;
        }

        // Restore readline bindings if not proceeding
        restoreReadline();
        
        std::cout << "\n\033[1;93mWrite operation aborted by user.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
}


// Function to send writes to writeToUsb
void performWriteOperation(const std::vector<std::pair<IsoInfo, std::string>>& validPairs) {
    // Reset progress data before starting a new operation
    progressData.clear();
    progressData.reserve(validPairs.size());
    
    g_operationCancelled.store(false);
    
    // Initialize progress data
    for (const auto& [iso, device] : validPairs) {
        progressData.push_back(ProgressInfo{
            iso.filename,  // Pass filename
            device,        // Pass device
            iso.sizeStr    // Pass totalSize
        });
    }

    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    const size_t totalTasks = validPairs.size();
    const unsigned int numThreads = std::min(static_cast<unsigned int>(totalTasks), 
                                           static_cast<unsigned int>(maxThreads));
    ThreadPool pool(numThreads);

    disableInput();
    clearScrollBuffer();

    std::cout << "\n\033[0;1mProcessing " << (totalTasks > 1 ? "tasks" : "task") << " for \033[1;93mwrite\033[0;1m operation... (\033[1;91mCtrl+c\033[0;1m:cancel)\n\n";
    std::cout << "\033[s";  // Save cursor position

    auto startTime = std::chrono::high_resolution_clock::now();

    // Initialize device information maps in main thread
    std::unordered_map<std::string, std::string> deviceNames;
    std::unordered_map<std::string, uint64_t> deviceSizes;
    std::unordered_map<std::string, std::string> deviceSizeStrs;
    
    // Initialize device maps once in main thread
    for (const auto& prog : progressData) {
        if (deviceNames.find(prog.device) == deviceNames.end()) {
            deviceNames[prog.device] = getDriveName(prog.device);
            deviceSizes[prog.device] = getBlockDeviceSize(prog.device);
            deviceSizeStrs[prog.device] = formatFileSize(deviceSizes[prog.device]);
        }
    }

    // Helper lambda to display all progress entries
    auto displayAllProgress = [&]() {
		for (size_t i = 0; i < progressData.size(); ++i) {
			const auto& prog = progressData[i];
			std::string currentSize = formatFileSize(prog.bytesWritten.load());

			std::cout << "\033[K"  // Clear line
					<< ("\033[1;95m" + prog.filename + " \033[0;1m→ {" + 
						"\033[1;93m" + prog.device + "\033[0;1m \033[0;1m<" + 
						deviceNames[prog.device] + "> (\033[1;35m" + 
						deviceSizeStrs[prog.device] + "\033[0;1m)} \033[0;1m")
					<< std::right
					<< (prog.completed ? "\033[1;92mDONE\033[0;1m" :
						prog.failed ? "\033[1;91mFAIL\033[0;1m" :
						g_operationCancelled.load() ? "\033[1;93mCXL\033[0;1m" :
						std::to_string(prog.progress) + "%")
					<< " ["
					<< currentSize
					<< "/\033[1;35m"
					<< prog.totalSize
					<< "\033[0;1m] "
					<< "\033[0;1m" + formatSpeed(prog.speed) + "\033[0;1m"
					<< "\n";
		}
		std::cout << std::flush;
	};

    // Display progress lambda (modified to use helper)
    auto displayProgress = [&]() {
        while (!isProcessingComplete.load(std::memory_order_acquire) && 
              !g_operationCancelled.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "\033[u";  // Restore cursor position
            displayAllProgress();
        }
    };

    // Launch tasks
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < totalTasks; ++i) {
        futures.push_back(pool.enqueue([&, i]() {
            const auto& [iso, device] = validPairs[i];
            bool success = writeIsoToDevice(iso.path, device, i);
            
            if (success) {
                progressData[i].completed.store(true);
                completedTasks.fetch_add(1);
            }
        }));
    }

    // Start progress display thread
    std::thread progressThread(displayProgress);

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
    
    isProcessingComplete.store(true, std::memory_order_release);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
    
        std::cout << "\033[s";  // Save current position
        std::cout << "\033[2H\033[2K";  // Go to message line and clear it
        std::cout << "\033[0;1mProcessing for \033[1;93mwrite\033[0;1m operation " << (!g_operationCancelled.load() ? "→ \033[1;92mCOMPLETED\033[0;1m\n" : "→ \033[1;33mINTERRUPTED\033[0;1m\n");
        std::cout << "\033[u";  // Restore to current position

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(endTime - startTime).count();

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n\033[0;1mCompleted: \033[1;92m" << completedTasks.load()
            << "\033[0;1m/\033[1;93m" << validPairs.size() 
            << "\033[0;1m in \033[0;1m" << duration << " seconds.\033[0;1m\n";
    
    flushStdin();
    restoreInput();
}


// Function to prepare selections for write
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages) {
    clearScrollBuffer();
    std::unordered_set<int> indicesToProcess;

    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    tokenizeInput(input, isoFiles, uniqueErrorMessages, indicesToProcess);
    if (indicesToProcess.empty()) {
        return;
    }

    std::vector<IsoInfo> selectedIsos;
    for (int idx : indicesToProcess) {
        try {
            // Check if the file exists before processing
            if (!std::filesystem::exists(isoFiles[idx - 1])) {
                uniqueErrorMessages.insert("\033[1;35mMissing: \033[1;93m'" + isoFiles[idx - 1] + "'\033[1;35m.");
                continue;
            }

            selectedIsos.emplace_back(IsoInfo{
                isoFiles[idx - 1],
                std::filesystem::path(isoFiles[idx - 1]).filename().string(),
                std::filesystem::file_size(isoFiles[idx - 1]),
                formatFileSize(std::filesystem::file_size(isoFiles[idx - 1])),
                static_cast<size_t>(idx)
            });
        } catch (const std::filesystem::filesystem_error& e) {
            uniqueErrorMessages.insert("\033[1;91mError accessing ISO file: " + std::string(e.what()) + ".");
            continue;  // Skip this file and proceed with the next one
        }
    }

    if (selectedIsos.empty()) {
        clear_history();
        return;
    }

    auto validPairs = collectDeviceMappings(selectedIsos, uniqueErrorMessages);
    if (validPairs.empty()) {
        clear_history();
        return;
    }

    performWriteOperation(validPairs);
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to write ISO to USB device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex) {
    // Open ISO file
    std::ifstream iso(isoPath, std::ios::binary);
    if (!iso) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Open device with O_DIRECT
    int device_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (device_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Get device sector size
    int sectorSize = 0;
    if (ioctl(device_fd, BLKSSZGET, &sectorSize) < 0 || sectorSize == 0) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        return false;
    }

    // Get ISO file size and check alignment
    const uint64_t fileSize = std::filesystem::file_size(isoPath);
    if (fileSize % sectorSize != 0) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        return false;
    }

    // Set buffer size as multiple of sector size (8MB default)
    size_t bufferSize = 8 * 1024 * 1024;
    bufferSize = (bufferSize / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Allocate aligned buffer
    char* alignedBuffer = nullptr;
    if (posix_memalign((void**)&alignedBuffer, sectorSize, bufferSize) != 0) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        return false;
    }
    std::unique_ptr<char, decltype(&free)> bufferGuard(alignedBuffer, &free);

    // Initialize timing and speed calculation variables
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    const int UPDATE_INTERVAL_MS = 500;

    try {
        while (progressData[progressIndex].bytesWritten.load() < fileSize && !g_operationCancelled) {
            const uint64_t totalWritten = progressData[progressIndex].bytesWritten.load();
            const uint64_t remaining = fileSize - totalWritten;
            const size_t bytesToRead = std::min(bufferSize, static_cast<size_t>(remaining));

            iso.read(alignedBuffer, bytesToRead);
            const std::streamsize bytesRead = iso.gcount();
            
            if (bytesRead <= 0 || static_cast<size_t>(bytesRead) != bytesToRead) {
                throw std::runtime_error("Read error");
            }

            // Handle partial writes
            ssize_t bytesWritten = 0;
            while (bytesWritten < static_cast<ssize_t>(bytesToRead)) {
                size_t chunk = std::min(static_cast<size_t>(bytesToRead - bytesWritten), bufferSize);
                chunk = (chunk / sectorSize) * sectorSize;
                if (chunk == 0) break;
                ssize_t result = write(device_fd, alignedBuffer + bytesWritten, chunk);
                if (result == -1) {
                    throw std::runtime_error("Write error");
                }
                bytesWritten += result;
            }

            // Atomically update progress
            progressData[progressIndex].bytesWritten.fetch_add(bytesWritten);
            bytesInWindow += bytesWritten;

            // Update progress and speed
            auto now = std::chrono::high_resolution_clock::now();
            auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);

            if (timeSinceLastUpdate.count() >= UPDATE_INTERVAL_MS) {
                // Calculate and update progress atomically
                const int progress = static_cast<int>((static_cast<double>(progressData[progressIndex].bytesWritten.load()) / fileSize) * 100);
                progressData[progressIndex].progress.store(progress);

                // Calculate and update speed atomically
                double seconds = timeSinceLastUpdate.count() / 1000.0;
                double mbPerSec = (static_cast<double>(bytesInWindow) / (1024 * 1024)) / seconds;
                progressData[progressIndex].speed.store(mbPerSec);

                // Reset window counters
                lastUpdate = now;
                bytesInWindow = 0;
            }
        }
    } catch (...) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        return false;
    }

    if (!g_operationCancelled.load()) {
        fsync(device_fd);
    }
    close(device_fd);

    if (!g_operationCancelled && progressData[progressIndex].bytesWritten.load() == fileSize) {
        progressData[progressIndex].completed.store(true);
        return true;
    }
    
    return false;
}
