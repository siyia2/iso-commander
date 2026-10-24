// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../inputHandling.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../sort.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../tokenize.h"
#include "../write2usbUI.h"

namespace fs = std::filesystem;

/**
 * @brief Reads the model name of a block device from sysfs.
 *
 * Looks up @c /sys/block/<name>/device/model and trims surrounding whitespace.
 *
 * @param device Absolute path to the block device (e.g. @c /dev/sdc).
 * @return Model name string, or @c "Unknown Drive" if the file is absent or empty.
 */
std::string getDriveName(const std::string& device) {
    std::string deviceName = device.substr(device.find_last_of('/') + 1);
    std::string sysfsPath = "/sys/block/" + deviceName + "/device/model";

    std::ifstream modelFile(sysfsPath);
    std::string driveName;

    if (modelFile.is_open()) {
        std::getline(modelFile, driveName);
        driveName.erase(0, driveName.find_first_not_of(" \t"));
        driveName.erase(driveName.find_last_not_of(" \t") + 1);
    }

    return driveName.empty() ? "Unknown Drive" : driveName;
}

/**
 * @brief Determines whether a block device is a USB flash drive.
 *
 * Uses three complementary heuristics in priority order:
 *  1. Canonical sysfs path contains "/usb"  (strongest — short-circuits).
 *  2. A uevent file in the device tree contains an exact USB bus identifier.
 *  3. USB-specific sysfs attributes (speed, version, manufacturer) exist.
 *
 * The removable sysfs attribute MUST read "1" for the call to return true.
 * If the attribute is unreadable the function returns false (fail-closed).
 *
 * @param devicePath Absolute path to the block device (must start with /dev/).
 * @return true if the device is identified as a removable USB flash device.
 */
bool isUsbDevice(const std::string& devicePath) {
    try {
        // Basic path sanity check
        if (devicePath.size() < 6 || devicePath.substr(0, 5) != "/dev/") {
            return false;
        }

        const std::string deviceName = devicePath.substr(devicePath.find_last_of('/') + 1);

        // Reject empty names or names that look like partition nodes
        if (deviceName.empty() ||
            std::isdigit(static_cast<unsigned char>(deviceName.back()))) {
            return false;
        }

        const std::string sysPath = "/sys/block/" + deviceName;
        if (!fs::exists(sysPath)) {
            return false;
        }

        {
            std::ifstream removableFile(sysPath + "/removable");
            std::string removable;
            if (!removableFile || !std::getline(removableFile, removable)) {
                return false;   // can't confirm removable → reject
            }
            // Trim possible trailing whitespace / newline
            while (!removable.empty() && std::isspace(static_cast<unsigned char>(removable.back()))) {
                removable.pop_back();
            }
            if (removable != "1") {
                return false;
            }
        }

        // --- Heuristic 1: canonical sysfs path contains "/usb" --------------
        // This is the most reliable signal.  Short-circuit immediately so the
        // weaker heuristics don't run unnecessarily.
        {
            std::error_code ec;
            const std::string resolved = fs::canonical(sysPath, ec).string();
            if (!ec && resolved.find("/usb") != std::string::npos) {
                return true;
            }
        }

        // --- Heuristic 2: uevent contains exact USB bus identifier ----------
        //  Only match the key=value pairs that unambiguously indicate a USB storage bus.
        {
            const std::vector<std::string> ueventPaths = {
                sysPath + "/device/uevent",
                sysPath + "/uevent"
            };

            for (const auto& path : ueventPaths) {
                std::ifstream uevent(path);
                std::string line;
                while (std::getline(uevent, line)) {
                    if (line == "ID_BUS=usb" ||
                        line.rfind("DRIVER=usb", 0) == 0) {
                        return true;
                    }
                }
            }
        }

        // --- Heuristic 3: USB-specific sysfs attributes exist ---------------
        // Weakest signal — only reached when the two stronger ones both miss.
        // "speed" and "version" live under the USB interface node; their
        // presence is a reasonable (though not infallible) USB indicator.
        {
            const std::vector<std::string> usbIndicators = {
                sysPath + "/device/speed",
                sysPath + "/device/version",
                sysPath + "/device/manufacturer"
            };

            for (const auto& path : usbIndicators) {
                if (fs::exists(path)) {
                    return true;
                }
            }
        }

        return false;

    } catch (const std::exception&) {
        return false;
    }
}

/**
 * @brief Enumerates removable USB block devices visible in /sys/block.
 *
 * Skips loop devices, RAM disks, zram devices, and any entry whose name
 * ends with a digit (partition suffix). A device is included only when
 * its removable sysfs attribute reads "1" and isUsbDevice() confirms it
 * is USB-attached.
 *
 * @return Vector of absolute device paths such as /dev/sdb.
 */
std::vector<std::string> getRemovableDevices() {
    std::vector<std::string> devices;

    try {
        for (const auto& entry : fs::directory_iterator("/sys/block")) {
            const std::string deviceName = entry.path().filename().string();

            // Skip virtual / pseudo devices by prefix
            if (deviceName.rfind("loop", 0) == 0 ||
                deviceName.rfind("ram",  0) == 0 ||
                deviceName.rfind("zram", 0) == 0) {
                continue;
            }

            // Only skip names that END with a digit (partition suffix e.g. "sda1").
            if (!deviceName.empty() && std::isdigit(static_cast<unsigned char>(deviceName.back()))) {
                continue;
            }

            // Must be marked removable
            std::ifstream removableFile(entry.path() / "removable");
            std::string removable;
            if (!(removableFile >> removable) || removable != "1") {
                continue;
            }

            const std::string devPath = "/dev/" + deviceName;

            // Also filter by USB
            if (isUsbDevice(devPath)) {
                devices.push_back(devPath);
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << '\n';
    }

    return devices;
}

/**
 * @brief Checks whether a block device or any of its partitions is currently mounted.
 *
 * Parses @c /proc/mounts and compares the base device name (without @c /dev/)
 * against each mounted entry, including partition nodes whose names start with
 * the same base and continue with a digit.
 *
 * @param device Absolute path to the block device (e.g. @c /dev/sdb).
 * @return @c true if the device or a partition of it is mounted.
 */
bool isDeviceMounted(const std::string& device) {
    std::ifstream mountsFile("/proc/mounts");
    if (!mountsFile.is_open()) {
        return false;
    }

    std::string line;
    std::string deviceName = device;

    if (deviceName.substr(0, 5) == "/dev/") {
        deviceName = deviceName.substr(5);
    }

    while (std::getline(mountsFile, line)) {
        std::istringstream iss(line);
        std::string mountDevice;
        iss >> mountDevice;

        if (mountDevice.substr(0, 5) == "/dev/") {
            mountDevice = mountDevice.substr(5);
        }

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

/**
 * @brief Validates a set of ISO-to-device mappings and returns only the viable pairs.
 *
 * If @p noPermissions is already set to true on entry, validation is aborted
 * immediately with a "root privileges required" error (see below).
 *
 * Otherwise, each candidate pair is checked in order:
 * -# Device is a removable USB device (via @ref isUsbDevice).
 * -# Device is not currently mounted (via @ref isDeviceMounted).
 * -# Device has no pending kernel-level tasks (not present in @ref g_drainingDevices).
 * -# Device size can be determined; sets @p noPermissions flag to true on failure.
 * -# ISO capacity check (ISO size <= device size).
 *
 * @note If any validation error occurs (including the early privilege check),
 * all errors are printed, signal handlers are temporarily adjusted, and
 * an empty vector is returned to trigger a retry -- even if some pairs
 * would otherwise have been valid. Valid pairs are only returned when
 * *every* candidate passes all checks.
 *
 * @param deviceMap      Pairs of (1-based ISO index, device path) to validate.
 * @param selectedIsos   Ordered list of ISOs corresponding to the indices.
 * @param[in,out] noPermissions On entry, if true, aborts validation
 * immediately with a privilege error. This flag is also used to determine
 * the type of user prompt upon failure.
 *
 * @return A vector of valid (IsoInfo, device) pairs, or an empty vector if
 * validation failed for any pair (or for the early privilege check).
 */
std::vector<std::pair<IsoInfo, std::string>> validateDevices(const std::vector<std::pair<size_t, std::string>>& deviceMap,
                                                            const std::vector<IsoInfo>& selectedIsos, bool& noPermissions) {

    const WriteTheme wt = getWriteTheme();

    std::vector<std::string> validationErrors;
    std::vector<std::pair<IsoInfo, std::string>> validPairs;

    // Early exit check: If no permissions, add the error and skip the loop
    if (noPermissions) {
        validationErrors.push_back(std::string(wt.errLabel) + "Root privileges are required" + wt.rl_resetCol);
    } else {
        for (const auto& devicePair : deviceMap) {
            size_t index = devicePair.first;
            const std::string& device = devicePair.second;
            const auto& iso = selectedIsos[index - 1];

            if (!isUsbDevice(device)) {
                std::string errMsg;
                errMsg.append(wt.errPath).append("'").append(device).append("'")
                      .append(wt.rl_resetCol).append(wt.errLabel).append(" is not a removable USB flash device")
                      .append(wt.rl_resetCol);
                validationErrors.push_back(std::move(errMsg));
                continue;
            }

            if (isDeviceMounted(device)) {
                std::string errMsg;
                errMsg.append(wt.errPath).append("'").append(device).append("'")
                      .append(wt.rl_resetCol).append(wt.errLabel).append(" or its partitions are mounted")
                      .append(wt.rl_resetCol);
                validationErrors.push_back(std::move(errMsg));
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
                if (g_drainingDevices.count(device)) {
                    std::string errMsg;
                    errMsg.append(wt.errPath).append("'").append(device).append("'")
                          .append(wt.rl_resetCol).append(wt.errLabel)
                          .append(" has pending kernel-level tasks")
                          .append(wt.rl_resetCol);
                    validationErrors.push_back(std::move(errMsg));
                    continue;
                }
            }

            uint64_t deviceSize = getBlockDeviceSize(device);
            if (deviceSize == 0) {
                std::string errMsg;
                errMsg.append(wt.errLabel).append("Failed to get size for ")
                      .append(wt.errPath).append("'").append(device).append("'")
                      .append(wt.rl_resetCol);
                validationErrors.push_back(std::move(errMsg));
                continue;
            }

            if (iso.size > deviceSize) {
                std::string deviceSizeStr = formatFileSize(deviceSize);
                std::string driveName = getDriveName(device);
                std::string errMsg;
                errMsg.append(wt.warnLabel).append("'").append(wt.fileCol).append(iso.filename)
                      .append(wt.rl_resetCol).append(UI::Palette::DimGray).append(" (")
                      .append(wt.sizeCol).append(iso.sizeStr).append(wt.rl_resetCol).append(UI::Palette::DimGray)
                      .append(")").append(wt.warnLabel).append("'").append(wt.errLabel).append(" is too large for ")
                      .append(wt.errPath).append("'").append(device).append(wt.infoLabel)
                      .append(" <").append(driveName).append(">")
                      .append(wt.rl_resetCol).append(UI::Palette::DimGray).append(" (")
                      .append(wt.sizeCol).append(deviceSizeStr).append(UI::Palette::DimGray).append(")").append(wt.warnLabel).append("'")
                      .append(wt.rl_resetCol);
                validationErrors.push_back(std::move(errMsg));
                continue;
            }

            validPairs.emplace_back(iso, device);
        }
    }

    // Unified error reporting block
    if (!validationErrors.empty()) {
        std::cerr << "\n" << wt.errLabel << "Validation errors:" << wt.rl_resetCol << wt.bold << "\n";
        for (const auto& err : validationErrors) {
            std::cerr << "  \u2022 " << err << wt.bold << "\n";
        }

        signal(SIGINT, SIG_IGN);
        disable_ctrl_d();
        noPermissions ? (noPermissions = false, pressEnterToContinue()) : pressEnterToTry();
        return {};
    }

    return validPairs;
}

/**
 * @brief Parses a semicolon-delimited mapping string into (index, device) pairs.
 *
 * Expected format: @c "1>/dev/sdb;2>/dev/sdc" where each token is
 * @c INDEX>DEVICE_PATH.  The function enforces:
 *  - Valid @c INDEX>DEVICE format for each token.
 *  - Index within the range @c [1, selectedIsos.size()].
 *  - No device path used more than once.
 *  - Every ISO index in @p selectedIsos has at least one mapping.
 *
 * Errors are appended to @p errors; the caller inspects this vector to decide
 * whether to re-prompt the user.
 *
 * @param pairString   Raw user input string containing semicolon-separated mappings.
 * @param selectedIsos Ordered list of ISO paths used to validate index bounds.
 * @param[out] errors  Vector that will be cleared and populated with human-readable error messages.
 * @return Vector of valid (1-based index, device path) pairs extracted from the input.
 */
std::vector<std::pair<size_t, std::string>> parseDeviceMappings(const std::string& pairString,
                                                                const std::vector<std::string>& selectedIsos,
                                                                std::vector<std::string>& errors) {

    std::vector<std::pair<size_t, std::string>> deviceMap;
    std::unordered_set<std::string> usedDevices;
    std::istringstream pairStream(pairString);
    std::string pair;

    const WriteTheme wt = getWriteTheme();

    errors.clear();

    while (std::getline(pairStream, pair, ';')) {
        pair.erase(pair.find_last_not_of(" \t\n\r\f\v") + 1);
        pair.erase(0, pair.find_first_not_of(" \t\n\r\f\v"));

        if (pair.empty()) continue;

        size_t sepPos = pair.find('>');
        if (sepPos == std::string::npos) {
            errors.push_back(wt.warnLabel + "Invalid pair format: '" + wt.errLabel + pair +  wt.warnLabel + "'");
            continue;
        }

        std::string indexStr = pair.substr(0, sepPos);
        std::string device = pair.substr(sepPos + 1);

        try {
            size_t index = std::stoul(indexStr);

            if (index < 1 || index > selectedIsos.size()) {
                errors.push_back(wt.warnLabel + "Invalid index '" +  wt.errLabel +  indexStr + wt.warnLabel + "'");
                continue;
            }

            if (usedDevices.count(device)) {
                errors.push_back(wt.warnLabel + "Device '" + wt.errLabel + device +  wt.warnLabel + "' used multiple times");
                continue;
            }

            deviceMap.emplace_back(index, device);
            usedDevices.insert(device);

        } catch (...) {
            errors.push_back(wt.warnLabel + "Invalid index: '" + wt.errLabel + indexStr +  wt.warnLabel + "'");
        }
    }

    std::unordered_set<size_t> mappedIndices;
    for (const auto& [index, device] : deviceMap) {
        mappedIndices.insert(index);
    }

    for (size_t i = 1; i <= selectedIsos.size(); ++i) {
        if (mappedIndices.find(i) == mappedIndices.end()) {
            errors.push_back(wt.warnLabel + "Missing mapping for ISO '" + wt.errLabel + std::to_string(i) + wt.warnLabel + "'");
        }
    }

    return deviceMap;
}

void helpMappings();
void displayErrors();

/**
 * @brief Interactive loop that collects and validates ISO-to-device mappings from the user.
 *
 * Displays the sorted list of selected ISOs (largest first) alongside detected
 * removable USB devices, then reads mapping input via readline.  The loop
 * continues until the user provides a valid, confirmed set of mappings or
 * explicitly cancels with @c "\x1b" Esc.
 *
 * Readline tab-completion is configured for both ISO indices and device paths.
 * A @c "?" input triggers the help screen.  After a valid mapping is entered,
 * the user is shown a destructive-write warning and must confirm with @c y/Y.
 *
 * Detected devices are annotated with status indicators:
 * - @b [MOUNTED] — device or a partition is currently mounted; cannot be written to.
 * - @b [FLUSHING]   — device was part of a cancelled operation and is still draining
 *                  in the background; proceeding is possible but may race with cleanup.
 *
 * @param selectedIsos ISOs chosen by the user for writing.
 * @return Validated (IsoInfo, device) pairs ready for @ref performWriteOperation,
 *         or an empty vector if the user aborted, or if the number of selected
 *         ISOs exceeds the thread pool size.
 */
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos) {
    PendingReadlineTextClearGuard pendingTextGuard;
    const WriteTheme wt = getWriteTheme();

    while (true) {
        TerminalStateGuard termGuard;

        ThreadPool& pool = getStaticThreadPool();
        if (selectedIsos.size() > pool.threadCount()) {
            std::cout << "\n"
                      << wt.colorFailure << "Error: "
                      << UI::Palette::Yellow << "write2usb"
                      << wt.colorFailure << " selection limit" << wt.colorFailure << " is "
                      << wt.colorWarning << pool.threadCount()
                      << wt.colorFailure << " (thread pool size)."
                      << wt.speedCol << "\n";

            pressEnterToTry();
            return {};  // termGuard destructor restores readline state
        }

        displayErrors();

        std::vector<IsoInfo> sortedIsos = selectedIsos;
        std::sort(sortedIsos.begin(), sortedIsos.end(), [](const IsoInfo& a, const IsoInfo& b) {
            return a.size > b.size;
        });

        std::ostringstream devicePromptStream;
        devicePromptStream << "\n" << color << "Selected " << wt.fileCol << "ISO" << color << ":\n\n";

        for (size_t i = 0; i < sortedIsos.size(); ++i) {
            auto [isoDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write2usb");

            devicePromptStream << "  " << wt.indexCol << (i + 1) << ">" << wt.bold << " ";

            if (!displayConfig::toggleNamesOnly) {
                devicePromptStream << wt.pathCol << isoDir << "/";
            }

            devicePromptStream << wt.fileCol << filename;

            devicePromptStream << UI::Palette::DimGray << " (" << wt.sizeCol << sortedIsos[i].sizeStr
                              << UI::Palette::DimGray << ")\n";
        }

        devicePromptStream << "\n" << color << "Removable USB Flash Devices:" << wt.rl_resetCol << "\n\n";
        std::vector<std::string> usbDevices = getRemovableDevices();

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

        std::sort(deviceInfos.begin(), deviceInfos.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
            if (a.error != b.error) return !a.error;
            return a.size > b.size;
        });

        if (deviceInfos.empty()) {
            devicePromptStream << "  " << wt.colorFailure << "No USB flash drives detected!" << wt.rl_resetCol << "\n";
        } else {
            for (const auto& dev : deviceInfos) {
                if (dev.error) {
                    devicePromptStream << "  " << wt.colorFailure << dev.path << " (error)" << wt.rl_resetCol << "\n";
                } else {
                    devicePromptStream << "  " << wt.deviceCol << dev.path
                                      << wt.infoLabel << " <" << dev.driveName
                                      << ">" << UI::Palette::DimGray << " (" << wt.sizeCol << dev.sizeStr
                                      << UI::Palette::DimGray << ")"
                                      << ([&]() -> std::string {
                                          if (dev.mounted) return wt.colorFailure + " [MOUNTED]" + wt.rl_resetCol;
                                          std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
                                          if (g_drainingDevices.count(dev.path))
                                              return wt.colorFailure + " [FLUSHING]" + wt.rl_resetCol;
                                          return "";
                                      }())
                                      << "\n";
                }
            }
        }

        g_completerData.sortedIsos = &sortedIsos;
        g_completerData.usbDevices = &usbDevices;
        reset_custom_keybindingsForCpMvWrite2Usb();

        devicePromptStream << "\n" << UI::Palette::RL_idxGreen << "IDX>DEVICE"
                          << wt.rl_primaryCol << " ↵ for "
                          << UI::Palette::RL_Yellow << "write2usb"
                          << wt.rl_primaryCol << ", ? for help: "
                          << wt.rl_resetCol;

        std::string devicePrompt = devicePromptStream.str();

        armPendingInputRestore();
        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );
        rl_startup_hook = nullptr;


        if (!deviceInput) {
            return {};  // termGuard destructor restores readline state
        }

        if (deviceInput.get()[0] == '\0') {
            continue;  // termGuard destructor restores, next iteration re-acquires
        }

        std::string mainInputString(deviceInput.get());
        if (mainInputString == "\x1b") {
            return {};  // termGuard destructor restores readline state
        }

        if (mainInputString == "?") {
            helpMappings();
            continue;
        }

        if (deviceInput && *deviceInput) add_history(deviceInput.get());

        std::vector<std::string> errors;
        std::vector<std::string> isoFilenames;
        for (const auto& iso : sortedIsos) {
            isoFilenames.push_back(iso.path);
        }

        auto deviceMap = parseDeviceMappings(deviceInput.get(), isoFilenames, errors);

        if (!errors.empty()) {
            std::cerr << "\n" << wt.rl_errorCol << "Errors:" << wt.rl_resetCol << "\n";
            for (const auto& err : errors) {
                std::cerr << color << "  • " << err << "\n";
            }

            pressEnterToTry();
            continue;
        }
        // Check for Root privileges
        bool noPermissions = (geteuid() != 0);
        auto validPairs = validateDevices(deviceMap, sortedIsos, noPermissions);
        if (validPairs.empty()) {
            continue;
        }

        // Warning message with colors from theme
        std::cout << "\n" << wt.colorWarning << "WARNING: This will "
                  << UI::Palette::Red << "*ERASE ALL DATA*"
                  << wt.colorWarning << " on:" << wt.rl_resetCol << "\n\n";

        for (const auto& [iso, device] : validPairs) {
            uint64_t deviceSize = getBlockDeviceSize(device);
            std::string deviceSizeStr = formatFileSize(deviceSize);
            std::string driveName = getDriveName(device);

            std::cout << UI::Palette::DimGray << "  {" << wt.deviceCol << device
                      << " " << wt.infoLabel << "<" << driveName << ">" << UI::Palette::DimGray << " ("
                      << wt.sizeCol << deviceSizeStr
                      << UI::Palette::DimGray << ")} " << color << "←" << UI::Palette::DimGray << " {" << wt.fileCol
                      << iso.filename << UI::Palette::DimGray << " (" << wt.sizeCol
                      << iso.sizeStr << UI::Palette::DimGray << ")}\n";
        }

        // Constructing the Readline prompt using theme colors
        const std::string confirmPrompt =
            "\001" + std::string(color) + "\002" +
            "\nProceed? (y/n): "
            + wt.rl_resetCol;

        std::unique_ptr<char, decltype(&std::free)> confirmation(nullptr, &std::free);
        {
            ConfirmationModeGuard confirmGuard;
            confirmation.reset(readline(confirmPrompt.c_str()));
        }
        // confirmGuard destructor has already re-applied the mappings-prompt
        // readline configuration here.

        if (confirmation && (confirmation.get()[0] == 'y' || confirmation.get()[0] == 'Y')) {
            setupSignalHandlerCancellations();
            GlobalState::g_operationCancelled.store(false);
            return validPairs;  // termGuard destructor restores readline state
        }

        // "no" -> loop continues; termGuard destructor restores readline state
    }
}

/**
 * @brief Stable-sorts validated (IsoInfo, device) pairs and builds the matching
 *        per-task ProgressInfo vector, in one pass.
 *
 * Behaves exactly like the former sortPairsByFilename() followed by
 * buildProgressData(), but produces both results together:
 *
 *  - @c pairs    — a freshly allocated, stable-sorted copy of @p validPairsIn.
 *                  Never touches the input. Primary key is IsoInfo::filename via
 *                  naturalCompare (case-insensitive, numeric-aware); tiebreak is
 *                  the device path under ordinary std::string ordering.
 *  - @c progress — one ProgressInfo per sorted pair, same order and size, so that
 *                  task index i, progressData[i] and display row i refer to the
 *                  same pair. Reserved to exactly the input size before filling,
 *                  so the backing storage never moves after construction.
 *
 * Both vectors are shared_ptr-owned because the same instances are captured by
 * every pooled write task and by the cancellation drain; the data must outlive
 * this call whenever a task or drain thread still needs it.
 */
struct SortedWriteData {
    std::shared_ptr<std::vector<std::pair<IsoInfo, std::string>>> pairs;
    std::shared_ptr<std::vector<ProgressInfo>> progress;
};

static SortedWriteData prepareWriteData(
    const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn)
{
    auto pairsPtr =
        std::make_shared<std::vector<std::pair<IsoInfo, std::string>>>(validPairsIn);

    std::stable_sort(pairsPtr->begin(), pairsPtr->end(),
        [](const auto& a, const auto& b) {
            const int c = naturalCompare(a.first.filename, b.first.filename);
            if (c != 0) return c < 0;
            return a.second < b.second;
        });

    auto progressPtr = std::make_shared<std::vector<ProgressInfo>>();
    auto& progress   = *progressPtr;
    progress.reserve(pairsPtr->size());   // no reallocation after this point

    for (const auto& [iso, device] : *pairsPtr)
        progress.push_back(ProgressInfo{iso.filename, device, iso.sizeStr});

    return {std::move(pairsPtr), std::move(progressPtr)};
}

/**
 * @brief Detach in-flight write tasks into a background drain thread after cancellation.
 *
 * Called at the tail of performWriteOperation() when the user has interrupted the
 * operation via Ctrl+C (GlobalState::g_operationCancelled). Its job is to move the
 * still-running futures out of the local function scope into a detached background
 * thread, so that any hardware tasks still flushing cache data to a device can
 * finish safely *after* control has been returned to the shell prompt.
 *
 * The detached thread waits on each future in turn and, as each one completes,
 * erases its device from the global g_drainingDevices set. This lets the
 * "[FLUSHING]" indicator in the UI clear per-device as soon as that device has
 * finished draining, rather than waiting for all of them. When the set becomes
 * empty, g_drainingCancelled is cleared so the indicator disappears entirely.
 *
 * Only devices whose tasks had not yet completed or failed at the moment of
 * cancellation are added to g_drainingDevices — tasks that had already returned
 * from writeIsoToDevice() are known to be flush-clean and are skipped.
 *
 * The non-copyable, non-movable progressData vector is deliberately *not* moved
 * or captured by value; instead the shared_ptr aliases are captured so the
 * per-operation data stays alive for as long as the detached thread needs it.
 *
 * @param futures        Pending per-task futures. Taken by value and moved into the
 *                       detached thread, so the caller's vector is left empty after
 *                       this call. Must not be used by the caller afterwards.
 * @param validPairs     Device/ISO pairs for this operation, in the same index order
 *                       as @p progressData. Used to enumerate device names for the
 *                       draining set. Not captured by the detached thread.
 * @param progressData   Per-task progress records, indexed identically to
 *                       @p validPairs. Read only: .completed / .failed are inspected
 *                       to decide which devices still need draining. Not captured by
 *                       the detached thread (only the pointer is).
 * @param sortedPairsPtr Shared ownership of the sorted pair list; captured by the
 *                       detached thread to keep the operation data alive until the
 *                       drain completes.
 * @param progressDataPtr Shared ownership of the progress records; captured by the
 *                       detached thread for the same reason.
 *
 * @note The caller must not touch @p futures after this returns — it has been
 *       moved from. This is intentional: the whole point is to transfer ownership
 *       of the pending work out of the local scope.
 *
 * @note Must only be called on the cancellation path. The non-cancelled path in
 *       performWriteOperation waits on the futures inline instead, because nothing
 *       needs to outlive the function in that case.
 *
 * @see g_drainingDevices, g_drainingCancelled, g_drainingManager
 */
void handleOutOfRangeBackgroundDrainingForWrite2Usb(
    std::vector<std::future<void>> futures,
    const std::vector<std::pair<IsoInfo, std::string>>& validPairs,
    const std::vector<ProgressInfo>& progressData,
    std::shared_ptr<std::vector<std::pair<IsoInfo, std::string>>> sortedPairsPtr,
    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr)
{
    // 5. OOR (Out-Of-Range) BACKGROUND DRAINING:
    // If the process was canceled via Ctrl+C, move ONLY the active futures out of the
    // local function context into a safe, detached background engine.
    // This allows background hardware tasks to finish flushing cache data safely
    // without copying or moving the non-copyable 'progressData' structure.
    // Each device is erased from the dirty-device set as its own future completes,
    // allowing the [FLUSHING] indicator to clear per-device as soon as draining finishes.
    std::vector<std::string> drainDevices;
    drainDevices.reserve(validPairs.size());

    bool anyDraining = false;
    {
        std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
        for (size_t i = 0; i < validPairs.size(); ++i) {
            const std::string& device = validPairs[i].second;
            drainDevices.push_back(device);

            // Only devices whose task was still actively running when
            // cancellation hit need draining. Completed and failed tasks
            // have already returned from writeIsoToDevice and are flush-clean.
            if (!progressData[i].completed.load() && !progressData[i].failed.load()) {
                g_drainingDevices.insert(device);
                anyDraining = true;
            }
        }
    }
    g_drainingCancelled.store(anyDraining);

    g_drainingManager.add(std::thread([
        capturedFutures = std::move(futures),
        devices = std::move(drainDevices),
        sortedPairsPtr,
        progressDataPtr
    ]() mutable {
        for (size_t i = 0; i < capturedFutures.size(); ++i) {
            if (capturedFutures[i].valid()) {
                capturedFutures[i].wait();
            }

            std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
            g_drainingDevices.erase(devices[i]); // no-op if never inserted

            if (g_drainingDevices.empty()) {
                g_drainingCancelled.store(false);
            }
        }
    }));
}

/**
 * @brief Enqueues ISO-to-device write tasks and drives a live progress display.
 *
 * The input pairs are first copied and sorted alphabetically (case-insensitive) by
 * ISO filename via @ref prepareWriteData, with the device path as a tie-break when
 * the same ISO targets several devices. Task index, @ref progressData index and display
 * row all share this sorted order, so the live progress tracks always appear
 * alphabetically by ISO name.
 *
 * For each validated (IsoInfo, device) pair, in sorted order:
 * - A @ref ProgressInfo entry is appended to @ref progressData by @ref
 *   prepareWriteData (reserved up front, one entry per task, same order as the
 *   sorted pairs)
 * - Device names, sizes, and formatted size strings are cached once per device
 * - One @ref writeIsoToDevice call per task is submitted to the global @ref ThreadPool
 *
 * A background thread redraws per-task status, waking immediately whenever a task
 * completes (via a condition variable notified from each pooled task) and otherwise
 * falling back to a 500 ms pulse if nothing new has happened, using ANSI cursor
 * save/restore (\033[s / \033[u). The main thread waits on the same condition
 * variable to detect overall completion, also waking instantly on task completion.
 *
 * @par Structure
 * The body is split into two phases so the invariants are named once and reused:
 * -# @ref prepareWriteData produces, in one pass, a @c std::shared_ptr to a
 *    stable-sorted copy of the input and a @c std::shared_ptr to a parallel vector
 *    of @ref ProgressInfo entries, one per sorted pair and in the same order (task
 *    index @c i matches both vectors and the display row). Both shared pointers are
 *    captured by every pooled task and by the cancellation drain, so the data
 *    outlives this function if needed.
 * -# The remainder of the function wires up the thread pool, the render loop, the
 *    completion condition variable, and the final status/summary print.
 *
 * @par Ordering
 * Rows are sorted alphabetically by ISO filename (ASCII case-insensitive, byte-wise),
 * with the device path as a secondary key. The sort is stable and performed once on a
 * local copy of the input by @ref prepareWriteData, so the caller's vector is never
 * modified. The pooled tasks share the sorted copy through a @c std::shared_ptr, which
 * keeps it alive for any detached drain that outlives this function after cancellation.
 *
 * @par Filename Truncation
 * ISO names are shown in full unless the widest row (full names, device, drive name,
 * bar, status and speed columns) would not fit the current terminal width. Only then
 * is every name longer than 15 columns (".iso" included) truncated: a truncated name
 * keeps the first 10 stem columns, followed by "…" (U+2026, one column) and the
 * untouched ".iso" extension, so it is always exactly 10 + 1 + 4 = 15 columns, e.g.
 * "archlinux-2024.10.01-x86_64.iso" becomes "archlinux-….iso". The extension is
 * therefore always visible. Names of 15 columns or fewer are always shown in full.
 * Names without a ".iso" suffix are treated the same way, with the whole name as the
 * stem.
 * The decision is made once per render for all rows together, so either every long
 * name is truncated or none is, and the columns stay aligned. The terminal width is
 * re-queried on every render, so resizing the terminal takes effect on the next
 * redraw (within 500 ms). If the width cannot be determined (stdout is not a tty),
 * 80 columns is assumed. The width check reserves room for the " (avg)" suffix, so
 * the decision does not flip when tasks complete.
 * Truncation is UTF-8 safe (never splits a multi-byte sequence) and affects the
 * display only; the real path passed to @ref writeIsoToDevice is untouched.
 * Filename padding is computed from the displayed widths so columns stay aligned;
 * ASCII-only columns (device path, drive name) are padded through the shared
 * @c padRight helper.
 *
 * @par Redraw and Terminal Resize
 * Each frame is built as one string and written once. Rows are never allowed to wrap:
 * the last terminal column is kept free and any row still too wide after truncation is
 * clipped (ANSI-escape and UTF-8 aware). Instead of saving/restoring an absolute cursor
 * position (which goes stale when a terminal re-wraps its content on resize), the
 * previous frame's row widths are remembered; on the next frame the number of physical
 * lines they now occupy at the current width is computed, the cursor moves up exactly
 * that far, and the screen is cleared from there down before redrawing. This assumes
 * the terminal re-wraps already-printed lines when resized, as most modern emulators do.
 *
 * @par Row Layout
 * Each row prints seamlessly side-by-side:
 * @code
 * filename → /dev/sdX <drive name> [▇▇▄░░] percentage speed
 * @endcode
 * For example (with a terminal too narrow for the full name):
 * @code
 * archlinux-….iso → /dev/sdd <SanDisk 3.2> [▇▇▇▇▄] 92%   3.1 MB/s
 * @endcode
 * The device path and the drive name in angle brackets are each padded to the widest value
 * across all rows, so the progress bars line up in a single column.
 *
 * @par Progress Bar
 * The bar is 5 character cells wide but has 10 steps of 10% each: every cell can be
 * empty (░), half filled (▄, 4/8 height) or full (▇, 7/8 height). Both filled glyphs are
 * bottom-aligned and stop short of the cell top, so bars on consecutive rows never touch.
 * The step count is floored from the percentage, so the bar advances one step each time
 * progress reaches another multiple of 10%, and is completely full only at 100%.
 *
 * @par Status Tokens
 * - @b DONE — Task completed successfully (appends " (avg)" to final speed readout).
 * - @b FAIL — Task encountered an error during execution.
 * - @b CXL  — Task execution or cleanup was interrupted by user cancellation.
 * - @b %    — Live percentage value representing active transfer progress.
 *
 * @par Cancellation (Ctrl+C / SIGINT)
 * When @ref GlobalState::g_operationCancelled is tripped:
 * -# The main thread's condition-variable wait (shared with the display thread)
 *    re-checks @ref GlobalState::g_operationCancelled at least every 500 ms and
 *    breaks as soon as it observes the flag set.
 * -# The live UI render thread is flagged, notified via condition variable to wake
 *    from any in-progress wait, and joined immediately.
 * -# Terminal properties, standard input buffers, and layout configurations are restored @b instantly.
 * -# Operating system control is returned to the user's shell prompt without visual lag.
 * -# @b Out-of-Band Draining: Ownership of the still-running @c std::future objects,
 *    together with the shared sorted pair vector and progress vector, is transferred
 *    to @ref handleOutOfRangeBackgroundDrainingForWrite2Usb, which moves them into a
 *    detached background thread. This allows OS page caches (e.g. buffered NTFS
 *    flushes) to drain onto the hardware safely in the background, entirely avoiding
 *    stack-use-after-free crashes. Each device is removed from the global
 *    dirty-device set as its own drain completes, allowing the device selection
 *    screen to clear the @b [FLUSHING] indicator per-device as soon as it is safe.
 *
 * @par Normal Completion
 * The main thread's condition-variable wait unblocks once every task has finished
 * (succeeded or failed), the UI rendering loop is closed natively, a final static
 * frame update is committed, and lingering thread handles are cleanly joined. On this
 * path the futures are simply joined inline; the out-of-band drain helper is not invoked.
 *
 * @par Summary Line
 * On execution exit, the primary cursor moves up to print an operational status overview
 * derived from a single load of the completion counter (the value is reused for both the
 * status decision and the final successful-count line):
 * - @b COMPLETED — All targeted block operations succeeded.
 * - @b PARTIAL   — At least one task succeeded and at least one task failed.
 * - @b FAILED    — Total failure across all enqueued storage block streams.
 * - @b INTERRUPTED — Operation aborted by explicit user termination before total completion.
 *
 * Final execution metrics (elapsed duration and a successful/total count ratio) are printed below.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs targeted for writing. Copied and
 *                     sorted alphabetically by ISO filename (via @ref prepareWriteData)
 *                     before processing; the caller's vector is left untouched.
 */
void performWriteOperation(const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn) {
    // One call produces both the stable-sorted pairs and the matching
    // ProgressInfo vector; the ordering invariant (task i ↔ pair i ↔
    // progress row i) is established by prepareWriteData().
    auto writeData              = prepareWriteData(validPairsIn);
    const auto& sortedPairsPtr  = writeData.pairs;
    const auto& progressDataPtr = writeData.progress;

    const auto& validPairs = *sortedPairsPtr;  // Rest of the function uses this name.
    auto& progressData     = *progressDataPtr;

    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> finishedTasks(0);   // success OR failure — drives "all done" detection
    std::atomic<bool> isProcessingComplete(false);
    const size_t totalTasks = validPairs.size();

    // Condition variable used to wake the display thread the instant a task
    // completes, instead of waiting for its next 500ms poll tick. The
    // completion check is folded into the wait predicate (evaluated while
    // holding progressCvMutex) so there is no window between "check" and
    // "block" where a notify_one() from a finishing task could be missed.
    std::mutex progressCvMutex;
    std::condition_variable progressCv;

    ThreadPool& pool = getStaticThreadPool();

    disableInput();
    clearScrollBuffer();

    const WriteTheme wt = getWriteTheme();

    std::cout << "\n" << color << "Processing "
            << (totalTasks > 1 ? "tasks" : "task") << " for "
            << UI::Palette::Yellow << "write2usb" << color
            << "... (" << UI::Palette::Red << "Ctrl+c"
            << color << ":cancel)\n\n";

    auto startTime = std::chrono::high_resolution_clock::now();

    std::unordered_map<std::string, std::string> deviceNames;
    std::unordered_map<std::string, uint64_t> deviceSizes;
    std::unordered_map<std::string, std::string> deviceSizeStrs;

    for (const auto& prog : progressData) {
        if (deviceNames.find(prog.device) == deviceNames.end()) {
            deviceNames[prog.device] = getDriveName(prog.device);
            deviceSizes[prog.device] = getBlockDeviceSize(prog.device);
            deviceSizeStrs[prog.device] = formatFileSize(deviceSizes[prog.device]);
        }
    }

    // Visible width (in columns) of every row printed by the previous frame. Used to
    // work out how far to move the cursor back up, even if the terminal was resized
    // (and therefore re-wrapped the old rows) in between.
    std::vector<size_t> lastRowWidths;

    auto displayAllProgress = [&]() {
        constexpr size_t MAX_NAME_LEN = 20;
        constexpr size_t MAX_FILE_LEN = 15;   // names up to this many columns (with ".iso") are never truncated
        constexpr size_t STEM_KEEP = 10;      // stem columns kept when truncating, followed by "…" and ".iso" (10 + 1 + 4 = 15)
        constexpr int BAR_WIDTH = 5;               // character cells
        constexpr int BAR_STEPS = BAR_WIDTH * 2;   // each cell has 2 states (half / full) -> 10 steps, 10% each

        // Set once per render, after the full (untruncated) row width is known.
        // While false, shortFilename() returns names unchanged.
        bool truncateNames = false;

        // Current terminal width in columns; 80 if it cannot be determined
        // (e.g. stdout is not a tty).
        auto terminalColumns = []() -> size_t {
            struct winsize ws{};
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
                return ws.ws_col;
            return 80;
        };

        auto shortName = [&](const std::string& dev) -> std::string {
            const std::string& n = deviceNames[dev];
            if (n.size() <= MAX_NAME_LEN) return n;

            // Cut at most MAX_NAME_LEN - 1 bytes, but back up so we never split a
            // multi-byte UTF-8 sequence (continuation bytes look like 10xxxxxx).
            size_t cut = MAX_NAME_LEN - 1;
            while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;

            std::string out;
            out.reserve(cut + 3);                   // kept bytes + 3-byte "…"
            out.append(n, 0, cut);
            out.append("\u2026");
            return out;
        };

        // Counts visible characters (UTF-8 code points) instead of bytes,
        // so the "…" counts as 1 column rather than 3.
        auto displayWidth = [](const std::string& s) {
            size_t w = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) != 0x80) ++w;
            return w;
        };

        // Right-pads an ASCII string to the given byte width. Used for device
        // paths and other ASCII-only columns.
        auto padRight = [](std::string s, size_t target) {
            if (s.size() < target) s.append(target - s.size(), ' ');
            return s;
        };

        // Visible width of a string that may contain ANSI CSI escape sequences
        // (colors etc.): escape sequences count as 0 columns.
        auto visibleWidth = [](const std::string& s) -> size_t {
            size_t w = 0;
            for (size_t i = 0; i < s.size(); ) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
                    i += 2;
                    while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
                    if (i < s.size()) ++i;   // final byte of the sequence
                    continue;
                }
                if ((c & 0xC0) != 0x80) ++w;
                ++i;
            }
            return w;
        };

        // Cuts a string that may contain ANSI escape sequences to at most maxCols
        // visible columns. Escape sequences are passed through untouched and UTF-8
        // code points are never split.
        auto clipToWidth = [](const std::string& s, size_t maxCols) -> std::string {
            std::string out;
            out.reserve(s.size());
            size_t w = 0;
            for (size_t i = 0; i < s.size(); ) {
                if (static_cast<unsigned char>(s[i]) == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
                    size_t j = i + 2;
                    while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j;
                    if (j < s.size()) ++j;
                    out.append(s, i, j - i);
                    i = j;
                    continue;
                }
                size_t len = 1;
                while (i + len < s.size() &&
                    (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
                if (w >= maxCols) break;
                out.append(s, i, len);
                i += len;
                ++w;
            }
            return out;
        };

        // Truncates ISO filenames, but only when truncateNames is set (the terminal
        // is too narrow for the full-width rows) AND the whole name (".iso" included)
        // is longer than MAX_FILE_LEN columns. A truncated name shows exactly
        // STEM_KEEP stem columns, then "…" (1 column), then the ".iso" extension, e.g.
        // "archlinux-….iso", so a truncated name is always exactly 15 columns and the
        // extension is always visible.
        // Names of MAX_FILE_LEN columns or fewer are shown in full. Walks whole UTF-8
        // code points so a multi-byte sequence is never split.
        auto shortFilename = [&](const std::string& f) -> std::string {
            if (!truncateNames) return f;   // terminal is wide enough: show the full name

            // Split off a trailing ".iso" (case-insensitive), if present.
            std::string ext;
            std::string stem = f;
            if (f.size() >= 4) {
                const std::string tail = f.substr(f.size() - 4);
                if (tail.size() == 4 && tail[0] == '.' &&
                    std::tolower(static_cast<unsigned char>(tail[1])) == 'i' &&
                    std::tolower(static_cast<unsigned char>(tail[2])) == 's' &&
                    std::tolower(static_cast<unsigned char>(tail[3])) == 'o') {
                    ext  = tail;
                    stem = f.substr(0, f.size() - 4);
                }
            }

            if (displayWidth(f) <= MAX_FILE_LEN) return f;

            std::string out;
            size_t cols = 0;
            for (size_t i = 0; i < stem.size() && cols < STEM_KEEP; ) {
                size_t len = 1;
                while (i + len < stem.size() &&
                    (static_cast<unsigned char>(stem[i + len]) & 0xC0) == 0x80) ++len;
                out.append(stem, i, len);
                i += len;
                ++cols;
            }
            out.append("…");
            out.append(ext);
            return out;
        };

        size_t maxFilenameWidth     = 0;
        size_t maxFullFilenameWidth = 0;   // widest name with no truncation applied
        size_t maxDeviceLen         = 0;
        size_t maxNameWidth         = 0;
        size_t maxSpeedWidth        = 0;

        // Speed strings are formatted once per render so the column can be
        // right-aligned to the widest value (the "(avg)" suffix is appended
        // after the padded value and does not affect alignment).
        std::vector<std::string> speedStrs;
        speedStrs.reserve(progressData.size());

        for (const auto& prog : progressData) {
            maxFullFilenameWidth = std::max(maxFullFilenameWidth, displayWidth(prog.filename));
            maxDeviceLen         = std::max(maxDeviceLen,         prog.device.size());
            maxNameWidth         = std::max(maxNameWidth,         displayWidth(shortName(prog.device)));

            speedStrs.push_back(formatSpeed(prog.speed));
            maxSpeedWidth        = std::max(maxSpeedWidth,        displayWidth(speedStrs.back()));
        }

        // Width of everything except the filename column:
        //   " → " (3) + device + " <name>" (maxNameWidth + 3) + " [" (2) + bar (5)
        //   + "] " (2) + status (5) + speed + " (avg)" (6, always counted as the
        //   worst case so the truncation decision doesn't flip when tasks complete).
        const size_t fixedWidth = 3 + maxDeviceLen + (maxNameWidth + 3) + 2 + BAR_WIDTH
                                + 2 + 5 + maxSpeedWidth + 6;

        // Keep the last column free so a row can never trigger the terminal's
        // auto-wrap, which would break the cursor-up redraw below.
        const size_t cols       = terminalColumns();
        const size_t usableCols = cols > 1 ? cols - 1 : 1;

        // Truncate only if the widest full-width row would overflow the terminal.
        truncateNames = (fixedWidth + maxFullFilenameWidth) > usableCols;

        for (const auto& prog : progressData)
            maxFilenameWidth = std::max(maxFilenameWidth, displayWidth(shortFilename(prog.filename)));

        // The whole frame is built in one string and written once (less flicker).
        // The previous frame was printed at some earlier width, and terminals re-wrap
        // already-printed lines when resized, so a row that took one line may now
        // take several. Work out how many physical lines the old rows occupy at the
        // current width, move up exactly that far, and clear to the end of the screen.
        // Relative movement is used instead of \033[s / \033[u because a saved
        // absolute cursor position goes stale when the terminal re-wraps.
        std::string frame;
        {
            size_t up = 0;
            for (size_t w : lastRowWidths)
                up += std::max<size_t>(1, (w + cols - 1) / cols);
            if (up > 0) frame += "\033[" + std::to_string(up) + "A";
            frame += "\r\033[J";
        }
        lastRowWidths.clear();
        lastRowWidths.reserve(progressData.size());

        for (size_t i = 0; i < progressData.size(); ++i) {
            const auto& prog = progressData[i];

            const std::string fname = shortFilename(prog.filename);
            std::string paddedFilename = fname;
            paddedFilename.append(maxFilenameWidth - displayWidth(fname), ' ');

            const std::string paddedDevice = padRight(prog.device, maxDeviceLen);

            const std::string name = shortName(prog.device);
            const std::string namePad(maxNameWidth - displayWidth(name), ' ');

            // Half-block bar: BAR_WIDTH cells, BAR_STEPS steps (10% each).
            // Each cell is empty (░), half (▄) or full (▇). Both filled glyphs are
            // bottom-aligned and stop short of the cell top, so bars on consecutive
            // rows never touch.
            int pct = std::clamp(prog.progress.load(), 0, 100);
            int steps = (pct * BAR_STEPS) / 100;   // 0..BAR_STEPS
            int full  = steps / 2;
            bool half = (steps % 2) != 0;

            std::string bar, empty;
            for (int j = 0; j < full; ++j) bar += "▇";
            if (half)                      bar += "▄";   // 4/8 height, bottom-aligned like ▇
            for (int j = 0; j < BAR_WIDTH - full - (half ? 1 : 0); ++j) empty += "░";

            std::ostringstream row;
            row << wt.fileCol << paddedFilename
                << color << " → " << wt.deviceCol << paddedDevice
                << color << " " << wt.infoLabel << "<" << name << ">" << namePad
                << color << " ["
                << bar << wt.speedCol << empty
                << color << "] ";

            bool isCompleted = prog.completed.load();

            if (isCompleted)                                    row << wt.colorSuccess << "DONE ";
            else if (prog.failed.load())                        row << wt.colorFailure << "FAIL ";
            else if (GlobalState::g_operationCancelled.load())  row << wt.colorWarning << "CXL  ";
            else {
                std::string pctStr = std::to_string(pct) + "%";
                pctStr.resize(5, ' ');
                row << color << pctStr;
            }

            const std::string& speedStr = speedStrs[i];
            const std::string speedPad(maxSpeedWidth - displayWidth(speedStr), ' ');

            row << color << speedStr << speedPad;
            if (isCompleted) row << " (avg)";

            // Last line of defense: if the terminal is too narrow even for the
            // truncated row, cut the row instead of letting it wrap.
            const std::string line = clipToWidth(row.str(), usableCols);
            lastRowWidths.push_back(std::min(visibleWidth(line), usableCols));
            frame += line;
            frame += "\033[0m\n";
        }
        std::cout << frame << std::flush;
    };

    // UI render thread: wakes immediately on a completion notification
    // (predicate-based wait_for closes the check-then-block race) and
    // otherwise falls back to a 500ms pulse, matching the original pacing
    // when nothing new has happened.
    bool isFirstUpdate = true;
    auto displayProgress = [&]() {
        while (!isProcessingComplete.load(std::memory_order_acquire) &&
            !GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {

            // Check if all tasks are finished (succeeded or failed)
            if (finishedTasks.load() == totalTasks) {
                // Force one final render and exit the display thread
                displayAllProgress();
                isProcessingComplete.store(true, std::memory_order_release);
                break;
            }

            if (!isFirstUpdate) {
                std::unique_lock<std::mutex> lock(progressCvMutex);
                progressCv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                    return isProcessingComplete.load(std::memory_order_acquire) ||
                        GlobalState::g_operationCancelled.load(std::memory_order_acquire) ||
                        finishedTasks.load() == totalTasks;
                });
                lock.unlock();

                // Double check flags right after waking to prevent stale prints on quick exit
                if (isProcessingComplete.load(std::memory_order_acquire) ||
                    GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
                    break;
                }
            }
            isFirstUpdate = false;

            displayAllProgress();
        }
    };

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < totalTasks; ++i) {
        // Both shared pointers keep their per-operation data alive,
        // including while tasks are drained after cancellation.
        futures.push_back(pool.enqueue([
            sortedPairsPtr,
            progressDataPtr,
            i,
            &completedTasks,
            &finishedTasks,
            &progressCv,
            &progressCvMutex
        ]() {
            const auto& [iso, device] = (*sortedPairsPtr)[i];
            const bool success = writeIsoToDevice(
                iso.path, device, i, *progressDataPtr);

            auto& progress = (*progressDataPtr)[i];

            if (success) {
                progress.completed.store(true);
                completedTasks.fetch_add(1);
            } else if (!GlobalState::g_operationCancelled.load()) {
                progress.failed.store(true);
            }

            finishedTasks.fetch_add(1);

            // Wake the display thread immediately instead of waiting for
            // its next 500ms poll tick. Acquiring/releasing the mutex here,
            // *after* the state updates above and *before* notify_one,
            // establishes the happens-before relationship the predicate-based
            // wait_for on the other side relies on to never miss this wakeup.
            {
                std::lock_guard<std::mutex> lock(progressCvMutex);
            }
            progressCv.notify_one();
        }));
    }

    std::thread progressThread(displayProgress);

    // Wait for all tasks to finish, woken instantly by each task's
    // notify_one() rather than polling each future individually. Still
    // time-boxed to a 500ms cycle (not an unbounded wait) because
    // g_operationCancelled is set from a SIGINT handler, which cannot
    // safely call notify_one() — so cancellation must also be picked up
    // by periodic re-check, not solely by notification.
    {
        std::unique_lock<std::mutex> lock(progressCvMutex);
        while (finishedTasks.load() != totalTasks &&
            !GlobalState::g_operationCancelled.load()) {
            progressCv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return finishedTasks.load() == totalTasks ||
                    GlobalState::g_operationCancelled.load();
            });
        }
    }

    // 1. Instantly trip the display thread break condition flags
    isProcessingComplete.store(true, std::memory_order_release);
    progressCv.notify_one(); // wake progressThread immediately if it's mid-wait
    signal(SIGINT, SIG_IGN);
    if (progressThread.joinable()) {
        progressThread.join();
    }

    // 2. Force one final static render pass to commit final output matrix states cleanly
    displayAllProgress();

    std::cout << "\033[s";
    std::cout << "\033[2H\033[2K";

    size_t failedTasksValue = 0;
    for (const auto& prog : progressData) {
        if (prog.failed.load()) {
            failedTasksValue++;
        }
    }

    size_t completedTasksValue = completedTasks.load();
    std::string operation = std::string(UI::Palette::Yellow) + "write2usb" + wt.rl_resetCol;

    // 3. Print operational results summaries directly
    std::cout << "\r" << color << "Status: " << operation << color <<" → "
            << (!GlobalState::g_operationCancelled.load()
                ? (failedTasksValue > 0
                    ? (completedTasksValue > 0
                        ? wt.colorWarning + "PARTIAL"
                        : wt.colorFailure + "FAILED")
                    : wt.colorSuccess + "COMPLETED")
                : wt.colorWarning + "INTERRUPTED")
            << wt.rl_resetCol << std::endl;

    std::cout << "\033[u";

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(endTime - startTime).count();

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n" << color << "Successful: "
            << wt.colorSuccess << completedTasksValue
            << color << "/"
            << wt.colorWarning << validPairs.size()
            << color << " | Time Elapsed: "
            << color << duration << "s"
            << wt.colorStatus << UI::Palette::Reset << "\n";

    // 4. Instantly hand focus and console command lines back to your user's shell prompt
    flushStdin();
    restoreInput();

    // On cancellation the Drain manager for write2usb takes over
    if (GlobalState::g_operationCancelled.load()) {
        handleOutOfRangeBackgroundDrainingForWrite2Usb(
            std::move(futures),
            validPairs,
            progressData,
            sortedPairsPtr,
            progressDataPtr);
    } else {
        // Normal fast block cleanup sequence path if everything finished successfully
        for (auto& future : futures) {
            if (future.valid()) {
                future.wait();
            }
        }
    }
}

/**
 * @brief Entry point for the write-to-USB workflow.
 *
 * This function orchestrates the full flashing process:
 * 1. Parses the raw @p input string into unique ISO indices.
 * 2. Resolves filesystem metadata (size, name) for each selected ISO,
 *    skipping any files that are inaccessible or no longer present.
 * 3. Prompts the user for target device mappings via @ref collectDeviceMappings.
 * 4. Executes the flashing process via @ref performWriteOperation.
 *
 * @param input    Raw selection string from the main menu (e.g. "1 3-5").
 * @param isoFiles Full ordered list of available ISO paths.
 */
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    std::unordered_set<int> indicesToProcess;

    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    tokenizeInput(input, isoFiles, indicesToProcess);
    if (indicesToProcess.empty()) {
        return;
    }

    std::vector<IsoInfo> selectedIsos;
    for (int idx : indicesToProcess) {
        const std::string& path = isoFiles[idx - 1];
        std::filesystem::path fsPath(path);
        std::error_code ec;
        auto size = std::filesystem::file_size(fsPath, ec);
        if (ec) continue;
        selectedIsos.emplace_back(IsoInfo{
            path,
            fsPath.filename().string(),
            size,
            formatFileSize(size),
            static_cast<size_t>(idx)
        });
    }

    if (selectedIsos.empty()) {
        return;
    }

    auto validPairs = collectDeviceMappings(selectedIsos);
    if (validPairs.empty()) {
        return;
    }

    performWriteOperation(validPairs);

    // Cleanup and wait for user acknowledgment
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    pressEnterToContinue();
}
