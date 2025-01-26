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


// Function to prepare writing ISO to usb
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    
    // Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
    
    g_operationCancelled = false;
    
    // Validation helper lambda
    auto showErrorAndReturn = [](const std::string& message) {
        std::cerr << "\n\033[1;91m" << message << "\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        clearScrollBuffer();
        return;
    };

    // Input validation
    if (!std::all_of(input.begin(), input.end(), ::isdigit)) {
        showErrorAndReturn("Input must be a valid integer for write.");
        return;
    }

    int index = std::stoi(input);
    if (index < 1 || static_cast<size_t>(index) > isoFiles.size()) {
        showErrorAndReturn("Invalid input for write.");
        return;
    }

    std::string isoPath = isoFiles[index - 1];
    uint64_t isoFileSize = std::filesystem::file_size(isoPath);
    
    // Size formatting
    std::string isoFileSizeStr;
    if (isoFileSize < 1024 * 1024 * 1024) {
		isoFileSizeStr = (std::ostringstream{} << std::fixed << std::setprecision(2) 
        << static_cast<double>(isoFileSize) / (1024 * 1024) << " MB").str();
	} else {
		isoFileSizeStr = (std::ostringstream{} << std::fixed << std::setprecision(2) 
        << static_cast<double>(isoFileSize) / (1024 * 1024 * 1024) << " GB").str();
	}
    
    // Extract filename
    std::string filename = isoPath.substr(isoPath.find_last_of('/') + 1);
    std::string directory = isoPath.substr(0, isoPath.find_last_of('/'));
    
    // Restore readline settings
    rl_bind_key('\f', rl_clear_screen);
    rl_bind_key('\t', rl_complete);

    bool isFinished = false;
    do {
        std::string devicePrompt = "\n-> \001\033[0;1m\002" + directory + "/" "\001\033[1;95m\002" + filename + 
            "\n\n\001\033[1;92m\002RemovableBlockDrive\001\033[1;94m\002 ↵ for " + 
            "\001\033[1;93m\002write\001\033[1;94m\002 (e.g., /dev/sdc), or ↵ to return:\001\033[0;1m\002 ";
            
        std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(devicePrompt.c_str()), &std::free);
        
        if (!searchQuery || searchQuery.get()[0] == '\0') {
            clear_history();
            return;
        }
        
        std::string device(searchQuery.get());
        add_history(device.c_str());

        // Device validation
        if (!isUsbDevice(device) || isDeviceMounted(device)) {
            std::string errorMsg = !isUsbDevice(device) ? 
                "Error: \033[1;93m'" + device + "'\033[1;91m is not a removable drive." :
                "Error: \033[1;93m'" + device + "'\033[1;91m or its partitions are currently mounted. Please unmount all \033[1;93m'" + device + "'\033[1;91m partitions before proceeding.";
            std::cout << "\n\033[1;91m" << errorMsg << "\033[0;1m\n";
            std::cout << "\033[1;92m\n↵ to try again...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }

        uint64_t deviceSize = getBlockDeviceSize(device);
        if (deviceSize == 0) {
            showErrorAndReturn("Error: Unable to determine block device size, cannot proceed. Ensure Root privileges are acquired.");
            continue;
        }

        double deviceSizeGB = static_cast<double>(deviceSize) / (1024 * 1024 * 1024);
        
        // Display confirmation
        clearScrollBuffer();
        std::cout << "\033[1;94m\nThis will \033[1;91m*ERASE*\033[1;94m the removable drive and write to it the selected ISO file:\n\n"
                  << "\033[0;1mISO File: \033[1;92m" << filename << "\033[0;1m (\033[1;95m" << isoFileSizeStr << "\033[0;1m)\n"
                  << "\033[0;1mRemovable Drive: \033[1;93m" << device << " \033[0;1m(\033[1;95m" << std::fixed 
                  << std::setprecision(2) << deviceSizeGB << " GB\033[0;1m)\n";

        rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
        rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
        
        std::unique_ptr<char, decltype(&std::free)> input(readline("\n\033[1;94mProceed? (y/n):\033[0;1m "), &std::free);
        std::string confirmation(input.get());
        
        if (!(confirmation == "y" || confirmation == "Y")) {
            showErrorAndReturn("\033[1;93mWrite operation aborted by user.");
            continue;
        }
        
        if (isoFileSize >= deviceSize) {
            showErrorAndReturn("\033[1;92m'" + filename + "'\033[1;91m cannot fit into \033[1;93m'" + device + "'\033[1;91m aborting...");
            continue;
        }
        
        // First, compute the total visible length of the message
		int total_length = 34 + filename.length() + device.length();

		// Create the underline string with the computed length
		std::string underline(total_length, '_');
        
        // Perform write operation
        disableInput();
        clearScrollBuffer();
        std::cout << "\033[0;1m\nWriting: \033[1;92m" << filename << "\033[0;1m -> \033[1;93m" << device 
                  << "\033[0;1m (\033[1;91mCtrl + c\033[0;1m:cancel)\033[0;1m\n";
		// Print the underline in bold
		std::cout << underline << "\n" << std::endl;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        bool writeSuccess = writeIsoToDevice(isoPath, device, start_time);
        
        if (writeSuccess) {
            std::cout << "\n\033[1;92mISO file written successfully to device!\033[0;1m\n";
        } else if (g_operationCancelled) {
            std::cerr << "\n\n\033[1;93mWrite operation was cancelled...\033[0;1m\n";
        } else {
            std::cerr << "\n\033[1;91mFailed to write ISO file to device.\033[0;1m\n";
        }
        isFinished = true;
        
        flushStdin();
        restoreInput();
    } while (!isFinished);
    
    clear_history();
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to write ISO to usb device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, const std::chrono::high_resolution_clock::time_point& start_time) {
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
                  << formatSize(totalWritten) << "/" << formatSize(fileSize) << ")"
                  << std::flush;
    }
    
    // Ensure all data is written
    fsync(device_fd);
    close(device_fd);
    
    // Calculate and print the elapsed time after flushing is complete
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    std::cout << "\n\n\033[0;1mTotal time taken: " << std::fixed << std::setprecision(1) << total_elapsed_time << " seconds\033[0;1m\n";
    
    return true;
}
