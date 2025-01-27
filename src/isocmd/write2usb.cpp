// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


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
bool isUsbDevice(const std::string& device) {
    // Resolve symbolic links to get the actual device path
    char resolvedPath[PATH_MAX];
    if (realpath(device.c_str(), resolvedPath) == nullptr) {
        return false;
    }

    // Extract the device name (e.g., "sdb" from "/dev/sdb")
    std::string devicePath(resolvedPath);
    std::string deviceName = devicePath.substr(devicePath.find_last_of('/') + 1);

    // Reject partitions (e.g., "sdb1", "sda2")
    if (deviceName.find_first_of("0123456789") != std::string::npos) {
        return false; // Partitions are not allowed
    }

    // Check if the device exists in /sys/block/
    std::string sysPath = "/sys/block/" + deviceName + "/removable";
    if (!std::filesystem::exists(sysPath)) {
        return false;
    }

    // Check if it's removable
    std::ifstream removableFile(sysPath);
    if (!removableFile) {
        return false;
    }

    std::string removable;
    std::getline(removableFile, removable);
    return (removable == "1");
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


// Function to parse selection for writes
std::vector<size_t> parseIsoSelection(const std::string& input, size_t maxIsos) {
    std::vector<size_t> indices;
    std::istringstream iss(input);
    std::string token;
    
    while (iss >> token) {
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start = std::stoi(token.substr(0, dashPos));
            int end = std::stoi(token.substr(dashPos + 1));
            
            // Handle reverse ranges
            if (start > end) std::swap(start, end);
            
            if (start < 1 || end > static_cast<int>(maxIsos)) {
                throw std::invalid_argument("Invalid ISO range: " + token);
            }
            
            for (int i = start; i <= end; ++i) {
                indices.push_back(static_cast<size_t>(i));
            }
        } else {
            int idx = std::stoi(token);
            if (idx < 1 || idx > static_cast<int>(maxIsos)) {
                throw std::invalid_argument("Invalid ISO index: " + token);
            }
            indices.push_back(static_cast<size_t>(idx));
        }
    }
    
    // Remove duplicates and sort
    std::sort(indices.begin(), indices.end());
    auto last = std::unique(indices.begin(), indices.end());
    indices.erase(last, indices.end());
    
    return indices;
}


// Function to foram fileSize
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


// Function to format speed
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


// IsoInfo structure
struct IsoInfo {
    std::string path;
    std::string filename;
    uint64_t size;
    std::string sizeStr;
    size_t originalIndex;
};


// Progress tracking structure
struct ProgressInfo {
    std::string filename;
    std::string device;
    int progress = 0;
    std::string currentSize;
    std::string totalSize;
    double speed = 0.0;  // Speed in MB/s
    bool failed = false;
    bool completed = false;
};


// Shared progress data and mutex for protection
std::mutex progressMutex;
std::vector<ProgressInfo> progressData;


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


// Function to prepare writing ISO to usb
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    // Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
        
    // Reset cancellation flag
    g_operationCancelled = false;
    
    // Parse ISO selection
    std::vector<size_t> isoIndices;
    try {
        isoIndices = parseIsoSelection(input, isoFiles.size());
    } catch (const std::exception& e) {
        std::cerr << "\033[1;91mError: " << e.what() << "\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Collect selected ISOs
    std::vector<IsoInfo> selectedIsos;
    for (size_t pos = 0; pos < isoIndices.size(); ++pos) {
        size_t idx = isoIndices[pos];
        IsoInfo info;
        info.path = isoFiles[idx - 1];
        info.filename = info.path.substr(info.path.find_last_of('/') + 1);
        info.size = std::filesystem::file_size(info.path);
        info.sizeStr = formatFileSize(info.size);
        info.originalIndex = idx;
        selectedIsos.push_back(info);
    }

    bool isFinished = false;
    do {
		clearScrollBuffer();
        // Device mapping input
        std::string devicePrompt = "\n\033[0;1m Selected \033[1;92mISO\033[0;1m:\n\n";
        for (size_t i = 0; i < selectedIsos.size(); ++i) {
            // Extract the directory and filename
            auto [shortDir, filename] = extractDirectoryAndFilename(selectedIsos[i].path);

            // Highlight the filename in magenta
            std::string highlightedPath = shortDir + "/\033[1;95m" + filename + "\033[0;1m";

            // Append to the device prompt
            devicePrompt += "  \033[1;93m" + std::to_string(i + 1) + ">\033[0;1m " +
                            highlightedPath + " (\033[1;35m" + 
                            selectedIsos[i].sizeStr + "\033[0;1m)\n";
        }

        // Restore readline autocomplete and screen clear bindings
        rl_bind_key('\f', rl_clear_screen);
        rl_bind_key('\t', rl_complete);
        
        devicePrompt += "\n\033[0;1mAvailable Removable Devices:\033[0;1m\n\n";
		std::vector<std::string> usbDevices = getRemovableDevices();

		if (usbDevices.empty()) {
			devicePrompt += "  \033[1;91mNo available Removable devices detected!\033[0;1m\n";
		} else {
			for (const auto& device : usbDevices) {
				try {
						std::string driveName = getDriveName(device);
						uint64_t deviceSize = getBlockDeviceSize(device);
						std::string sizeStr = formatFileSize(deviceSize);
						bool mounted = isDeviceMounted(device);
            
						devicePrompt += "  \033[1;93m" + device + "\033[0;1m" +
									" <" + driveName + ">" +
									" (\033[1;35m" + sizeStr + "\033[0;1m)" +
									(mounted ? " \033[1;91m(mounted)\033[0;1m" : "") + 
									"\n";
				} catch (const std::exception& e) {
					devicePrompt += "  \033[1;91m" + device + " (error: " + e.what() + ")\033[0;1m\n";
				}
			}
		}

		// Add separator and instructions
		devicePrompt += "\n\001\033[1;92m\002Pair\001\033[1;94m\002 ↵ as \001\033[1;93m\002INDEX>DEVICE\001\033[1;94m\002 (e.g, \001\033[1;94m\0021>/dev/sdc;2>/dev/sdd\001\033[1;94m\002), or ↵ to return:\001\033[0;1m\002 ";

        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );
        
        if (deviceInput && *deviceInput) {
            add_history(deviceInput.get());
        }

        if (!deviceInput || deviceInput.get()[0] == '\0') {
            clear_history();
            return;
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
                validationErrors.push_back("\033[1;93m'" + device + "'\033[0;1m is not a removable device");
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

        // Confirmation
        std::cout << "\n\033[1;93mWARNING: This will \033[1;91m*ERASE ALL DATA*\033[1;93m on selected devices:\033[0;1m\n\n";
        for (const auto& [iso, device] : validPairs) {
            uint64_t deviceSize = getBlockDeviceSize(device);
            std::string deviceSizeStr = formatFileSize(deviceSize);
            std::string driveName = getDriveName(device);

            std::cout << "  \033[1;93m" << device << "\033[0;1m (\033[1;95m" << deviceSizeStr << "\033[0;1m)"
                    << " \033[1;93m<" + driveName + ">\033[0;1m"
                    << " ← \033[1;92m" << iso.filename << " \033[0;1m(\033[1;95m" << iso.sizeStr << "\033[0;1m)\n";
        }

        std::unique_ptr<char, decltype(&std::free)> confirmation(
            readline("\n\001\033[1;94m\002Proceed? (y/n): \001\033[0;1m\002"), &std::free
        );

        if (!confirmation || 
            (confirmation.get()[0] != 'y' && confirmation.get()[0] != 'Y')) {
            std::cout << "\n\033[1;93mWrite operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore();
            continue;
        }

        // Initialize progress tracking
        progressData.clear();
        for (size_t i = 0; i < validPairs.size(); ++i) {
            const auto& [iso, device] = validPairs[i];
            progressData.emplace_back(ProgressInfo{
                iso.filename,
                device,
                0,
                "0 B",
                iso.sizeStr,
                false,
                false
            });
        }

        // Thread pool setup
        std::atomic<size_t> completedTasks(0);
        std::atomic<bool> isProcessingComplete(false);
        const size_t totalTasks = validPairs.size();
        const unsigned int numThreads = std::min(static_cast<unsigned int>(totalTasks), 
                                               static_cast<unsigned int>(maxThreads));
        ThreadPool pool(numThreads);
        
        const size_t chunkSize = std::max(size_t(1), 
            std::min(size_t(50), (totalTasks + numThreads - 1) / numThreads));

        std::atomic<size_t> activeTaskCount(0);
        std::condition_variable taskCompletionCV;
        std::mutex taskCompletionMutex;

        auto startTime = std::chrono::high_resolution_clock::now();

        // Enqueue chunked tasks
        for (size_t i = 0; i < totalTasks; i += chunkSize) {
            const size_t end = std::min(i + chunkSize, totalTasks);
            activeTaskCount.fetch_add(1, std::memory_order_relaxed);

            pool.enqueue([&, i, end]() {
                for (size_t j = i; j < end && !g_operationCancelled.load(std::memory_order_acquire); ++j) {
                    const auto& [iso, device] = validPairs[j];
                    bool success = false;
                    
                    try {
                        success = writeIsoToDevice(iso.path, device, j);
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lock(progressMutex);
                        progressData[j].failed = true;
                    }

                    if (success) {
                        std::lock_guard<std::mutex> lock(progressMutex);
                        progressData[j].completed = true;
                        completedTasks.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if (activeTaskCount.fetch_sub(1, std::memory_order_release) == 1) {
                    taskCompletionCV.notify_one();
                }
            });
        }

        // Start operations
        disableInput();
        clearScrollBuffer();
        std::cout << "\n\033[0;1mWriting... (\033[1;91mCtrl+c to cancel\033[0;1m)\n\n";
        std::cout << "\033[s"; // Save cursor position

        // Progress display thread
        auto displayProgress = [&]() {
            while (!isProcessingComplete.load(std::memory_order_acquire) && 
                  !g_operationCancelled.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::lock_guard<std::mutex> lock(progressMutex);
                
                std::cout << "\033[u"; // Restore cursor
                for (size_t i = 0; i < progressData.size(); ++i) {
                    const auto& prog = progressData[i];
                    std::string driveName = getDriveName(prog.device);
                    std::cout << "\033[K" // Clear line
                            << ("\033[1;95m" + prog.filename + " \033[0;1m→ " + 
                                "\033[1;93m" + prog.device + "\033[0;1m \033[1;93m<" + driveName + "> \033[0;1m")
                            << std::right
                            << (prog.completed ? "\033[1;92mDONE\033[0;1m" :
                                prog.failed ? "\033[1;91mFAIL\033[0;1m" :
                                std::to_string(prog.progress) + "%")
                            << " ["
                            << prog.currentSize
                            << "/"
                            << prog.totalSize
                            << "] "
                            << (!prog.completed && !prog.failed ? "\033[0;1m" + formatSpeed(prog.speed) + "\033[0;1m" : "")
                            << "\n";
                }
                std::cout << std::flush;
            }
        };

        std::thread progressThread(displayProgress);

        // Wait for completion
        {
            std::unique_lock<std::mutex> lock(taskCompletionMutex);
            taskCompletionCV.wait(lock, [&]() { 
                return activeTaskCount.load(std::memory_order_acquire) == 0 || 
                      g_operationCancelled.load(std::memory_order_acquire);
            });
        }
        isProcessingComplete.store(true, std::memory_order_release);
        progressThread.join();

        // Final status
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            endTime - startTime).count();
        
        std::cout << "\n\033[0;1mCompleted: \033[1;92m" << completedTasks.load(std::memory_order_relaxed)
                << "\033[0;1m/\033[1;93m" << validPairs.size() 
                << "\033[0;1m in \033[0;1m" << duration << " seconds.\033[0;1m\n";
        
        if (g_operationCancelled.load(std::memory_order_acquire)) {
            std::cout << "\n\033[1;33mOperation interrupted by user.\033[0;1m\n";
        }

        isFinished = true;
        flushStdin();
        restoreInput();
        clear_history();
    } while (!isFinished);

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to write ISO to usb device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t& progressIndex) {
    // Open ISO file
    std::ifstream iso(isoPath, std::ios::binary);
    if (!iso) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        return false;
    }

    // Open device with O_DIRECT
    int device_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (device_fd == -1) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        return false;
    }

    // Get device sector size
    int sectorSize = 0;
    if (ioctl(device_fd, BLKSSZGET, &sectorSize) < 0 || sectorSize == 0) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        close(device_fd);
        return false;
    }

    // Get ISO file size and check alignment
    const uint64_t fileSize = std::filesystem::file_size(isoPath);
    if (fileSize % sectorSize != 0) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        close(device_fd);
        return false;
    }

    // Initialize progress
    const std::string totalSize = formatFileSize(fileSize);
    uint64_t totalWritten = 0;

    // Set buffer size as multiple of sector size (8MB default)
    size_t bufferSize = 8 * 1024 * 1024;
    bufferSize = (bufferSize / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;  // Handle large sector sizes

    // Allocate aligned buffer
    char* alignedBuffer = nullptr;
    if (posix_memalign((void**)&alignedBuffer, sectorSize, bufferSize) != 0) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        close(device_fd);
        return false;
    }
    std::unique_ptr<char, decltype(&free)> bufferGuard(alignedBuffer, &free);

    // Initialize timing and speed calculation variables
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    const int UPDATE_INTERVAL_MS = 500; // Update every 500ms for smoother readings

    try {
        while (totalWritten < fileSize && !g_operationCancelled) {
            const uint64_t remaining = fileSize - totalWritten;
            const size_t bytesToRead = std::min(bufferSize, static_cast<size_t>(remaining));

            iso.read(alignedBuffer, bytesToRead);
            const std::streamsize bytesRead = iso.gcount();
            
            if (bytesRead <= 0 || static_cast<size_t>(bytesRead) != bytesToRead) {
                throw std::runtime_error("Read error");
            }

            const ssize_t bytesWritten = write(device_fd, alignedBuffer, bytesToRead);
            if (bytesWritten == -1 || static_cast<size_t>(bytesWritten) != bytesToRead) {
                throw std::runtime_error("Write error");
            }

            totalWritten += bytesWritten;
            bytesInWindow += bytesWritten;

            // Update progress and speed
            auto now = std::chrono::high_resolution_clock::now();
            auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
            const int progress = static_cast<int>((static_cast<double>(totalWritten) / fileSize) * 100);

            // Update progress and speed every UPDATE_INTERVAL_MS
            if (timeSinceLastUpdate.count() >= UPDATE_INTERVAL_MS) {
                // Calculate current speed based on bytes written in the last window
                double seconds = timeSinceLastUpdate.count() / 1000.0;
                double mbPerSec = (static_cast<double>(bytesInWindow) / (1024 * 1024)) / seconds;

                std::lock_guard<std::mutex> lock(progressMutex);
                progressData[progressIndex].progress = progress;
                progressData[progressIndex].currentSize = formatFileSize(totalWritten);
                progressData[progressIndex].speed = mbPerSec;
                
                // Reset window counters
                lastUpdate = now;
                bytesInWindow = 0;
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
        close(device_fd);
        return false;
    }

    fsync(device_fd);
    close(device_fd);

    if (!g_operationCancelled && totalWritten == fileSize) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].completed = true;
        return true;
    }
    
    return false;
}
