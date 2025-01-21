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


void processToken(const std::string& input, std::vector<std::string>& isoFiles) {
    clearScrollBuffer();

    // Check if the input is a valid integer
    try {
        int index = std::stoi(input);
        
        // Ensure the index is within the bounds of the isoFiles vector
        if (index < 1 || static_cast<size_t>(index) > isoFiles.size()) {
            std::cerr << "Error: Invalid index. Aborting.\n";
            return;
        }

        // Get the ISO file path
        std::string isoPath = isoFiles[index - 1]; // Adjust for 0-based indexing

        // Get the ISO file size in bytes
        uint64_t isoFileSize = std::filesystem::file_size(isoPath);

        // Convert ISO file size to MB or GB based on its size
        std::string isoFileSizeStr;
        if (isoFileSize < 1024 * 1024 * 1024) { // If size is less than 1 GB
            uint64_t isoFileSizeMB = isoFileSize / (1024 * 1024); // Convert to MB
            isoFileSizeStr = std::to_string(isoFileSizeMB) + " MB";
        } else { // If size is 1 GB or larger
            double isoFileSizeGB = static_cast<double>(isoFileSize) / (1024 * 1024 * 1024); // Convert to GB
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << isoFileSizeGB << " GB"; // Format to 1 decimal place
            isoFileSizeStr = oss.str();
        }

        // Ask for the block device
        std::string device;
        std::cout << "Enter the block device (e.g., /dev/sdc): ";
        std::cin >> device;

        // Get the block device size in bytes
        uint64_t deviceSize = getBlockDeviceSize(device);
        if (deviceSize == 0) {
            std::cerr << "Error: Unable to determine block device size. Aborting.\n";
            return;
        }
        double deviceSizeGB = static_cast<double>(deviceSize) / (1024 * 1024 * 1024); // Convert to GB

        // Display confirmation prompt
        std::cout << "\nYou are about to write the following ISO to the block device:\n";
        std::cout << "ISO File: " << isoPath << " (" << isoFileSizeStr << ")\n";
        std::cout << "Block Device: " << device << " (" << std::fixed << std::setprecision(1) << deviceSizeGB << " GB)\n";
        std::cout << "Are you sure you want to proceed? (y/n): ";

        // Get user confirmation
        std::string confirmation;
        std::cin >> confirmation;

        if (confirmation != "y") {
            std::cout << "Operation aborted by user.\n";
            return;
        }
		
        if (writeIsoToDevice(isoPath, device)) {
            std::cout << "ISO written to device successfully.\n";
        } else {
            std::cerr << "Failed to write ISO to device.\n";
        }

    } catch (const std::invalid_argument&) {
        std::cerr << "Error: Input must be a valid integer. Aborting.\n";
        return;
    } catch (const std::out_of_range&) {
        std::cerr << "Error: Input is out of range. Aborting.\n";
        return;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error: " << e.what() << ". Aborting.\n";
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
