// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Global flag to track cancellation for write2usb
std::atomic<bool> w_cancelOperation(false);

// Signal handler for write2usb
void signalHandlerWrite(int signum) {
    if (signum == SIGINT) {
        w_cancelOperation.store(true);
    }
}


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
    struct udev *udev;
    struct udev_device *dev;
    const char *removable;

    // Create the udev context
    udev = udev_new();
    if (!udev) {
        return false;
    }

    // Extract the device name from the full path
    size_t lastSlash = device.find_last_of('/');
    if (lastSlash == std::string::npos) {
        udev_unref(udev);
        return false;
    }
    std::string deviceName = device.substr(lastSlash + 1);

    // Create a udev device object from the device name
    dev = udev_device_new_from_subsystem_sysname(udev, "block", deviceName.c_str());
    if (!dev) {
        udev_unref(udev);
        return false;
    }

    // Check if the device is removable
    removable = udev_device_get_sysattr_value(dev, "removable");
    if (!removable || std::string(removable) != "1") {
        udev_device_unref(dev);
        udev_unref(udev);
        return false;
    }

    // Traverse the device's parent hierarchy to check if it is connected via USB
    struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
    if (!parent) {
        udev_device_unref(dev);
        udev_unref(udev);
        return false;
    }

    // Clean up
    udev_device_unref(dev);
    udev_unref(udev);

    return true;
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


// Function to prepare writing ISO to usb
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    // Check if the input is a valid integer and contains only digits
    for (char ch : input) {
        if (!isdigit(ch)) {
            clearScrollBuffer();
            std::cerr << "\033[1;91m\nInput must be a valid integer for write2usb.\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }
    
    try {
		// Restore readline autocomplete and screen clear bindings
        rl_bind_key('\f', rl_clear_screen);
		rl_bind_key('\t', rl_complete);
		
        int index = std::stoi(input);
        
        // Ensure the index is within the bounds of the isoFiles vector
        if (index < 1 || static_cast<size_t>(index) > isoFiles.size()) {
			clearScrollBuffer();
            std::cerr << "\n\033[1;91mInvalid input for write2usb.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
        
        std::string isoPath = isoFiles[index - 1];
        uint64_t isoFileSize = std::filesystem::file_size(isoPath);
        
        // Convert ISO file size to MB or GB
        std::string isoFileSizeStr;
        if (isoFileSize < 1024 * 1024 * 1024) {
            uint64_t isoFileSizeMB = isoFileSize / (1024 * 1024);
            isoFileSizeStr = std::to_string(isoFileSizeMB) + " MB";
        } else {
            double isoFileSizeGB = static_cast<double>(isoFileSize) / (1024 * 1024 * 1024);
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << isoFileSizeGB << " GB";
            isoFileSizeStr = oss.str();
        }
        
        // Device selection loop
        bool validDevice = false;
        
        do {
            std::string devicePrompt = "\n\001\033[1;92m\002UsbBlockDevice \001\033[1;94m\002↵ (e.g., /dev/sdc), or ↵ to return:\001\033[0;1m\002 ";
            std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(devicePrompt.c_str()), &std::free);
            
            if (!searchQuery || searchQuery.get()[0] == '\0') {
                return;
            }
            
            std::string device(searchQuery.get());
            add_history(device.c_str());  // Add to readline history
            
            if (!isUsbDevice(device)) {
                std::cout << "\n\033[1;91mError: \033[1;93m" << device << "\033[1;91m is not a USB device.\033[0;1m\n";
                std::cout << "\033[1;92m\n↵ to try again...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                continue;
            }
            
            // Check if device or its partitions are mounted
            if (isDeviceMounted(device)) {
                std::cout << "\n\033[1;91mError: \033[1;93m" << device << "\033[1;91m or its partitions are currently mounted.\033[0;1m\n";
                std::cout << "\033[0;1mPlease unmount all partitions of " << device << " before proceeding.\n";
                std::cout << "\033[1;92m\n↵ to try again...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                clearScrollBuffer();
                continue;
            }
            
            uint64_t deviceSize = getBlockDeviceSize(device);
            if (deviceSize == 0) {
				clearScrollBuffer();
                std::cerr << "\n\033[1;91mError: Unable to determine block device size.\033[0;1m\n";
                continue;
            }
            
            validDevice = true;
            double deviceSizeGB = static_cast<double>(deviceSize) / (1024 * 1024 * 1024);
            clear_history();
            // Display confirmation prompt
            clearScrollBuffer();
            std::cout << "\033[1;94m\nYou are about to write the following ISO file to the USB device:\n\n";
            std::cout << "\033[0;1mISO File: \033[1;92m" << isoPath << "\033[0;1m (\033[1;95m" << isoFileSizeStr << "\033[0;1m)\n";
            std::cout << "\033[0;1mUSB Device: \033[1;93m" << device << " \033[0;1m(\033[1;95m" << std::fixed << std::setprecision(1) << deviceSizeGB << " GBb\033[0;1m)\n";
            
            rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
			rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
            
            std::string confirmation;
			std::string prompt = "\n\033[1;94mAre you sure you want to proceed? (y/n):\033[0;1m ";
            
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
			std::string mainInputString(input.get());

			if (!(mainInputString == "y" || mainInputString == "Y")) {
                std::cout << "\n\033[1;93mOperation aborted by user.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return;
            }
            disableInput();
			std::cout << "\033[0;1m\nWriting: \033[1;92m" << isoPath << "\033[0;1m -> \033[1;93m" << device << "\033[0;1m, \033[1;91mCtrl + c\033[0;1m to cancel\033[0;1m\n";
			// Start time measurement
			auto start_time = std::chrono::high_resolution_clock::now();
			
			bool writeSuccess = writeIsoToDevice(isoPath, device, start_time);
        
			// Restore signal handling to default
			struct sigaction sa;
			sa.sa_handler = SIG_DFL;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = 0;
			sigaction(SIGINT, &sa, nullptr);
        
			if (writeSuccess) {
				std::cout << "\n\033[1;92mWrite2Usb completed successfully!\033[0;1m\n";
			} else if (w_cancelOperation.load()) {
				std::cerr << "\n\n\033[1;93mWrite operation was cancelled...\033[0;1m\n";
			} else {
				std::cerr << "\n\033[1;91mFailed to write ISO file to device.\033[0;1m\n";
			}

			// Flush and Restore input after processing
			flushStdin();
			restoreInput();
        } while (!validDevice);
        
    } catch (const std::invalid_argument&) {
        std::cerr << "\n\033[1;91mError: Input must be a valid integer. Aborting.\033[0;1m\n";
        return;
    } catch (const std::out_of_range&) {
        std::cerr << "\n\033[1;91mError: Input is out of range. Aborting.\033[0;1m\n";
        return;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "\n\033[1;91mError: " << e.what() << ". Aborting.\033[0;1m\n";
        return;
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to async cleanup usb on cancel
void asyncCleanup(int device_fd) {
    // Skip fsync for faster cleanup (if data integrity is not critical)
    //fsync(device_fd);
    close(device_fd);
}


// Function to write ISO to usb device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, const std::chrono::high_resolution_clock::time_point& start_time) {
    constexpr std::streamsize BUFFER_SIZE = 8 * 1024 * 1024; // 8 MB buffer
    constexpr size_t NUM_BUFFERS = 3;  // Number of buffers in rotation
    constexpr size_t ALIGNMENT = 4096;  // Typical page size alignment
    
    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = signalHandlerWrite;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    
    // Reset cancellation flag
    w_cancelOperation.store(false);
    
    // Open input file
    std::ifstream iso(isoPath, std::ios::binary);
    if (!iso) {
        std::cerr << "\n\n\033[1;91mCannot open ISO file: " << isoPath << " (" << strerror(errno) << ")\n";
        return false;
    }
    
    // Open output device with direct I/O
    int device_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (device_fd == -1) {
        std::cerr << "\n\n\033[1;91mCannot open USB device: " << device << " (" << strerror(errno) << ")\n";
        return false;
    }
    
    // Get ISO file size
    std::streamsize fileSize = std::filesystem::file_size(isoPath);
    if (fileSize <= 0) {
        std::cerr << "\n\n\033[1;91mInvalid ISO file size: " << fileSize << "\n";
        close(device_fd);
        return false;
    }
    
    // Create aligned buffers
    std::vector<void*> buffers;
    for (size_t i = 0; i < NUM_BUFFERS; i++) {
        void* aligned_buffer = std::aligned_alloc(ALIGNMENT, BUFFER_SIZE);
        if (!aligned_buffer) {
            for (void* buf : buffers) {
                std::free(buf);
            }
            close(device_fd);
            return false;
        }
        buffers.push_back(aligned_buffer);
    }
    
    // Shared state between threads
    std::queue<std::pair<void*, size_t>> filled_buffers;
    std::mutex mutex;
    std::condition_variable buffer_ready;
    std::atomic<bool> reader_finished(false);
    std::atomic<bool> error_occurred(false);
    std::atomic<std::streamsize> totalWritten(0);
    
    // Reader thread
    auto reader = std::thread([&]() {
        size_t current_buffer = 0;
        std::streamsize total_read = 0;
        
        while (total_read < fileSize && !w_cancelOperation.load() && !error_occurred.load()) {
            std::streamsize bytes_to_read = std::min(BUFFER_SIZE, fileSize - total_read);
            
            // Read into current buffer
            iso.read(static_cast<char*>(buffers[current_buffer]), bytes_to_read);
            std::streamsize bytes_read = iso.gcount();
            
            if (bytes_read <= 0) {
                error_occurred.store(true);
                break;
            }
            
            // Queue the filled buffer
            {
                std::lock_guard<std::mutex> lock(mutex);
                filled_buffers.push({buffers[current_buffer], static_cast<size_t>(bytes_read)});
            }
            buffer_ready.notify_one();
            
            total_read += bytes_read;
            current_buffer = (current_buffer + 1) % NUM_BUFFERS;
        }
        
        reader_finished.store(true);
        buffer_ready.notify_one();
    });
    
    // Writer thread (main thread)
    while (!reader_finished.load() || !filled_buffers.empty()) {
        // Check for cancellation
        if (w_cancelOperation.load()) {
            error_occurred.store(true);
            reader.join();
            std::thread cleanupThread(asyncCleanup, device_fd);
            cleanupThread.detach();
            
            // Cleanup aligned buffers
            for (void* buf : buffers) {
                std::free(buf);
            }
            return false;
        }
        
        std::pair<void*, size_t> current_buffer;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (filled_buffers.empty() && !reader_finished.load()) {
                buffer_ready.wait(lock);
                continue;
            }
            if (filled_buffers.empty()) {
                break;
            }
            current_buffer = filled_buffers.front();
            filled_buffers.pop();
        }
        
        // Write the buffer
        ssize_t bytes_written = write(device_fd, current_buffer.first, current_buffer.second);
        if (bytes_written == -1) {
            std::cerr << "\n\n\033[1;91mWrite error: " << strerror(errno) << "\n";
            error_occurred.store(true);
            reader.join();
            close(device_fd);
            
            // Cleanup aligned buffers
            for (void* buf : buffers) {
                std::free(buf);
            }
            return false;
        }
        
        totalWritten += bytes_written;
        
        // Show progress
        int progress = static_cast<int>((static_cast<double>(totalWritten) / fileSize) * 100);
        std::cout << "\rProgress: " << progress << "%" << std::flush;
    }
    
    reader.join();
    
    // Ensure all data is written
    fsync(device_fd);
    close(device_fd);
    
    // Cleanup aligned buffers
    for (void* buf : buffers) {
        std::free(buf);
    }
    
    if (error_occurred.load()) {
        return false;
    }
    
    // Calculate and print the elapsed time
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    
    std::cout << "\n\n\033[0;1mTotal time taken: " << std::fixed << std::setprecision(1) 
              << total_elapsed_time << " seconds\033[0;1m\n";
    
    return true;
}

