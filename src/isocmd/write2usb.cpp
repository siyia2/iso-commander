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


struct IsoInfo {
    std::string path;
    std::string filename;
    uint64_t size;
    std::string sizeStr;
    size_t originalIndex;
};


// Function to prepare writing ISO to usb
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    setupSignalHandlerCancellations();
    g_operationCancelled = false;

    // Parse ISO selection
    std::vector<size_t> isoIndices;
    try {
        isoIndices = parseIsoSelection(input, isoFiles.size());
    } catch (const std::exception& e) {
        std::cerr << "\033[1;91mError: " << e.what() << "\033[0m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Collect selected ISOs with original indices
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
        // Device input prompt
        std::string devicePrompt = "\n\033[1;94mAvailable ISO selections:\033[0m\n";
        for (size_t i = 0; i < selectedIsos.size(); ++i) {
            devicePrompt += "  \033[1;93m" + std::to_string(i+1) + ">\033[0m " +
                          selectedIsos[i].filename + " (\033[1;95m" + 
                          selectedIsos[i].sizeStr + "\033[0m)\n";
        }
        devicePrompt += "\n\033[1;94mEnter device mappings as \033[1;93mINDEX>DEVICE\033[1;94m separated by ';'\n"
                      "Example: \033[1;93m1>/dev/sdc;2>/dev/sdd\033[1;94m\n"
                      "\033[1;92m↵ to write\033[0m, \033[1;91m↵ to cancel:\033[0m ";

        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );

        if (!deviceInput || deviceInput.get()[0] == '\0') {
            clear_history();
            return;
        }

        // Parse device pairs
        std::unordered_map<size_t, std::string> deviceMap;
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
                errors.push_back("Invalid pair format: '" + pair + "' - use INDEX>DEVICE");
                continue;
            }

            std::string indexStr = pair.substr(0, sepPos);
            std::string device = pair.substr(sepPos + 1);

            // Validate index
            try {
                size_t index = std::stoul(indexStr);
                if (index < 1 || index > selectedIsos.size()) {
                    errors.push_back("Invalid index " + indexStr + 
                                   " - valid range: 1-" + 
                                   std::to_string(selectedIsos.size()));
                    continue;
                }

                // Check duplicate index
                if (deviceMap.count(index)) {
                    errors.push_back("Duplicate index " + indexStr);
                    continue;
                }

                // Check device usage
                if (usedDevices.count(device)) {
                    errors.push_back("Device " + device + " used multiple times");
                    continue;
                }

                deviceMap[index] = device;
                usedDevices.insert(device);
            } catch (...) {
                errors.push_back("Invalid index format: '" + indexStr + "'");
            }
        }

        // Validate all ISOs are mapped
        if (deviceMap.size() != selectedIsos.size()) {
            errors.push_back("Missing mappings for " + 
                std::to_string(selectedIsos.size() - deviceMap.size()) + " ISO(s)");
        }

        // Show parsing errors
        if (!errors.empty()) {
            std::cerr << "\n\033[1;91mMapping errors:\033[0m\n";
            for (const auto& err : errors) {
                std::cerr << "  • " << err << "\n";
            }
            std::cout << "\n\033[1;92m↵ to try again...\033[0m";
            std::cin.ignore();
            clearScrollBuffer();
            continue;
        }

        // Validate devices
        std::vector<std::pair<IsoInfo, std::string>> validPairs;
        std::vector<std::string> validationErrors;

        for (const auto& [index, device] : deviceMap) {
            const auto& iso = selectedIsos[index - 1];

            if (!isUsbDevice(device)) {
                validationErrors.push_back("\033[1;91m" + device + " is not a removable drive");
                continue;
            }
            
            if (isDeviceMounted(device)) {
                validationErrors.push_back("\033[1;91m" + device + " has mounted partitions");
                continue;
            }

            uint64_t deviceSize = getBlockDeviceSize(device);
            if (deviceSize == 0) {
                validationErrors.push_back("\033[1;91mFailed to get size for " + device);
                continue;
            }

            if (iso.size >= deviceSize) {
                validationErrors.push_back("\033[1;91m" + iso.filename + " (" + iso.sizeStr + 
                               ") too large for " + device + " (" + 
                               formatFileSize(deviceSize) + ")");
                continue;
            }

            validPairs.emplace_back(iso, device);
        }

        if (!validationErrors.empty()) {
            std::cerr << "\n\033[1;91mDevice validation errors:\033[0m\n";
            for (const auto& err : validationErrors) {
                std::cerr << "  • " << err << "\033[0m\n";
            }
            std::cout << "\n\033[1;92m↵ to try again...\033[0m";
            std::cin.ignore();
            clearScrollBuffer();
            continue;
        }

        // Confirmation prompt
        std::cout << "\n\033[1;91mWARNING: This will ERASE ALL DATA on:\033[0m\n";
        for (const auto& [iso, device] : validPairs) {
            std::cout << "  \033[1;93m" << device << "\033[0m ← \033[1;92m" 
                     << iso.filename << " \033[0m(\033[1;95m" << iso.sizeStr << "\033[0m)\n";
        }

        std::unique_ptr<char, decltype(&std::free)> confirmation(
            readline("\n\033[1;94mProceed? (y/N): \033[0m"), &std::free
        );

        if (!confirmation || 
            (confirmation.get()[0] != 'y' && confirmation.get()[0] != 'Y')) {
            std::cout << "\033[1;93mOperation cancelled\033[0m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0m";
            std::cin.ignore();
            clearScrollBuffer();
            continue;
        }

        // Prepare task queue
        struct WriteTask {
            std::string isoPath;
            std::string device;
            std::string filename;
        };

        std::queue<WriteTask> taskQueue;
        for (const auto& [iso, device] : validPairs) {
            taskQueue.push({iso.path, device, iso.filename});
        }

        // Thread pool setup
        std::mutex coutMutex;
        std::mutex queueMutex;
        std::condition_variable cv;
        std::atomic<size_t> completed(0);
        std::atomic<size_t> successes(0);
        
        auto startTime = std::chrono::high_resolution_clock::now();

        auto worker = [&]() {
            while (!g_operationCancelled) {
                WriteTask task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    if (taskQueue.empty()) return;
                    task = taskQueue.front();
                    taskQueue.pop();
                }

                bool success = false;
                
                try {
                    success = writeIsoToDevice(task.isoPath, task.device);
                } catch (...) {
                    success = false;
                }

                {
                    std::lock_guard<std::mutex> lock(coutMutex);
                    if (success) {
                        std::cout << "\033[0;1m | \033[1;92mSuccess: " << task.device << "\033[0;1m\n";
                        successes++;
                    } else if (g_operationCancelled) {
                        std::cout << "\033[0;1m | \033[1;93mCancelled: " << task.device << "\033[0;1m\n";
                    } else {
                        std::cout << "\033[0;1m | \033[1;91mFailed: " << task.device << "\033[0;1m\n";
                    }
                }

                completed++;
            }
        };

        // Start workers
        disableInput();
        clearScrollBuffer();
        std::cout << "\033[1;94mStarting write operations (\033[1;91mCtrl+C to cancel\033[1;94m)\033[0;1m\n";

        std::vector<std::thread> pool;
        for (size_t i = 0; i < maxThreads; ++i) {
            pool.emplace_back(worker);
        }

        // Wait for completion
        while (completed < validPairs.size() && !g_operationCancelled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        cv.notify_all();

        for (auto& t : pool) {
            if (t.joinable()) t.join();
        }

        // Show summary
        std::cout << "\n\033[1;94mCompleted: \033[1;92m" << successes 
                 << "\033[1;94m/\033[1;93m" << validPairs.size() 
                 << "\033[1;94m devices\033[0m\n";
        
        isFinished = true;
        // Calculate and print the elapsed time after flushing is complete
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - startTime).count();
    std::cout << "\n\033[0;1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
        flushStdin();
        restoreInput();
    } while (!isFinished);

    std::cout << "\n\033[1;32m↵ to continue...\033[0m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to write ISO to usb device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device) {
    // Size formatting lambda
    auto formatSize = [](size_t bytes) -> std::string {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }
        
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << size << units[unit];
        return stream.str();
    };
    
    // Find the position of the last '/'
    size_t lastSlashPos = isoPath.find_last_of('/');
    
    // Extract everything after the last '/'
    std::string filename;
    if (lastSlashPos != std::string::npos) {
        filename = isoPath.substr(lastSlashPos + 1);
    } else {
        // If there is no '/', the entire string is the filename
        filename = isoPath;
    }

    constexpr std::streamsize BUFFER_SIZE = 8 * 1024 * 1024; // 8 MB buffer
    
    // Open ISO file
    std::ifstream iso(isoPath, std::ios::binary);
    if (!iso) {
        std::cerr << "\n\n\033[1;91mCannot open ISO file: '\033[1;93m" << isoPath << "'\033[1;91m (" << strerror(errno) << ").\n";
        return false;
    }
    
    // Open the device with O_DIRECT
    int device_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (device_fd == -1) {
        std::cerr << "\n\n\033[1;91mCannot open removable drive: '\033[1;93m" << device << "'\033[1;91m (" << strerror(errno) << ").\n";
        return false;
    }
    
    // Get ISO file size
    std::streamsize fileSize = std::filesystem::file_size(isoPath);
    if (fileSize <= 0) {
        std::cerr << "\n\n\033[1;91mInvalid ISO file size: '\033[1;93m" << fileSize << "'\033[1;91m.\n";
        close(device_fd);
        return false;
    }
    
    // Allocate aligned buffer for O_DIRECT
    const size_t alignment = 4096; // Typical block size for O_DIRECT
    std::vector<char> buffer(BUFFER_SIZE + alignment - 1);
    char* alignedBuffer = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(buffer.data()) + alignment - 1) & ~(alignment - 1));
    
    std::streamsize totalWritten = 0;
    while (totalWritten < fileSize) {
        // Check for cancellation
        if (g_operationCancelled) {
            close(device_fd);
            return false;
        }
        
        // Read into buffer
        std::streamsize bytesToRead = std::min(BUFFER_SIZE, fileSize - totalWritten);
        iso.read(alignedBuffer, bytesToRead);
        std::streamsize bytesRead = iso.gcount();
        
        if (bytesRead <= 0) {
            std::cerr << "\n\n\033[1;91mRead error or end of file reached prematurely.\n";
            close(device_fd);
            return false;
        }
        
        // Write to device
        ssize_t bytesWritten = write(device_fd, alignedBuffer, bytesRead);
        if (bytesWritten == -1) {
            std::cerr << "\n\n\033[1;91mWrite error: " << strerror(errno) << ".\n";
            close(device_fd);
            return false;
        }
        
        totalWritten += bytesWritten;
        
        // Show progress
        int progress = static_cast<int>((static_cast<double>(totalWritten) / fileSize) * 100);
        std::cout << "\033[1K"; // ANSI escape sequence to clear the rest of the line
        std::cout << "\rProgress: " << progress << "% (" 
                  << formatSize(totalWritten) << "/" << formatSize(fileSize) << ") \033[1;92m" << filename << "\033[0;1m -> \033[1;93m" << device << "\033[0;1m"
                  << std::flush;
    }
    
    // Ensure all data is written
    fsync(device_fd);
    close(device_fd);
    
    return true;
}
