// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../write.h"
#include "../readline.h"
#include "../display.h"


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


// Function to check if block device is usb for write2usb
bool isUsbDevice(const std::string& devicePath) {
    try {
		// Early return if path doesn't start with "/dev/"
        if (devicePath.substr(0, 5) != "/dev/") {
            return false;
        }
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


// Function to check if usb device is mounted for write2usb
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


// Function to parse device mappings from a string and validate the mappings
std::vector<std::pair<size_t, std::string>> parseDeviceMappings(const std::string& pairString, const std::vector<std::string>& selectedIsos, std::vector<std::string>& errors) {
    
    // Vector to hold the final device mappings (index, device)
    std::vector<std::pair<size_t, std::string>> deviceMap;
    
    // Unordered set to keep track of devices already used (to prevent duplicates)
    std::unordered_set<std::string> usedDevices;
    
    // Create a string stream from the input pair string to read individual pairs
    std::istringstream pairStream(pairString);
    std::string pair;
    
    // Clear previous errors
    errors.clear();
    
    // Read pairs separated by ';' from the input string
    while (std::getline(pairStream, pair, ';')) {
        // Trim leading and trailing whitespace from the pair
        pair.erase(pair.find_last_not_of(" \t\n\r\f\v") + 1);  // Remove trailing whitespace
        pair.erase(0, pair.find_first_not_of(" \t\n\r\f\v"));    // Remove leading whitespace
        
        // Skip empty pairs
        if (pair.empty()) continue;
        
        // Find the position of the '>' separator between index and device
        size_t sepPos = pair.find('>');
        if (sepPos == std::string::npos) {
            // If no '>' is found, the format is invalid; log error and skip the pair
            errors.push_back("Invalid pair format: '" + pair + "'");
            continue;
        }
        
        // Extract the index part (before '>') and the device part (after '>')
        std::string indexStr = pair.substr(0, sepPos);
        std::string device = pair.substr(sepPos + 1);
        
        try {
            // Try to convert the index string to a size_t
            size_t index = std::stoul(indexStr);
            
            // Check if the index is valid (within the range of selected ISOs)
            if (index < 1 || index > selectedIsos.size()) {
                errors.push_back("Invalid index " + indexStr);
                continue;
            }
            
            // Check if the device has been used already
            if (usedDevices.count(device)) {
                errors.push_back("Device " + device + " used multiple times");
                continue;
            }
            
            // Add the valid mapping (index, device) to the deviceMap
            deviceMap.emplace_back(index, device);
            usedDevices.insert(device);  // Mark this device as used
            
        } catch (...) {
            // If index conversion fails, log the error
            errors.push_back("Invalid index: '" + indexStr + "'");
        }
    }
    
    // Validate that all selected ISOs have at least one device mapping
    std::unordered_set<size_t> mappedIndices;
    // Collect all indices that have been mapped to a device
    for (const auto& [index, device] : deviceMap) {
        mappedIndices.insert(index);
    }
    
    // Check if each index from 1 to the number of selected ISOs is mapped
    for (size_t i = 1; i <= selectedIsos.size(); ++i) {
        if (mappedIndices.find(i) == mappedIndices.end()) {
            // If an ISO index is not mapped, add an error
            errors.push_back("Missing mapping for ISO " + std::to_string(i));
        }
    }
    
    // Return the vector of valid device mappings
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
            auto [isoDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write");
            devicePromptStream << "  \033[1;93m" << (i+1) << ">\033[0;1m " 
                               << (!displayConfig::toggleNamesOnly ? isoDir + "/\033[1;95m" : "\033[1;95m" ) 
                               << filename 
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
        devicePromptStream << "\n\001\033[1;92m\002Mappings\001\033[1;94m\002 ↵ as \001\033[1;93m\002INDEX>DEVICE\001\033[1;94m\002, ? ↵ for help, < ↵ to return:\001\033[0;1m\002 ";
        std::string devicePrompt = devicePromptStream.str();

        // Get user input
        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );
        
        // Handle empty input
        if (!deviceInput) {
            restoreReadline();
            return {};
        }
        
        if (deviceInput.get()[0] == '\0') {
			continue;
		}
		
		 // Process input
        std::string mainInputString(deviceInput.get());
		if (mainInputString == "<") {
			restoreReadline();
            return {};
        }
        
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
            else {
                progressData[i].failed.store(true);
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
    
    // Count failed tasks
    size_t failedTasksValue = 0;
    for (const auto& prog : progressData) {
        if (prog.failed.load()) {
            failedTasksValue++;
        }
    }
    
    size_t completedTasksValue = completedTasks.load();
    std::string operation = "\033[1;93mwrite\033[0;1m";
    
    std::cout << "\r\033[0;1mStatus: " << operation << "\033[0;1m → " 
              << (!g_operationCancelled.load() 
                  ? (failedTasksValue > 0 
                     ? (completedTasksValue > 0 
                        ? "\033[1;93mPARTIAL" // Yellow, some completed, some failed
                        : "\033[1;91mFAILED")  // Red, none completed, some failed
                     : "\033[1;92mCOMPLETED") // Green, all completed successfully
                  : "\033[1;33mINTERRUPTED") // Yellow, operation cancelled
              << "\033[0;1m" << std::endl;
    
    std::cout << "\033[u";  // Restore to current position

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(endTime - startTime).count();

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n\033[0;1mSuccessful: \033[1;92m" << completedTasks.load()
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

    // Get ISO file size
    const uint64_t fileSize = std::filesystem::file_size(isoPath);
    
    // Calculate aligned file size (round up to next sector boundary)
    const uint64_t alignedFileSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

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
        uint64_t totalBytesProcessed = 0;
        
        while (totalBytesProcessed < alignedFileSize && !g_operationCancelled) {
            const uint64_t remaining = alignedFileSize - totalBytesProcessed;
            const size_t chunkSize = std::min(bufferSize, static_cast<size_t>(remaining));
            
            // Always work with sector-aligned chunks
            assert(chunkSize % sectorSize == 0);
            
            // Clear buffer to ensure padding bytes are zero
            std::memset(alignedBuffer, 0, chunkSize);
            
            // Read data from ISO file
            size_t bytesToReadFromFile = chunkSize;
            if (totalBytesProcessed + chunkSize > fileSize) {
                // This is the final chunk that extends beyond the file
                bytesToReadFromFile = fileSize - totalBytesProcessed;
            }
            
            if (bytesToReadFromFile > 0) {
                iso.read(alignedBuffer, bytesToReadFromFile);
                const std::streamsize bytesRead = iso.gcount();
                
                if (bytesRead < 0) {
                    throw std::runtime_error("Read error from ISO file");
                }
                
                if (static_cast<size_t>(bytesRead) != bytesToReadFromFile) {
                    if (iso.eof() && totalBytesProcessed + bytesRead == fileSize) {
                        // Expected EOF - we've read the entire file
                    } else {
                        throw std::runtime_error("Incomplete read from ISO file");
                    }
                }
            }
            
            // The buffer now contains file data + zero padding to sector boundary
            // Write the entire sector-aligned chunk
            ssize_t totalWritten = 0;
            while (totalWritten < static_cast<ssize_t>(chunkSize)) {
                ssize_t result = write(device_fd, alignedBuffer + totalWritten, chunkSize - totalWritten);
                if (result == -1) {
                    if (errno == EINTR) {
                        continue; // Interrupted by signal, retry
                    }
                    throw std::runtime_error("Write error to device: " + std::string(strerror(errno)));
                }
                if (result == 0) {
                    throw std::runtime_error("Unexpected zero write to device");
                }
                totalWritten += result;
            }

            // Update progress tracking
            totalBytesProcessed += chunkSize;
            
            // Update progress based on actual file bytes written (not including padding)
            const uint64_t fileBytesWritten = std::min(totalBytesProcessed, fileSize);
            progressData[progressIndex].bytesWritten.store(fileBytesWritten);
            bytesInWindow += totalWritten;

            // Update progress and speed
            auto now = std::chrono::high_resolution_clock::now();
            auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);

            if (timeSinceLastUpdate.count() >= UPDATE_INTERVAL_MS) {
                // Calculate and update progress atomically
                const int progress = static_cast<int>((static_cast<double>(fileBytesWritten) / fileSize) * 100);
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
        
        // Ensure all data is written to device
        if (!g_operationCancelled.load()) {
            if (fsync(device_fd) != 0) {
                throw std::runtime_error("Failed to sync data to device: " + std::string(strerror(errno)));
            }
        }
        
    } catch (const std::exception& e) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        // Log the specific error if logging is available
        // std::cerr << "ISO write failed: " << e.what() << std::endl;
        return false;
    } catch (...) {
        progressData[progressIndex].failed.store(true);
        close(device_fd);
        return false;
    }

    close(device_fd);

    if (!g_operationCancelled.load()) {
        fsync(device_fd);
    }

    if (!g_operationCancelled && progressData[progressIndex].bytesWritten.load() == fileSize) {
        progressData[progressIndex].completed.store(true);
        return true;
    }
    
    return false;
}
