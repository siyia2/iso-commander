// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Function to check if a string starts with '0' for tokenize input
bool startsWithZero(const std::string& str) {
    return !str.empty() && str[0] == '0';
}


// Function to check if a file already exists for conversion output
bool fileExists(const std::string& fullPath) {
        return std::filesystem::exists(fullPath);
}


// Function to check if a string is numeric for tokenize
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// Function to check if a directory input is valid
bool isValidDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}


// Function to check if directory is empty for umount
bool isDirectoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;  // Unable to open directory
    }
    
    errno = 0;  // Reset errno
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 && 
            strcmp(entry->d_name, "..") != 0) {
            closedir(dir);
            return false;  // Found a real entry, directory not empty
        }
    }
    
    // Check if readdir() set errno
    bool isEmpty = (errno == 0);
    closedir(dir);
    return isEmpty;
}


// Function to validate Linux file paths for Cp/Mv FolderPath prompt
bool isValidLinuxPath(const std::string& path) {
    // Check if path starts with a forward slash (absolute path)
    if (path.empty() || path[0] != '/') {
        return false;
    }
    
    // Check for invalid characters in path
    const std::string invalidChars = "|><&*?`$()[]{}\"'\\";
    
    for (char c : invalidChars) {
        if (path.find(c) != std::string::npos) {
            return false;
        }
    }
    
    // Check for control characters
    for (char c : path) {
        if (iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    
    // Avoid paths that are just spaces
    bool isOnlySpaces = true;
    for (char c : path) {
        if (c != ' ' && c != '\t') {
            isOnlySpaces = false;
            break;
        }
    }
    
    if (isOnlySpaces && !path.empty()) {
        return false;
    }
    
    // Check if path exists
    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0) {
        return false; // Path doesn't exist
    }
    
    // Ensure it's a directory
    if (!S_ISDIR(pathStat.st_mode)) {
        return false; // Path exists but is not a directory
    }
    
    return true;
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
