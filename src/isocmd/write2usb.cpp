// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"
#include "../write.h"


// Shared progress data
std::vector<ProgressInfo> progressData;


// Function to get the size of a block device
uint64_t getBlockDeviceSize(const std::string& device) {
    struct stat st;
    if (stat(device.c_str(), &st) == 0) {
        if (S_ISBLK(st.st_mode)) {
            int fd = open(device.c_str(), O_RDONLY);
            if (fd != -1) {
                uint64_t size;
                if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
                    close(fd);
                    return size;
                }
                close(fd);
            }
        }
    }
    return 0; // Return 0 if unable to determine size
}


// Function to check if block device is usb
bool isUsbDevice(const std::string& devicePath) {
    try {
        // Extract device name (e.g., "sdb" from "/dev/sdb")
        size_t lastSlash = devicePath.find_last_of('/');
        std::string deviceName = (lastSlash == std::string::npos) ? 
            devicePath : devicePath.substr(lastSlash + 1);
            
        // Skip if empty or is a partition (contains numbers)
        if (deviceName.empty() || 
            std::any_of(deviceName.begin(), deviceName.end(), ::isdigit)) {
            return false;
        }

        // Base sysfs path
        std::string sysPath = "/sys/block/" + deviceName;
        if (!std::filesystem::exists(sysPath)) {
            return false;
        }

        bool isUsb = false;

        // Method 1: Check device/vendor ID in device path
        std::error_code ec;
        std::string resolvedPath = std::filesystem::canonical(sysPath, ec).string();
        if (!ec) {
            // Look for patterns like "usb" or "0951" (common USB vendor IDs)
            isUsb = resolvedPath.find("/usb") != std::string::npos;
        }

        // Method 2: Check various uevent locations
        std::vector<std::string> ueventPaths = {
            sysPath + "/device/uevent",
            sysPath + "/uevent"
        };

        for (const auto& path : ueventPaths) {
            std::ifstream uevent(path);
            std::string line;
            while (std::getline(uevent, line)) {
                // Check for various USB indicators
                if (line.find("ID_BUS=usb") != std::string::npos ||
                    line.find("DRIVER=usb") != std::string::npos ||
                    line.find("ID_USB") != std::string::npos) {
                    isUsb = true;
                    break;
                }
            }
            if (isUsb) break;
        }

        // Method 3: Check if device has USB-specific attributes
        std::vector<std::string> usbIndicators = {
            sysPath + "/device/speed",      // USB speed attribute
            sysPath + "/device/version",    // USB version
            sysPath + "/device/manufacturer" // USB manufacturer
        };

        for (const auto& path : usbIndicators) {
            if (std::filesystem::exists(path)) {
                isUsb = true;
                break;
            }
        }

        // Only consider it a USB device if we found USB indicators AND
        // can verify it's removable (if removable info is available)
        std::string removablePath = sysPath + "/removable";
        std::ifstream removableFile(removablePath);
        std::string removable;
        if (removableFile && std::getline(removableFile, removable)) {
            return isUsb && (removable == "1");
        }

        // If we can't check removable status, rely on USB indicators alone
        return isUsb;

    } catch (const std::exception&) {
        return false;
    }
}


// Function to check if usb device is mounted
bool isDeviceMounted(const std::string& device) {
    std::ifstream mountsFile("/proc/mounts");
    if (!mountsFile.is_open()) {
        return false; // Can't check mounts, assume not mounted for safety
    }

    std::string line;
    std::string deviceName = device;
    
    // Remove "/dev/" prefix if present for consistent comparison
    if (deviceName.substr(0, 5) == "/dev/") {
        deviceName = deviceName.substr(5);
    }
    
    while (std::getline(mountsFile, line)) {
        std::istringstream iss(line);
        std::string mountDevice;
        iss >> mountDevice;
        
        // Remove "/dev/" prefix from mounted device for comparison
        if (mountDevice.substr(0, 5) == "/dev/") {
            mountDevice = mountDevice.substr(5);
        }
        
        // Check if the device or any of its partitions are mounted
        if (mountDevice == deviceName || 
            (mountDevice.find(deviceName) == 0 && 
             std::isdigit(mountDevice[deviceName.length()]))) {
            mountsFile.close();
            return true;
        }
    }
    
    mountsFile.close();
    return false;
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
    namespace fs = std::filesystem;
    
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


// Function to handle device mapping collection and validation
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos,std::set<std::string>& uniqueErrorMessages) {
    while (true) {
        clearScrollBuffer();

        // Display user input errors at top
        if (!uniqueErrorMessages.empty()) {
            std::cout << "\n";
            for (const auto& err : uniqueErrorMessages) {
                std::cout << err << "\n";
            }
            uniqueErrorMessages.clear();
        }

        // Build device prompt
        std::string devicePrompt = "\n\033[0;1m Selected \033[1;92mISO\033[0;1m:\n\n";
        for (size_t i = 0; i < selectedIsos.size(); ++i) {
            auto [shortDir, filename] = extractDirectoryAndFilename(selectedIsos[i].path);
            devicePrompt += "  \033[1;93m" + std::to_string(i+1) + ">\033[0;1m " + 
                          shortDir + "/\033[1;95m" + filename + "\033[0;1m (\033[1;35m" + 
                          selectedIsos[i].sizeStr + "\033[0;1m)\n";
        }

        devicePrompt += "\n\033[0;1mRemovable USB Devices:\033[0;1m\n\n";
        std::vector<std::string> usbDevices = getRemovableDevices();

        if (usbDevices.empty()) {
            devicePrompt += "  \033[1;91mNo removable USB devices detected!\033[0;1m\n";
        } else {
            for (const auto& device : usbDevices) {
                try {
                    std::string driveName = getDriveName(device);
                    uint64_t deviceSize = getBlockDeviceSize(device);
                    std::string sizeStr = formatFileSize(deviceSize);
                    bool mounted = isDeviceMounted(device);
                    devicePrompt += "  \033[1;93m" + device + "\033[0;1m <" + driveName + 
                                  "> (\033[1;35m" + sizeStr + "\033[0;1m)" +
                                  (mounted ? " \033[1;91m(mounted)\033[0;1m" : "") + "\n";
                } catch (...) {
                    devicePrompt += "  \033[1;91m" + device + " (error)\033[0;1m\n";
                }
            }
        }
        
        // Restore readline autocomplete and screen clear bindings
        rl_bind_key('\f', rl_clear_screen);
        rl_bind_key('\t', rl_complete);
        
        rl_bind_keyseq("\033[A", rl_get_previous_history); // Restore Up arrow
		rl_bind_keyseq("\033[B", rl_get_next_history);     // Restore Down arrow

        devicePrompt += "\n\001\033[1;92m\002Mappings\001\033[1;94m\002 ↵ as \001\033[1;93m\002INDEX>DEVICE\001\033[1;94m\002, ? ↵ for help, ↵ to return:\001\033[0;1m\002 ";

        // Get user input
        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );
        std::string mainInputString(deviceInput.get());
        if (mainInputString == "?") {
            help();
            continue;
        }
        
        if (deviceInput && *deviceInput) add_history(deviceInput.get());

        if (!deviceInput || deviceInput.get()[0] == '\0') {
            return {};
        }

        // Parse device mappings
        std::vector<std::pair<size_t, std::string>> deviceMap;
        std::set<std::string> usedDevices;
        std::vector<std::string> errors;
        std::istringstream pairStream(deviceInput.get());
        std::string pair;

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
        std::set<size_t> mappedIndices;
        for (const auto& [index, device] : deviceMap) {
            mappedIndices.insert(index);
        }
        for (size_t i = 1; i <= selectedIsos.size(); ++i) {
            if (mappedIndices.find(i) == mappedIndices.end()) {
                errors.push_back("Missing mapping for ISO " + std::to_string(i));
            }
        }

        if (!errors.empty()) {
            std::cerr << "\n\033[1;91mErrors:\033[0;1m\n";
            for (const auto& err : errors) {
                std::cerr << "  • " << err << "\n";
            }
            std::cout << "\n\033[1;92m↵ to try again...\033[0;1m";
            std::cin.ignore();
            continue;
        }

        // Validate devices
        std::vector<std::pair<IsoInfo, std::string>> validPairs;
        std::vector<std::string> validationErrors;
        bool permissions = false;

        for (const auto& [index, device] : deviceMap) {
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
            if (!permissions) {
                std::cout << "\n\033[1;92m↵ to try again...\033[0;1m";
            } else {
                std::cout << "\n\033[1;92m↵ to continue...\033[0;1m";
                permissions = false;
            }
            std::cin.ignore();
            continue;
        }

        // Final confirmation
        std::cout << "\n\033[1;93mWARNING: This will \033[1;91m*ERASE ALL DATA*\033[1;93m on:\033[0;1m\n\n";
        for (const auto& [iso, device] : validPairs) {
			// Get device size before other checks
            uint64_t deviceSize = getBlockDeviceSize(device);
            std::string deviceSizeStr = formatFileSize(deviceSize);
            std::string driveName = getDriveName(device);
            
            std::cout << "  {\033[1;93m" << device << " \033[0;1m<" << driveName << "> (\033[1;35m" << deviceSizeStr << "\033[0;1m)} ← \033[1;92m" 
                      << iso.filename << "\033[0;1m\n";
        }
        
        rl_bind_key('\f', prevent_readline_keybindings);
		rl_bind_key('\t', prevent_readline_keybindings);
		// Disable up/down arrow keys for history browsing
		rl_bind_keyseq("\033[A", prevent_readline_keybindings); // Up arrow
		rl_bind_keyseq("\033[B", prevent_readline_keybindings); // Down arrow

        std::unique_ptr<char, decltype(&std::free)> confirmation(
            readline("\n\001\033[1;94m\002Proceed? (y/n): \001\033[0;1m\002"), &std::free
        );

        if (confirmation && (confirmation.get()[0] == 'y' || confirmation.get()[0] == 'Y')) {
            return validPairs;
        }

        std::cout << "\n\033[1;93mWrite operation aborted by user.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore();
    }
}


// Function to send writes to writeToUsb
void performWriteOperation(const std::vector<std::pair<IsoInfo, std::string>>& validPairs) {
    // Reset progress data before starting a new operation
    progressData.clear();
    progressData.reserve(validPairs.size());
    
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
    std::cout << "\n\033[0;1mWriting... (\033[1;91mCtrl+c to cancel\033[0;1m)\n\n";
    std::cout << "\033[s";

    auto startTime = std::chrono::high_resolution_clock::now();

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

    // Create separate maps for device information
	std::unordered_map<std::string, std::string> deviceNames;
	std::unordered_map<std::string, uint64_t> deviceSizes;
	std::unordered_map<std::string, std::string> deviceSizeStrs;

	// Initialize maps before the display loop
	auto initDeviceMaps = [&]() {
		for (const auto& prog : progressData) {
			if (deviceNames.find(prog.device) == deviceNames.end()) {
				deviceNames[prog.device] = getDriveName(prog.device);
				deviceSizes[prog.device] = getBlockDeviceSize(prog.device);
				deviceSizeStrs[prog.device] = formatFileSize(deviceSizes[prog.device]);
			}
		}
	};

	// Display progress lambda
	auto displayProgress = [&]() {
		// Initialize maps once
		initDeviceMaps();

		while (!isProcessingComplete.load(std::memory_order_acquire) && !g_operationCancelled.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			std::cout << "\033[u";
			for (size_t i = 0; i < progressData.size(); ++i) {
				const auto& prog = progressData[i];
				std::string currentSize = formatFileSize(prog.bytesWritten.load());

				std::cout << "\033[K"
						<< ("\033[1;95m" + prog.filename + " \033[0;1m→ {" + 
                          "\033[1;93m" + prog.device + "\033[0;1m \033[0;1m<" + deviceNames[prog.device] + "> (\033[1;35m" + deviceSizeStrs[prog.device] + "\033[0;1m)} \033[0;1m")
						<< std::right
						<< (prog.completed ? "\033[1;92mDONE\033[0;1m" :
							prog.failed ? "\033[1;91mFAIL\033[0;1m" :
							std::to_string(prog.progress) + "%")
						<< " ["
						<< currentSize
						<< "/\033[1;35m"
						<< prog.totalSize
						<< "\033[0;1m] "
						<< "\033[0;1m" + formatSpeed(prog.speed) + "\033[0;1m" // Always display speed
						<< "\n";
			}
			std::cout << std::flush;
		}
	};

    std::thread progressThread(displayProgress);

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }
    
    isProcessingComplete.store(true, std::memory_order_release);
    progressThread.join();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        endTime - startTime).count();
    
    std::cout << "\n\033[0;1mCompleted: \033[1;92m" << completedTasks.load()
              << "\033[0;1m/\033[1;93m" << validPairs.size() 
              << "\033[0;1m in \033[0;1m" << duration << " seconds.\033[0;1m\n";
    
    if (g_operationCancelled.load()) {
        std::cout << "\n\033[1;33mOperation interrupted by user.\033[0;1m\n";
    }
    flushStdin();
    restoreInput();
}


// Function to prepare selections for write
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages) {
    clearScrollBuffer();
    std::set<int> indicesToProcess;

    setupSignalHandlerCancellations();
    g_operationCancelled = false;

    tokenizeInput(input, isoFiles, uniqueErrorMessages, indicesToProcess);
    if (indicesToProcess.empty()) {
        std::cout << "\n\033[1;91mNo valid input provided for write.\n\033[0;1m";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore();
        return;
    }

    std::vector<IsoInfo> selectedIsos;
    for (int idx : indicesToProcess) {
        selectedIsos.emplace_back(IsoInfo{
            isoFiles[idx-1],
            std::filesystem::path(isoFiles[idx-1]).filename().string(),
            std::filesystem::file_size(isoFiles[idx-1]),
            formatFileSize(std::filesystem::file_size(isoFiles[idx-1])),
            static_cast<size_t>(idx)  // Added cast here
        });
    }

    auto validPairs = collectDeviceMappings(selectedIsos, uniqueErrorMessages);
    if (validPairs.empty()) {
        clear_history();
        return;
    }

    performWriteOperation(validPairs);
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore();
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

    if (!g_operationCancelled) {
        fsync(device_fd);
    }
    close(device_fd);

    if (!g_operationCancelled && progressData[progressIndex].bytesWritten.load() == fileSize) {
        progressData[progressIndex].completed.store(true);
        return true;
    }
    
    return false;
}
