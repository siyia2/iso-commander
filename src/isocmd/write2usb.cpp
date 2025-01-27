// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


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
        oss << std::fixed << std::setprecision(1) 
            << static_cast<double>(size) / 1024 << " KB";
    } else if (size < 1024 * 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) 
            << static_cast<double>(size) / (1024 * 1024) << " MB";
    } else {
        oss << std::fixed << std::setprecision(1) 
            << static_cast<double>(size) / (1024 * 1024 * 1024) << " GB";
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
    bool failed = false;
    bool completed = false;
};


// Shared progress data and mutex for protection
std::mutex progressMutex;
std::vector<ProgressInfo> progressData;


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
        // Device mapping input
        std::string devicePrompt = "\033[0;1m\n";
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
        
        devicePrompt += "\n\001\033[1;92m\002DevicePairing\001\033[1;94m\002 ↵ as \001\033[1;93m\002INDEX>DEVICE\001\033[1;94m\002 (e.g, \001\033[1;94m\0021>/dev/sdc;2>/dev/sdd\001\033[1;94m\002), or ↵ to return:\001\033[0;1m\002 ";

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
            clearScrollBuffer();
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
            clearScrollBuffer();
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
            std::cout << "\n\033[1;93mWrite operation aborted by user\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore();
            clearScrollBuffer();
            continue;
        }

        // Initialize progress tracking
        progressData.clear();
        std::queue<std::pair<size_t, std::pair<std::string, std::string>>> tasks;
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
            tasks.emplace(i, std::make_pair(iso.path, device));
        }

        // Thread pool setup
        std::mutex queueMutex;
        std::atomic<size_t> completed{0};
        std::atomic<size_t> successes{0};

        auto worker = [&]() {
            while (!g_operationCancelled) {
                std::pair<size_t, std::pair<std::string, std::string>> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    if (tasks.empty()) return;
                    task = tasks.front();
                    tasks.pop();
                }

                bool success = writeIsoToDevice(
                    task.second.first,
                    task.second.second,
                    task.first
                );

                if (success) successes++;
                completed++;
            }
        };
        auto startTime = std::chrono::high_resolution_clock::now();

        // Start operations
        disableInput();
        clearScrollBuffer();
        std::cout << "\n\033[0;1mWriting... (\033[1;91mCtrl+c to cancel\033[1;94m)\033[0;1m\n\n";
        std::cout << "\033[s"; // Save cursor position
        
        std::vector<std::thread> threads;
        for (size_t i = 0; i < maxThreads; ++i) {
            threads.emplace_back(worker);
        }

        // Progress display
        auto displayProgress = [&]() {
            while (completed < validPairs.size() && !g_operationCancelled) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
                std::lock_guard<std::mutex> lock(progressMutex);
                std::cout << "\033[u"; // Restore cursor
            
                for (const auto& prog : progressData) {
                    std::string driveName = getDriveName(prog.device);
                    std::cout << "\033[K"; // Clear line
                    std::cout << std::left << std::setw(40) 
                            << ("\033[1;95m" + prog.filename + " \033[0;1m→ " + 
                                "\033[1;93m" + prog.device + "\033[0;1m \033[1;93m<" + driveName + ">\033[0;1m")
                            << std::right << std::setw(5)
                            << (prog.completed ? "\033[1;92m DONE\033[0;1m" :
                                prog.failed ? "\033[1;91m FAIL\033[0;1m" :
                                std::to_string(prog.progress) + "%")
                            << " ["
                            << std::setw(9) << prog.currentSize
                            << "/"
                            << std::setw(9) << prog.totalSize
                            << "]\n";
                }
                std::cout << std::flush;
            }
        };

        std::thread displayThread(displayProgress);

        // Cleanup
        for (auto& t : threads) if (t.joinable()) t.join();
        displayThread.join();

        // Final status
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            endTime - startTime).count();
        
        std::cout << "\n\033[1;94mCompleted: \033[1;92m" << successes 
                  << "\033[1;94m/\033[1;93m" << validPairs.size() 
                  << "\033[1;94m in \033[0;1m" << duration << "s.\033[0;1m\n";
        
        if (g_operationCancelled) {
            std::cout << "\n\033[1;33mOperation cancelled by user.\033[0;1m\n";
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

    // Get file size
    const uint64_t fileSize = std::filesystem::file_size(isoPath);
    const std::string totalSize = formatFileSize(fileSize);
    uint64_t totalWritten = 0;
    int lastReportedProgress = -1;
    constexpr size_t bufferSize = 16 * 1024 * 1024; // 16MB buffer

    // Aligned buffer for O_DIRECT
    const size_t alignment = 4096;
    std::vector<char> buffer(bufferSize + alignment - 1);
    char* alignedBuffer = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(buffer.data()) + alignment - 1) 
        & ~(alignment - 1)
    );

    try {
        while (totalWritten < fileSize && !g_operationCancelled) {
            const auto bytesToRead = static_cast<std::streamsize>(
                std::min(bufferSize, fileSize - totalWritten)
            );
            
            iso.read(alignedBuffer, bytesToRead);
            const std::streamsize bytesRead = iso.gcount();
            
            if (bytesRead <= 0) break;

            const ssize_t bytesWritten = write(device_fd, alignedBuffer, bytesRead);
            if (bytesWritten == -1) throw std::runtime_error("Write error");

            totalWritten += bytesWritten;

            // Update progress if changed
            const int progress = static_cast<int>(
                (static_cast<double>(totalWritten) / fileSize) * 100
            );
            
            if (progress != lastReportedProgress) {
                std::lock_guard<std::mutex> lock(progressMutex);
                progressData[progressIndex].progress = progress;
                progressData[progressIndex].currentSize = formatFileSize(totalWritten);
                lastReportedProgress = progress;
            }
        }
    } catch (...) {
        close(device_fd);
        std::lock_guard<std::mutex> lock(progressMutex);
        progressData[progressIndex].failed = true;
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
