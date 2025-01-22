// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Global flag to track cancellation for write2usb
std::atomic<bool> g_cancelOperation(false);

// Signal handler for write2usb
void signalHandlerWrite(int signum) {
    if (signum == SIGINT) {
        g_cancelOperation.store(true);
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
    
    // Check if the input is a valid integer and contains only digits
    for (char ch : input) {
        if (!isdigit(ch)) {
            clearScrollBuffer();
            std::cerr << "\033[1;91m\nInput must be a valid integer for write.\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }
    
    // Restore readline autocomplete and screen clear bindings
    rl_bind_key('\f', rl_clear_screen);
    rl_bind_key('\t', rl_complete);
    
    int index = std::stoi(input);
    
    // Ensure the index is within the bounds of the isoFiles vector
    if (index < 1 || static_cast<size_t>(index) > isoFiles.size()) {
        clearScrollBuffer();
        std::cerr << "\n\033[1;91mInvalid input for write.\033[0;1m\n";
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
    
    // Find the position of the last '/'
    size_t lastSlashPos = isoPath.find_last_of('/');

    // Extract everything after the last '/'
    std::string filename;
    if (lastSlashPos != std::string::npos) {
        filename = isoPath.substr(lastSlashPos + 1);
    } else {
        // If there's no '/', the entire string is the filename
        filename = isoPath;
    }

    bool isFinished = false;
    // Device selection loop    
    do {
        std::string devicePrompt = "\n\001\033[1;92m\002RemovableBlockDrive \001\033[1;94m\002↵ (e.g., /dev/sdc), or ↵ to return:\001\033[0;1m\002 ";
        std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(devicePrompt.c_str()), &std::free);
        
        if (!searchQuery || searchQuery.get()[0] == '\0') {
			clear_history();
            return;
        }
        
        std::string device(searchQuery.get());
        add_history(device.c_str());  // Add to readline history
        
        if (!isUsbDevice(device)) {
            std::cout << "\n\033[1;91mError: \033[1;93m'" << device << "'\033[1;91m is not a removable drive.\033[0;1m\n";
            std::cout << "\033[1;92m\n↵ to try again...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }
        
        // Check if device or its partitions are mounted
        if (isDeviceMounted(device)) {
            std::cout << "\n\033[1;91mError: \033[1;93m'" << device << "'\033[1;91m or its partitions are currently mounted.\033[0;1m\n";
            std::cout << "\n\033[0;1mPlease unmount all '\033[1;93m" << device << "\033[0;1m' partitions before proceeding.\n";
            std::cout << "\033[1;92m\n↵ to try again...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }
        
        uint64_t deviceSize = getBlockDeviceSize(device);
        if (deviceSize == 0) {
            std::cerr << "\n\033[1;91mError: Unable to determine block device size, cannot proceed. Ensure Root privileges are acquired.\033[0;1m\n";
            std::cout << "\033[1;92m\n↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();
            continue;
        }
        
        double deviceSizeGB = static_cast<double>(deviceSize) / (1024 * 1024 * 1024);
        
        // Display confirmation prompt
        clearScrollBuffer();
        std::cout << "\033[1;94m\nThis will \033[1;91m*ERASE*\033[1;94m the removable drive and write to it the selected ISO file:\n\n";
        std::cout << "\033[0;1mISO File: \033[1;92m" << filename << "\033[0;1m (\033[1;95m" << isoFileSizeStr << "\033[0;1m)\n";
        std::cout << "\033[0;1mRemovable Drive: \033[1;93m" << device << " \033[0;1m(\033[1;95m" << std::fixed << std::setprecision(1) << deviceSizeGB << " GBb\033[0;1m)\n";
        
        rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
        rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
        
        std::string confirmation;
        std::string prompt = "\n\033[1;94mProceed? (y/n):\033[0;1m ";
        
        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string mainInputString(input.get());
        
        if (!(mainInputString == "y" || mainInputString == "Y")) {
            std::cout << "\n\033[1;93mWrite operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();  // Clear the screen before looping back
			continue;
        }
        
        if ((mainInputString == "y" || mainInputString == "Y") && isoFileSize >= deviceSize) {
            std::cout << "\n\033[1;92m'" << filename << "' \033[1;91mcannot fit into \033[1;93m'" << device << "'\033[1;91m aborting...\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            clearScrollBuffer();  // Clear the screen before looping back
			continue;
        }
        
        disableInput();
        std::cout << "\033[0;1m\nWriting: \033[1;92m" << filename << "\033[0;1m -> \033[1;93m" << device << "\033[0;1m || \033[1;91mCtrl + c\033[0;1m to cancel\033[0;1m\n";
        
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
            std::cout << "\n\033[1;92mISO file written successfully to device!\033[0;1m\n";
            isFinished = true;
        } else if (g_cancelOperation.load()) {
            std::cerr << "\n\n\033[1;93mWrite operation was cancelled...\033[0;1m\n";
            isFinished = true;
        } else {
            std::cerr << "\n\033[1;91mFailed to write ISO file to device.\033[0;1m\n";
            isFinished = true;
        }
        
        // Flush and Restore input after processing
        flushStdin();
        restoreInput();
    } while (!isFinished);
    
    clear_history();
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to write ISO to usb device
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, const std::chrono::high_resolution_clock::time_point& start_time) {
    constexpr std::streamsize BUFFER_SIZE = 8 * 1024 * 1024; // 8 MB buffer

    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = signalHandlerWrite;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // Reset cancellation flag
    g_cancelOperation.store(false);

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
        if (g_cancelOperation.load()) {
            close(device_fd);
            return false;
        }

        std::streamsize bytesToRead = std::min(BUFFER_SIZE, fileSize - totalWritten);
        iso.read(alignedBuffer, bytesToRead);
        std::streamsize bytesRead = iso.gcount();

        if (bytesRead <= 0) {
            std::cerr << "\n\n\033[1;91mRead error or end of file reached prematurely.\n";
            close(device_fd);
            return false;
        }

        ssize_t bytesWritten = write(device_fd, alignedBuffer, bytesRead);
        if (bytesWritten == -1) {
            std::cerr << "\n\n\033[1;91mWrite error: " << strerror(errno) << ".\n";
            close(device_fd);
            return false;
        }

        totalWritten += bytesWritten;

        // Show progress
        int progress = static_cast<int>((static_cast<double>(totalWritten) / fileSize) * 100);
        std::cout << "\rProgress: " << progress << "%" << std::flush;

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
