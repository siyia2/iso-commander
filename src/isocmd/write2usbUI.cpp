// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../inputHandling.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../tokenize.h"
#include "../write2usb.h"

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
 * Each candidate pair is checked in order:
 *  -# Device is a removable USB device (via @ref isUsbDevice).
 *  -# Device is not currently mounted (via @ref isDeviceMounted).
 *  -# Device size can be determined; sets @p permissions flag to true on failure.
 *  -# ISO capacity check (ISO size <= device size).
 *
 * @note If any validation error occurs, all errors are printed, signal handlers are
 *       temporarily adjusted, and an empty vector is returned to trigger a retry.
 *
 * @param deviceMap      Pairs of (1-based ISO index, device path) to validate.
 * @param selectedIsos   Ordered list of ISOs corresponding to the indices.
 * @param[in,out] permissions Set to true if a size query fails (permissions issue);
 *                            used to determine the type of user prompt.
 * @return A vector of valid (IsoInfo, device) pairs, or an empty vector if validation failed.
 */
std::vector<std::pair<IsoInfo, std::string>> validateDevices(const std::vector<std::pair<size_t, std::string>>& deviceMap,
                                                             const std::vector<IsoInfo>& selectedIsos, bool& permissions) {

    const WriteTheme wt = getWriteTheme();

    std::vector<std::string> validationErrors;
    std::vector<std::pair<IsoInfo, std::string>> validPairs;

    for (const auto& devicePair : deviceMap) {
        size_t index = devicePair.first;
        const std::string& device = devicePair.second;
        const auto& iso = selectedIsos[index - 1];

        if (!isUsbDevice(device)) {
            std::string errMsg;
            errMsg.append(wt.errPath).append("'").append(device).append("'")
                  .append(wt.rl_resetCol).append(wt.errLabel).append(" is not a removable USB device")
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

        uint64_t deviceSize = getBlockDeviceSize(device);

        if (deviceSize == 0) {
            std::string errMsg;
            errMsg.append(wt.errLabel).append("Failed to get size for ")
                  .append(wt.errPath).append("'").append(device).append("'")
                  .append(wt.rl_resetCol).append(wt.errLabel).append(" check permissions")
                  .append(wt.rl_resetCol);
            validationErrors.push_back(std::move(errMsg));
            permissions = true;
            continue;
        }

        if (iso.size > deviceSize) {
            std::string deviceSizeStr = formatFileSize(deviceSize);
            std::string driveName = getDriveName(device);
            std::string errMsg;
            errMsg.append(wt.warnLabel).append("'").append(wt.fileCol).append(iso.filename)
                  .append(wt.rl_resetCol).append(wt.bold).append(" (")
                  .append(wt.sizeCol).append(iso.sizeStr).append(wt.rl_resetCol).append(wt.bold)
                  .append(")").append(wt.warnLabel).append("'").append(wt.errLabel).append(" is too large for ")
                  .append(wt.errPath).append("'").append(device).append(wt.bold)
                  .append(" <").append(driveName).append(">")
                  .append(wt.rl_resetCol).append(wt.bold).append(" (")
                  .append(wt.sizeCol).append(deviceSizeStr).append(wt.rl_resetCol).append(")").append(wt.warnLabel).append("'")
                  .append(wt.rl_resetCol);
            validationErrors.push_back(std::move(errMsg));
            continue;
        }

        validPairs.emplace_back(iso, device);
    }

    if (!validationErrors.empty()) {
        std::cerr << "\n" << wt.errLabel << "Validation errors:" << wt.rl_resetCol << wt.bold << "\n";
        for (const auto& err : validationErrors) {
            std::cerr << "  \u2022 " << err << wt.bold << "\n";
        }

        signal(SIGINT, SIG_IGN);
        disable_ctrl_d();
        permissions ? (permissions = false, pressEnterToContinue()) : pressEnterToTry();
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

    errors.clear();

    while (std::getline(pairStream, pair, ';')) {
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

    std::unordered_set<size_t> mappedIndices;
    for (const auto& [index, device] : deviceMap) {
        mappedIndices.insert(index);
    }

    for (size_t i = 1; i <= selectedIsos.size(); ++i) {
        if (mappedIndices.find(i) == mappedIndices.end()) {
            errors.push_back("Missing mapping for ISO " + std::to_string(i));
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
 * explicitly cancels with @c "<".
 *
 * Readline tab-completion is configured for both ISO indices and device paths.
 * A @c "?" input triggers the help screen.  After a valid mapping is entered,
 * the user is shown a destructive-write warning and must confirm with @c y/Y.
 *
 * @param selectedIsos          ISOs chosen by the user for writing.
 * @return Validated (IsoInfo, device) pairs ready for @ref performWriteOperation,
 *         or an empty vector if the user aborted.
 */
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos) {
    auto setupReadline = []() {
        rl_completion_display_matches_hook = [](char **matches, int num_matches, int max_length) {
            (void)matches;
            (void)num_matches;
            (void)max_length;
        };

        rl_attempted_completion_function = completion_cb;
        rl_bind_key('\t', rl_complete);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_keyseq("\033[A", rl_get_previous_history);
        rl_bind_keyseq("\033[B", rl_get_next_history);
    };

    const WriteTheme wt = getWriteTheme();

    while (true) {
        setupReadline();
        signal(SIGINT, SIG_IGN);
        disable_ctrl_d();
        clearScrollBuffer();

        ThreadPool& pool = getStaticThreadPool();
        if (selectedIsos.size() > pool.threadCount()) {
            std::cout << "\n" << wt.colorFailure << "ISO selections for "
                      << UI::Palette::Yellow << "write2usb"
                      << wt.colorFailure << " cannot exceed the current global thread pool size of "
                      << wt.colorWarning << pool.threadCount()
                      << wt.colorFailure << "!"
                      << wt.speedCol << "\n";

            pressEnterToTry();
            return {};
        }

        displayErrors();

        std::vector<IsoInfo> sortedIsos = selectedIsos;
        std::sort(sortedIsos.begin(), sortedIsos.end(), [](const IsoInfo& a, const IsoInfo& b) {
            return a.size > b.size;
        });

        std::ostringstream devicePromptStream;
        devicePromptStream << "\n" << wt.pathCol << "Selected " << wt.headerCol << "ISO" << wt.bold << ":\n\n";

        for (size_t i = 0; i < sortedIsos.size(); ++i) {
            auto [isoDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write2usb");

            devicePromptStream << "  " << wt.indexCol << (i + 1) << ">" << wt.bold << " ";

            if (!displayConfig::toggleNamesOnly) {
                devicePromptStream << wt.pathCol << isoDir << "/";
            }

            devicePromptStream << wt.fileCol << filename;

            devicePromptStream << wt.bold << " (" << wt.sizeCol << sortedIsos[i].sizeStr
                              << wt.bold << ")\n";
        }

        devicePromptStream << "\n" << wt.rl_resetCol << "Removable USB Devices:" << wt.rl_resetCol << "\n\n";
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
                                      << wt.rl_resetCol << " <" << dev.driveName
                                      << "> (" << wt.sizeCol << dev.sizeStr
                                      << wt.rl_resetCol << ")"
                                      << (dev.mounted ? wt.colorFailure + " (mounted)" + wt.rl_resetCol : "")
                                      << "\n";
                }
            }
        }

        g_completerData.sortedIsos = &sortedIsos;
        g_completerData.usbDevices = &usbDevices;
        reset_custom_keybindingsForCpMvWrite2Usb();

        devicePromptStream << "\n" << wt.rl_labelCol << "Mappings"
                          << wt.rl_primaryCol << " ↵ as "
                          << wt.rl_highlightCol << "INDEX>DEVICE"
                          << wt.rl_primaryCol << ", ? help, < return: "
                          << wt.rl_resetCol;

        std::string devicePrompt = devicePromptStream.str();

        std::unique_ptr<char, decltype(&std::free)> deviceInput(
            readline(devicePrompt.c_str()), &std::free
        );

        if (!deviceInput) {
            restoreReadline();
            return {};
        }

        if (deviceInput.get()[0] == '\0') {
            continue;
        }

        std::string mainInputString(deviceInput.get());
        if (mainInputString == "<") {
            restoreReadline();
            return {};
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
                std::cerr << "  • " << err << "\n";
            }

            pressEnterToTry();
            continue;
        }

        bool permissions = false;
        auto validPairs = validateDevices(deviceMap, sortedIsos, permissions);
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

            std::cout << "  {" << wt.deviceCol << device
                      << " " << wt.rl_resetCol << "<" << driveName << "> ("
                      << wt.sizeCol << deviceSizeStr
                      << wt.rl_resetCol << ")} ← {" << wt.fileCol
                      << iso.filename << wt.rl_resetCol << " (" << wt.sizeCol
                      << iso.sizeStr << wt.rl_resetCol << ")}\n";
        }

        disableReadlineForConfirmation();

        // Constructing the Readline prompt using theme colors
        const std::string confirmPrompt =
            "\001" + std::string(color) + "\002" +
            "\nProceed? (y/n): "
            + wt.rl_resetCol;

        std::unique_ptr<char, decltype(&std::free)> confirmation(
            readline(confirmPrompt.c_str()),
            &std::free
        );

        if (confirmation && (confirmation.get()[0] == 'y' || confirmation.get()[0] == 'Y')) {
            restoreReadline();
            setupSignalHandlerCancellations();
            GlobalState::g_operationCancelled.store(false);
            return validPairs;
        }

        restoreReadline();
    }
}

/**
 * @brief Dispatches ISO-to-device write tasks to the thread pool and tracks progress.
 *
 * Initialises @ref progressData for each pair, caches device information (names,
 * sizes, formatted strings), enqueues one @ref writeIsoToDevice call per pair
 * in the global thread pool, and runs a background display thread that redraws
 * per-task progress (percentage, bytes written, speed) at 100 ms intervals using
 * ANSI cursor save/restore sequences. On completion or cancellation, prints a
 * summary line with overall status (COMPLETED / PARTIAL / FAILED / INTERRUPTED)
 * and elapsed time.
 *
 * When Ctrl+C is detected (@ref GlobalState::g_operationCancelled), the function:
 * - Stops waiting for remaining futures immediately
 * - Disables further SIGINT handling
 * - Allows already-enqueued writes to finish naturally (they check the flag)
 * - Waits only for the progress display thread to exit
 *
 * The function blocks until either:
 * - All futures resolve AND the progress thread exits (normal completion), OR
 * - Cancellation occurs (waits only for progress thread, abandons remaining futures)
 *
 * @param validPairs Validated (IsoInfo, device) pairs produced by
 *                   @ref collectDeviceMappings
 */
void performWriteOperation(const std::vector<std::pair<IsoInfo, std::string>>& validPairs) {
    progressData.clear();
    progressData.reserve(validPairs.size());

    GlobalState::g_operationCancelled.store(false);

    for (const auto& [iso, device] : validPairs) {
        progressData.push_back(ProgressInfo{
            iso.filename,
            device,
            iso.sizeStr
        });
    }

    std::atomic<size_t> completedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    const size_t totalTasks = validPairs.size();

    ThreadPool& pool = getStaticThreadPool();

    disableInput();
    clearScrollBuffer();

    const WriteTheme wt = getWriteTheme();

    std::cout << "\n" << wt.colorStatus << "Processing "
              << (totalTasks > 1 ? "tasks" : "task") << " for "
              << UI::Palette::Yellow << "write2usb" << wt.colorStatus
              << " operation... (" << UI::Palette::Red << "Ctrl+c"
              << wt.colorStatus << ":cancel)\n\n";
    std::cout << "\033[s";

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

    auto displayAllProgress = [&]() {
        for (size_t i = 0; i < progressData.size(); ++i) {
            const auto& prog = progressData[i];
            std::string currentSize = formatFileSize(prog.bytesWritten.load());
            const std::string& displayTotal = prog.completed.load() ? currentSize : prog.totalSize;

            std::cout << "\033[K"
                      << wt.fileCol << prog.filename << " " << wt.bold << "→ {"
                      << wt.deviceCol << prog.device << wt.bold << " <"
                      << deviceNames[prog.device] << "> (" << wt.sizeCol
                      << deviceSizeStrs[prog.device] << wt.bold << ")} " << wt.bold;

            if (prog.completed.load())                             std::cout << wt.colorSuccess << "DONE";
            else if (prog.failed.load())                           std::cout << wt.colorFailure << "FAIL";
            else if (GlobalState::g_operationCancelled.load())     std::cout << wt.colorWarning << "CXL";
            else                                                   std::cout << prog.progress << "%";

            std::cout << wt.bold << " [" << wt.headerCol << currentSize << "/" << wt.sizeCol << displayTotal << wt.bold << "] "
                      << wt.speedCol << formatSpeed(prog.speed) << (prog.completed.load() ? " (avg)" : "") << wt.bold << "\n";
        }
        std::cout << std::flush;
    };

    auto displayProgress = [&]() {
        while (!isProcessingComplete.load(std::memory_order_acquire) &&
              !GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "\033[u";
            displayAllProgress();
        }
    };

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < totalTasks; ++i) {
        futures.push_back(pool.enqueue([&, i]() {
            const auto& [iso, device] = validPairs[i];
            bool success = writeIsoToDevice(iso.path, device, i);

            if (success) {
                progressData[i].completed.store(true);
                completedTasks.fetch_add(1);
            } else if (!GlobalState::g_operationCancelled.load()) {
                progressData[i].failed.store(true);
            }
        }));
    }

    std::thread progressThread(displayProgress);

    // Wait for futures with timeout to allow immediate cancellation
    for (auto& future : futures) {
        while (!GlobalState::g_operationCancelled.load()) {
            auto status = future.wait_for(std::chrono::milliseconds(100));
            if (status == std::future_status::ready) {
                break;  // Task completed
            }
            // If cancelled, break out - the write function will detect
            // GlobalState::g_operationCancelled and return quickly
        }
        // If cancelled, don't wait anymore - tasks will finish on their own
        if (GlobalState::g_operationCancelled.load()) {
            break;
        }
    }

    isProcessingComplete.store(true, std::memory_order_release);

    // Don't wait for cancelled tasks - just detach them
    if (GlobalState::g_operationCancelled.load()) {
        signal(SIGINT, SIG_IGN);
        progressThread.join();
        std::cout << "\033[u";
        displayAllProgress();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        signal(SIGINT, SIG_IGN);
        progressThread.join();
    }

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

    std::cout << "\r" << wt.colorStatus << "Status: " << operation << " → "
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
    std::cout << "\n" << wt.colorStatus << "Successful: "
              << wt.colorSuccess << completedTasks.load()
              << wt.colorStatus << "/"
              << wt.colorWarning << validPairs.size()
              << wt.colorStatus << " | Time Elapsed: "
              << wt.colorStatus << duration << "s"
              << wt.colorStatus << "\n";

    flushStdin();
    restoreInput();
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
