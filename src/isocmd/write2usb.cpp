#include "../headers.h"

constexpr size_t BUFFER_SIZE = 8 * 1024 * 1024;  // 8MB buffer

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
            std::string devicePrompt = "\n\033[1;94mEnter the block device (e.g., /dev/sdc) or press Enter to quit:\033[0;1m ";
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
            
            uint64_t deviceSize = getBlockDeviceSize(device);
            if (deviceSize == 0) {
				clearScrollBuffer();
                std::cerr << "\n\033[1;91mError: Unable to determine block device size.\033[0;1m\n";
                continue;
            }
            
            validDevice = true;
            double deviceSizeGB = static_cast<double>(deviceSize) / (1024 * 1024 * 1024);
            
            // Display confirmation prompt
            clearScrollBuffer();
            std::cout << "\033[1;94m\nYou are about to write the following ISO to the USB device:\n\n";
            std::cout << "\033[0;1mISO File: \033[1;92m" << isoPath << " (\033[1;95m" << isoFileSizeStr << "\033[0;1m)\n";
            std::cout << "\033[0;1mUSB Device: \033[1;93m" << device << " (\033[1;95m" << std::fixed << std::setprecision(1) << deviceSizeGB << " GBb)\n";
            
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
            
            if (writeIsoToDevice(isoPath, device)) {
                std::cout << "\033[0;1mISO written to device successfully!\n";
            } else {
                std::cerr << "\033[1;91mFailed to write ISO to device.\033[0;1m\n";
            }
            
        } while (!validDevice);
        
    } catch (const std::invalid_argument&) {
        std::cerr << "\033[1;91mError: Input must be a valid integer. Aborting.\033[0;1m\n";
        return;
    } catch (const std::out_of_range&) {
        std::cerr << "\033[1;91mError: Input is out of range. Aborting.\033[0;1m\n";
        return;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "\033[1;91mError: " << e.what() << ". Aborting.\033[0;1m\n";
        return;
    }
}


bool writeIsoToDevice(const std::string& isoPath, const std::string& device) {
    constexpr std::streamsize BUFFER_SIZE = 4096; // Define buffer size

    std::ifstream iso(isoPath, std::ios::binary);
    if (!iso) {
        std::cerr << "Cannot open ISO file: " << isoPath << "\n";
        return false;
    }

    int device_fd = open(device.c_str(), O_WRONLY | O_SYNC);
    if (device_fd == -1) {
        std::cerr << "Cannot open USB device: " << device << "\n";
        return false;
    }

    // Get ISO file size
    iso.seekg(0, std::ios::end);
    std::streamsize fileSize = iso.tellg();
    iso.seekg(0, std::ios::beg);

    if (fileSize <= 0) {
        std::cerr << "Invalid ISO file size: " << fileSize << "\n";
        close(device_fd);
        return false;
    }

    // Buffer for copying
    char buffer[BUFFER_SIZE];
    std::streamsize totalWritten = 0;

    // Write ISO to device
    std::cout << "Writing ISO to device...\n";

    while (totalWritten < fileSize) {
        // Calculate the number of bytes to read in this iteration
        std::streamsize bytesToRead = std::min(BUFFER_SIZE, fileSize - totalWritten);

        // Read data from the ISO file
        iso.read(buffer, bytesToRead);
        std::streamsize bytesRead = iso.gcount();

        if (bytesRead <= 0) {
            std::cerr << "Read error or end of file reached prematurely.\n";
            close(device_fd);
            return false;
        }

        // Write data to the device
        ssize_t bytesWritten = write(device_fd, buffer, bytesRead);
        if (bytesWritten == -1) {
            std::cerr << "Write error: " << strerror(errno) << "\n";
            close(device_fd);
            return false;
        }

        totalWritten += bytesWritten;

        // Show progress
        int progress = (totalWritten * 100) / fileSize;
        std::cout << "\rProgress: " << progress << "%" << std::flush;
    }

    // Ensure all data is written
    fsync(device_fd);
    close(device_fd);

    std::cout << "\nWrite completed successfully!\n";
    return true;
}
