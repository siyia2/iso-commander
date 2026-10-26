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
#include <numeric>
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
 * @brief Determines whether a block device is a removable USB storage device.
 *
 * Resolves symlinks (e.g. /dev/disk/by-id/...), requires the sysfs
 * "removable" attribute to read "1", then walks up the device's canonical
 * sysfs path looking for a USB device node (identified by its "idVendor"
 * file). Fails closed on any error.
 *
 * @param devicePath Path to the block device (must start with /dev/).
 * @return true if the device is a removable USB block device.
 */
bool isFlashDrive(const std::string& devicePath) {
    namespace fs = std::filesystem;
    try {
        if (devicePath.size() <= 5 || devicePath.rfind("/dev/", 0) != 0) {
            return false;
        }

        std::error_code ec;

        // Resolve symlinks so /dev/disk/by-id/usb-... maps to /dev/sdX.
        const fs::path devNode = fs::canonical(devicePath, ec);
        if (ec) return false;

        // Whole disks only: partitions (sda1) have no entry in /sys/block.
        const std::string name = devNode.filename().string();
        if (name.empty()) return false;

        const fs::path sysPath = fs::path("/sys/block") / name;
        if (!fs::exists(sysPath, ec) || ec) return false;

        // The removable attribute must read "1" (fail-closed if unreadable).
        {
            std::ifstream file(sysPath / "removable");
            std::string value;
            if (!file || !std::getline(file, value)) return false;
            while (!value.empty() &&
                   std::isspace(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
            if (value != "1") return false;
        }

        // Walk up the real sysfs path; a USB device node has an idVendor file.
        const fs::path resolved = fs::canonical(sysPath, ec);
        if (ec) return false;

        for (fs::path p = resolved; p != p.root_path(); p = p.parent_path()) {
            if (fs::exists(p / "idVendor", ec)) return true;
            ec.clear();
        }
        return false;

    } catch (...) {
        return false;
    }
}

/**
 * @brief Enumerates removable USB block devices visible in /sys/block.
 *
 * Skips loop devices, RAM disks, zram devices, and any entry whose name
 * ends with a digit (partition suffix). A device is included only when
 * its removable sysfs attribute reads "1" and isFlashDrive() confirms it
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
            if (isFlashDrive(devPath)) {
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

namespace WriteValidation {

/**
 * @brief Builds a themed error message that has no associated path.
 *
 * @param wt  Active write theme supplying the label and reset colours.
 * @param msg Message text (e.g. "Root privileges are required").
 * @return The message prefixed with the error label colour and terminated
 *         with a colour reset.
 */
std::string plainErr(const WriteTheme& wt, const std::string& msg) {
    return std::string(wt.errLabel) + msg + wt.rl_resetCol;
}

/**
 * @brief Builds a themed error message of the form "'<path>' <message>".
 *
 * The path is rendered in the theme's path colour and the message in the
 * error label colour.
 *
 * @param wt   Active write theme.
 * @param path Path to display, quoted, at the start of the message.
 * @param msg  Message text; include a leading space (e.g. " is not an ISO file").
 * @return The fully coloured message string.
 */
std::string pathErr(const WriteTheme& wt, const std::string& path, const std::string& msg) {
    std::string s;
    s.append(wt.errPath).append("'").append(path).append("'")
     .append(wt.rl_resetCol).append(wt.errLabel).append(msg)
     .append(wt.rl_resetCol);
    return s;
}

/**
 * @brief Prints a themed "Validation errors:" block to stderr.
 *
 * Each entry is printed as an indented bullet. Entries are expected to be
 * already themed (see @ref plainErr and @ref pathErr).
 *
 * @param wt   Active write theme.
 * @param errs Pre-formatted error messages to print.
 */
void printValidationErrors(const WriteTheme& wt, const std::vector<std::string>& errs) {
    std::cerr << "\n" << wt.errLabel << "Validation errors:" << wt.rl_resetCol << wt.bold << "\n";
    for (const auto& e : errs)
        std::cerr << "  \u2022 " << e << wt.bold << "\n";
    std::cerr << wt.rl_resetCol << "\n";
}

/**
 * @brief Reports whether a block device still has write I/O in flight.
 *
 * Reads the kernel's per-device in-flight counters from
 * /sys/block/<name>/inflight, so it also sees flushing started by another
 * process. Returns false if the counters cannot be read.
 *
 * @param device Whole-disk path (e.g. "/dev/sdb").
 * @return true if one or more write requests are still pending on the device.
 */
bool isDeviceFlushing(const std::string& device) {
    const std::string name = std::filesystem::path(device).filename().string();
    std::ifstream in("/sys/block/" + name + "/inflight");
    unsigned long reads = 0, writes = 0;
    if (!(in >> reads >> writes))
        return false;
    return writes > 0;
}

/**
 * @brief Runs the device-level checks, independent of which ISO is paired with it.
 *
 * Checks, in order:
 * -# In non-interactive mode only, the device has no write I/O still in
 *    flight (@ref isDeviceFlushing). This catches flushing started by another
 *    process, such as a previously cancelled write, which the in-process
 *    @ref g_drainingDevices set cannot see.
 * -# The device is a removable USB flash drive (@ref isFlashDrive).
 * -# The device and its partitions are not mounted (@ref isDeviceMounted).
 * -# The device is not in @ref g_drainingDevices (pending kernel-level tasks
 *    tracked by the interactive UI).
 * -# The device size can be determined (@ref getBlockDeviceSize).
 *
 * @param wt     Active write theme.
 * @param device Path used for the checks (canonical /dev/sdX).
 * @param shown  Path displayed in error messages (what the user typed).
 * @param[out] size Device size in bytes; only valid when no error is returned.
 * @return An error message for the first failed check, or std::nullopt if
 *         the device is ready for writing.
 */
std::optional<std::string> checkDevice(const WriteTheme& wt, const std::string& device,
                                       const std::string& shown, uint64_t& size) {

    if (isDeviceFlushing(device) && GlobalState::isNonInteractive.load())
        return pathErr(wt, shown, " is flushing data, try again shortly");
    if (!isFlashDrive(device))
        return pathErr(wt, shown, " is not a removable USB flash device");

    if (isDeviceMounted(device))
        return pathErr(wt, shown, " or its partitions are mounted");

    {
        std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
        if (g_drainingDevices.count(device))
            return pathErr(wt, shown, " has pending kernel-level tasks");
    }

    size = getBlockDeviceSize(device);
    if (size == 0) {
        std::string e;
        e.append(wt.errLabel).append("Failed to get size for ")
         .append(wt.errPath).append("'").append(shown).append("'")
         .append(wt.rl_resetCol);
        return e;
    }
    return std::nullopt;
}

/**
 * @brief Checks that an ISO fits on a device.
 *
 * @param wt         Active write theme.
 * @param iso        ISO to be written.
 * @param device     Device path, used for the message and to look up the drive name.
 * @param deviceSize Device capacity in bytes (as returned by @ref checkDevice).
 * @return A themed "is too large for" message including both sizes and the
 *         drive name, or std::nullopt if the ISO fits.
 */
std::optional<std::string> checkCapacity(const WriteTheme& wt, const IsoInfo& iso,
                                         const std::string& device, uint64_t deviceSize) {
    if (iso.size <= deviceSize)
        return std::nullopt;

    const std::string deviceSizeStr = formatFileSize(deviceSize);
    const std::string driveName = getDriveName(device);
    std::string e;
    e.append(wt.warnLabel).append("'").append(wt.fileCol).append(iso.filename)
     .append(wt.rl_resetCol).append(UI::Palette::DimGray).append(" (")
     .append(wt.sizeCol).append(iso.sizeStr).append(wt.rl_resetCol)
     .append(UI::Palette::DimGray).append(")")
     .append(wt.warnLabel).append("'").append(wt.errLabel).append(" is too large for ")
     .append(wt.errPath).append("'").append(device).append(wt.infoLabel)
     .append(" <").append(driveName).append(">")
     .append(wt.rl_resetCol).append(UI::Palette::DimGray).append(" (")
     .append(wt.sizeCol).append(deviceSizeStr).append(UI::Palette::DimGray)
     .append(")").append(wt.warnLabel).append("'")
     .append(wt.rl_resetCol);
    return e;
}

} // namespace WriteValidation

/**
 * @brief Validates a set of ISO-to-device mappings and returns only the viable pairs.
 *
 * If @p noPermissions is true on entry, validation is skipped and a
 * "Root privileges are required" error is reported. Otherwise each pair is
 * checked with @ref WriteValidation::checkDevice and then
 * @ref WriteValidation::checkCapacity.
 *
 * @note If any error occurs, all errors are printed, SIGINT and Ctrl-D are
 * disabled, the user is prompted to retry, and an empty vector is returned,
 * even if some pairs were valid. Valid pairs are returned only when every
 * pair passes all checks.
 *
 * @param deviceMap     Pairs of (1-based ISO index, device path) to validate.
 * @param selectedIsos  Ordered list of ISOs corresponding to the indices.
 * @param[in,out] noPermissions On entry, true aborts validation with a
 *        privilege error. It is reset to false after the error is shown,
 *        and selects which prompt is displayed.
 * @return The valid (IsoInfo, device) pairs, or an empty vector if any
 *         validation failed.
 */
std::vector<std::pair<IsoInfo, std::string>> validateDevices(
        const std::vector<std::pair<size_t, std::string>>& deviceMap,
        const std::vector<IsoInfo>& selectedIsos, bool& noPermissions) {

    using namespace WriteValidation;
    const WriteTheme wt = getWriteTheme();

    std::vector<std::string> validationErrors;
    std::vector<std::pair<IsoInfo, std::string>> validPairs;

    if (noPermissions) {
        validationErrors.push_back(plainErr(wt, "Root privileges are required"));
    } else {
        for (const auto& [index, device] : deviceMap) {
            const auto& iso = selectedIsos[index - 1];

            uint64_t deviceSize = 0;
            if (auto err = checkDevice(wt, device, device, deviceSize)) {
                validationErrors.push_back(std::move(*err));
                continue;
            }
            if (auto err = checkCapacity(wt, iso, device, deviceSize)) {
                validationErrors.push_back(std::move(*err));
                continue;
            }
            validPairs.emplace_back(iso, device);
        }
    }

    if (!validationErrors.empty()) {
        printValidationErrors(wt, validationErrors);

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

            devicePromptStream << " " << wt.indexCol << (i + 1) << ">" << wt.bold << " ";

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
            devicePromptStream << " " << wt.colorFailure << "No USB flash drives detected!" << wt.rl_resetCol << "\n";
        } else {
            for (const auto& dev : deviceInfos) {
                if (dev.error) {
                    devicePromptStream << " " << wt.colorFailure << dev.path << " (error)" << wt.rl_resetCol << "\n";
                } else {
                    devicePromptStream << " " << wt.deviceCol << dev.path
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

            std::cout << UI::Palette::DimGray << " {" << wt.deviceCol << device
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
 * @brief Builds the per-task ProgressInfo vector from (IsoInfo, device) pairs,
 *        in the order they would appear after a stable, case-insensitive sort
 *        by ISO filename (device path as tie-break).
 *
 * The caller's vector is never modified and is never copied. A vector of
 * indices into @p validPairsIn is stable-sorted instead, and the pairs are
 * then visited in that order; only the four fields ProgressInfo actually
 * carries (filename, device, sizeStr, sourcePath) are copied, straight into
 * the progress vector. No intermediate vector of (IsoInfo, std::string) pairs
 * is materialised.
 *
 * The progress vector is reserved to exactly @p validPairsIn.size() before
 * filling, so its backing storage never reallocates after construction
 * (ProgressInfo holds atomics and is therefore non-movable — the reserve is
 * load-bearing, not just an optimisation).
 *
 * The returned @c shared_ptr is captured by every pooled write task and by
 * the cancellation drain, so the entries outlive this function whenever a
 * task or drain thread still needs them.
 *
 * @par Ordering
 * Primary key: @c IsoInfo::filename via @ref naturalCompare (case-insensitive,
 * numeric-aware: "archlinux-9.iso" before "archlinux-10.iso"). Secondary key:
 * the device path under ordinary @c std::string ordering. Equal
 * (filename, device) keys keep their input order.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs targeted for writing.
 *                     Not modified.
 * @return A shared pointer to a new vector of ProgressInfo entries, one per
 *         input pair, in stable sorted order.
 *
 * @see performWriteOperation
 */
static std::shared_ptr<std::vector<ProgressInfo>>
prepareWriteData(const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn) {
    // Sort indices, not pairs: no IsoInfo copy, no device-string copy.
    std::vector<size_t> order(validPairsIn.size());
    std::iota(order.begin(), order.end(), size_t{0});   // requires <numeric>

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const int c = naturalCompare(validPairsIn[a].first.filename,
                                     validPairsIn[b].first.filename);
        if (c != 0) return c < 0;
        return validPairsIn[a].second < validPairsIn[b].second;
    });

    auto progressPtr = std::make_shared<std::vector<ProgressInfo>>();
    auto& progress   = *progressPtr;
    progress.reserve(validPairsIn.size());   // must not reallocate: atomics block it

    for (size_t idx : order) {
        const auto& [iso, device] = validPairsIn[idx];
        progress.push_back(ProgressInfo{iso.filename, device, iso.sizeStr, iso.path});
    }
    return progressPtr;
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
 * or captured by value; the shared_ptr is captured instead so the per-operation
 * data stays alive for as long as the detached thread needs it. The device path
 * for each pending future is read from progressData[i].device, so no separate
 * pair vector is required.
 *
 * @param futures        Pending per-task futures. Taken by value and moved into the
 *                       detached thread, so the caller's vector is left empty after
 *                       this call. Must not be used by the caller afterwards.
 * @param progressData   Per-task progress records. Read only: .completed / .failed
 *                       are inspected to decide which devices still need draining,
 *                       and .device is used to enumerate the draining set.
 * @param progressDataPtr Shared ownership of the progress records; captured by the
 *                       detached thread to keep the operation data alive until the
 *                       drain completes.
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
void drainManagerForWrite2UsbUI(
    std::vector<std::future<void>> futures,
    const std::vector<ProgressInfo>& progressData,
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
    drainDevices.reserve(progressData.size());

    bool anyDraining = false;
    {
        std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
        for (size_t i = 0; i < progressData.size(); ++i) {
            const std::string& device = progressData[i].device;
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
 * @brief Owns the live progress screen and keeps it correctly laid out.
 *
 * The screen has two phases:
 * - @b Running  — a keeper thread redraws the per-task rows (see
 *   @ref performWriteOperation for the row layout, truncation and bar rules).
 * - @b Finished — after @ref finish() the screen shows the final rows, the
 *   status line and the summary (and, once set with @ref setPrompt, the prompt).
 *   The keeper thread keeps running in this phase and repaints the whole result
 *   screen whenever the terminal size changes, so the screen re-truncates and
 *   re-wraps correctly even while the program is blocked waiting for input
 *   after the write operation has completed.
 *
 * @par Single renderer
 * Every draw happens under one mutex (@c renderMutex_) and is performed either by
 * the keeper thread or by @ref finish(). There is no second drawing thread, so
 * frames can never interleave. The keeper wakes on a task-completion
 * notification (condition variable in the shared @c WriteSync state) or every
 * 100 ms, at which point it compares the terminal size with the size of the last
 * frame. While running it redraws on completion, on a size change, or when
 * 500 ms have passed since the last frame (live percentages and speeds). While
 * finished it redraws only on a size change.
 *
 * @par Result screen layout
 * The finished screen is always painted from scratch (home, clear screen, clear
 * scrollback), so it never depends on cursor-up arithmetic that a terminal
 * reflow would invalidate:
 * @code
 * (line 1: empty)
 * Status: write2usb → COMPLETED        (line 2)
 * (line 3: empty)
 * <rows...>                            (from line 4)
 * (empty)
 * Successful: n/m | Time Elapsed: x.xs
 * <prompt, if set>
 * @endcode
 *
 * @par Lifetime
 * The object holds @c shared_ptr copies of the progress vector and of the
 * @c WriteSync state, so it never refers to the stack of the function that
 * created it. The destructor stops and joins the keeper thread.
 */
class WriteScreen {
public:
    WriteScreen(std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr,
                std::shared_ptr<WriteSync> syncPtr,
                WriteTheme theme)
        : progressDataPtr_(std::move(progressDataPtr)),
          progressData_(*progressDataPtr_),
          syncPtr_(std::move(syncPtr)),
          wt_(std::move(theme)),
          startTime_(std::chrono::steady_clock::now()) {}

    ~WriteScreen() { stop(); }

    WriteScreen(const WriteScreen&)            = delete;
    WriteScreen& operator=(const WriteScreen&) = delete;

    /// Prints the header, hides the cursor, computes the per-operation column
    /// data and starts the keeper thread.
    void start() {
        {
            std::lock_guard<std::mutex> rl(renderMutex_);
            std::cout << headerText(false) << "\033[?25l" << std::flush;
        }
        computeConstants();
        thread_ = std::thread([this] { keeperLoop(); });
    }

    /// Switches to the result screen and paints it. Call once, after the wait
    /// for completion / cancellation. The keeper keeps running afterwards.
    void finish() {
        std::lock_guard<std::mutex> rl(renderMutex_);

        size_t failed = 0;
        for (const auto& prog : progressData_)
            if (prog.failed.load()) ++failed;

        const size_t completed = syncPtr_->completedTasks.load();
        const bool   cancelled = GlobalState::g_operationCancelled.load();

        const std::string operation =
            std::string(UI::Palette::Yellow) + "write2usb" + wt_.rl_resetCol;

        const std::string word =
            !cancelled
                ? (failed > 0
                    ? (completed > 0
                        ? wt_.colorWarning + "PARTIAL"
                        : wt_.colorFailure + "FAILED")
                    : wt_.colorSuccess + "COMPLETED")
                : wt_.colorWarning + "INTERRUPTED";

        std::ostringstream st;
        st << color << "Status: " << operation << color << " → " << word << wt_.rl_resetCol;
        statusLine_ = st.str();

        const double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startTime_).count();

        std::ostringstream sm;
        sm << std::fixed << std::setprecision(1);
        sm << "\n" << color << "Successful: "
           << wt_.colorSuccess << completed
           << color << "/"
           << wt_.colorWarning << progressData_.size()
           << color << " | Time Elapsed: "
           << color << secs << "s"
           << wt_.colorStatus << UI::Palette::Reset << "\n";
        summaryText_ = sm.str();

        phase_ = Phase::Finished;
        renderLocked();
    }

    /// Text printed under the summary on every result-screen repaint. It must be
    /// the same text the program prints for its "press Enter" prompt, so a
    /// repaint can put the prompt back after wiping the screen.
    void setPrompt(std::string prompt) {
        std::lock_guard<std::mutex> rl(renderMutex_);
        promptText_ = std::move(prompt);
    }

    /// Stops and joins the keeper thread. Safe to call more than once.
    void stop() {
        if (!thread_.joinable()) return;
        stop_.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(syncPtr_->mutex); }   // close check-then-block race
        syncPtr_->cv.notify_all();
        thread_.join();
    }

private:
    enum class Phase { Running, Finished };

    static constexpr size_t MAX_NAME_LEN = 20;
    static constexpr size_t MAX_FILE_LEN = 15;   // names up to this many columns (with ".iso") are never truncated
    static constexpr size_t STEM_KEEP    = 10;   // stem columns kept when truncating, then "…" and ".iso" (10 + 1 + 4 = 15)
    static constexpr int    BAR_WIDTH    = 5;    // character cells
    static constexpr int    BAR_STEPS    = BAR_WIDTH * 2;   // each cell has 2 states (half / full) -> 10 steps, 10% each
    static constexpr auto   kPulse = std::chrono::milliseconds(500);   // live-value refresh while running
    static constexpr auto   kPoll  = std::chrono::milliseconds(100);   // resize polling interval

    // ---- helpers (formerly lambdas inside the render function) ---------------

    // Counts visible characters (UTF-8 code points) instead of bytes,
    // so the "…" counts as 1 column rather than 3.
    static size_t displayWidth(const std::string& s) {
        size_t w = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++w;
        return w;
    }

    // Shortens a drive name to at most MAX_NAME_LEN columns, backing up so we
    // never split a multi-byte UTF-8 sequence (continuation bytes are 10xxxxxx).
    static std::string makeShortName(const std::string& n) {
        if (n.size() <= MAX_NAME_LEN) return n;
        size_t cut = MAX_NAME_LEN - 1;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
        std::string out;
        out.reserve(cut + 3);                   // kept bytes + 3-byte "…"
        out.append(n, 0, cut);
        out.append("\u2026");
        return out;
    }

    // Current terminal size {cols, rows}; 80x24 if it cannot be determined
    // (e.g. stdout is not a tty).
    static std::pair<size_t, size_t> terminalSize() {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
            return { ws.ws_col, ws.ws_row };
        return { 80, 24 };
    }

    // Right-pads an ASCII string to the given byte width. Used for device
    // paths and other ASCII-only columns.
    static std::string padRight(std::string s, size_t target) {
        if (s.size() < target) s.append(target - s.size(), ' ');
        return s;
    }

    // Visible width of a string that may contain ANSI CSI escape sequences
    // (colors etc.): escape sequences count as 0 columns.
    static size_t visibleWidth(const std::string& s) {
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
    }

    // Cuts a string that may contain ANSI escape sequences to at most maxCols
    // visible columns. Escape sequences are passed through untouched and UTF-8
    // code points are never split.
    static std::string clipToWidth(const std::string& s, size_t maxCols) {
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
    }

    // Truncates ISO filenames, but only when `truncate` is set (the terminal is
    // too narrow for the full-width rows) AND the whole name (".iso" included)
    // is longer than MAX_FILE_LEN columns. A truncated name shows exactly
    // STEM_KEEP stem columns, then "…" (1 column), then the ".iso" extension, e.g.
    // "archlinux-….iso", so a truncated name is always exactly 15 columns and the
    // extension is always visible. Names of MAX_FILE_LEN columns or fewer are
    // shown in full. Walks whole UTF-8 code points so a multi-byte sequence is
    // never split.
    static std::string shortFilename(const std::string& f, bool truncate) {
        if (!truncate) return f;   // terminal is wide enough: show the full name

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
    }

    // ---- per-operation constants ---------------------------------------------

    // Device names, shortened drive names and column widths cannot change while
    // the operation runs, so they are computed once, before the keeper starts.
    void computeConstants() {
        std::unordered_map<std::string, std::string> deviceNames;
        for (const auto& prog : progressData_) {
            if (deviceNames.find(prog.device) == deviceNames.end())
                deviceNames[prog.device] = getDriveName(prog.device);
        }

        // Index i matches progressData_[i].
        rowShortNames_.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            rowShortNames_.push_back(makeShortName(deviceNames[prog.device]));
            maxDeviceLen_         = std::max(maxDeviceLen_,         prog.device.size());
            maxNameWidth_         = std::max(maxNameWidth_,         displayWidth(rowShortNames_.back()));
            maxFullFilenameWidth_ = std::max(maxFullFilenameWidth_, displayWidth(prog.filename));
        }
    }

    // Header text: reprinted after every full-screen wipe. While running it is the
    // "Processing..." banner; once finished, line 2 holds the status line instead.
    std::string headerText(bool finished) const {
        std::ostringstream h;
        if (!finished) {
            h << "\n" << color << "Processing "
              << (progressData_.size() > 1 ? "tasks" : "task") << " for "
              << UI::Palette::Yellow << "write2usb" << color
              << "... (" << UI::Palette::Red << "Ctrl+c"
              << color << ":cancel)\n\n";
        } else {
            h << "\n" << statusLine_ << "\n\n";
        }
        return h.str();
    }

    // ---- keeper thread ---------------------------------------------------------

    void keeperLoop() {
        using clock = std::chrono::steady_clock;
        clock::time_point lastDraw{};                       // epoch: first pass draws at once
        size_t seenFinished = static_cast<size_t>(-1);      // differs from any real count: first pass draws

        while (!stop_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> rl(renderMutex_);

                const auto   size        = terminalSize();
                const bool   sizeChanged = hasDrawn_ && (size.first != lastCols_ || size.second != lastRows_);
                const size_t finishedNow = syncPtr_->finishedTasks.load();
                const bool   completion  = (finishedNow != seenFinished);
                seenFinished = finishedNow;   // updated in every phase so the wait below can't spin

                if (phase_ == Phase::Running) {
                    const bool pulse = (clock::now() - lastDraw) >= kPulse;
                    if (sizeChanged || completion || pulse) {
                        renderLocked();
                        lastDraw = clock::now();
                    }
                } else if (sizeChanged) {
                    renderLocked();   // result screen: repaint only when the size changed
                }
            }

            // Wake at once on a task completion (or stop); otherwise re-check the
            // terminal size after kPoll.
            std::unique_lock<std::mutex> lk(syncPtr_->mutex);
            syncPtr_->cv.wait_for(lk, kPoll, [&] {
                return stop_.load(std::memory_order_acquire) ||
                       syncPtr_->finishedTasks.load() != seenFinished;
            });
        }
    }

    // ---- rendering -------------------------------------------------------------

    // Builds the whole frame in one string and writes it once (less flicker).
    // The caller must hold renderMutex_.
    void renderLocked() {
        const bool finished = (phase_ == Phase::Finished);

        size_t maxFilenameWidth = 0;
        size_t maxSpeedWidth    = 0;

        // Speed strings are formatted once per render so the column can be
        // right-aligned to the widest value (the "(avg)" suffix is appended
        // after the padded value and does not affect alignment).
        std::vector<std::string> speedStrs;
        speedStrs.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            speedStrs.push_back(formatSpeed(prog.speed));
            maxSpeedWidth = std::max(maxSpeedWidth, displayWidth(speedStrs.back()));
        }

        // Reserve room for " (avg)" only if some task is already showing it,
        // so names are truncated only when a row is really wider than the terminal.
        bool anyCompleted = false;
        for (const auto& prog : progressData_)
            if (prog.completed.load()) { anyCompleted = true; break; }
        const size_t avgExtra = anyCompleted ? 6 : 0;

        //   " → " (3) + device + " <name>" (maxNameWidth + 3) + " [" (2) + bar (5)
        //   + "] " (2) + status (5) + speed (+ " (avg)" once a task is done)
        const size_t fixedWidth = 3 + maxDeviceLen_ + (maxNameWidth_ + 3) + 2 + BAR_WIDTH
                                + 2 + 5 + maxSpeedWidth + avgExtra;

        const auto   termSize   = terminalSize();
        const size_t cols       = termSize.first;
        const size_t rows       = termSize.second;
        const size_t usableCols = cols > 1 ? cols - 1 : 1;

        // Truncate only if the widest full-width row is longer than the terminal.
        const bool truncateNames = (fixedWidth + maxFullFilenameWidth_) > usableCols;

        // Shortened filenames, computed once per frame (depends on truncateNames).
        std::vector<std::string> shortFiles;
        shortFiles.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            shortFiles.push_back(shortFilename(prog.filename, truncateNames));
            maxFilenameWidth = std::max(maxFilenameWidth, displayWidth(shortFiles.back()));
        }

        std::string frame;
        frame += "\033[?2026h";
        const bool resized = hasDrawn_ && (cols != lastCols_ || rows != lastRows_);
        if (finished || resized) {
            // The result screen is always painted from scratch, and a resized
            // running screen is too: don't trust cursor-up after a resize, the
            // terminal may have reflowed or scrolled old content into scrollback,
            // and cursor-up cannot move above the visible screen.
            frame += "\033[H\033[2J\033[3J";   // home, clear screen, clear scrollback
            frame += headerText(finished);
        } else {
            // Same size as last frame: move up over the previous rows (accounting
            // for any wrapping) and clear to the end of the screen. Relative
            // movement is used instead of \033[s / \033[u because a saved absolute
            // cursor position goes stale when the terminal re-wraps.
            size_t up = 0;
            for (size_t w : lastRowWidths_)
                up += std::max<size_t>(1, (w + cols - 1) / cols);
            if (up > 0) frame += "\033[" + std::to_string(up) + "A";
            frame += "\r\033[J";
        }
        lastCols_ = cols;
        lastRows_ = rows;
        hasDrawn_ = true;
        lastRowWidths_.clear();
        lastRowWidths_.reserve(progressData_.size());

        for (size_t i = 0; i < progressData_.size(); ++i) {
            const auto& prog = progressData_[i];

            const std::string& fname = shortFiles[i];
            std::string paddedFilename = fname;
            paddedFilename.append(maxFilenameWidth - displayWidth(fname), ' ');

            const std::string paddedDevice = padRight(prog.device, maxDeviceLen_);

            const std::string& name = rowShortNames_[i];
            const std::string namePad(maxNameWidth_ - displayWidth(name), ' ');

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
            row << wt_.fileCol << paddedFilename
                << color << " → " << wt_.deviceCol << paddedDevice
                << color << " " << wt_.infoLabel << "<" << name << ">" << namePad
                << color << " ["
                << bar << wt_.speedCol << empty
                << color << "] ";

            bool isCompleted = prog.completed.load();

            if (isCompleted)                                    row << wt_.colorSuccess << "DONE ";
            else if (prog.failed.load())                        row << wt_.colorFailure << "FAIL ";
            else if (GlobalState::g_operationCancelled.load())  row << wt_.colorWarning << "CXL  ";
            else {
                std::string pctStr = std::to_string(pct) + "%";
                pctStr.resize(5, ' ');
                row << color << pctStr;
            }

            const std::string& speedStr = speedStrs[i];
            const std::string speedPad(maxSpeedWidth - displayWidth(speedStr), ' ');

            row << color << speedStr << (isCompleted ? " (avg)" : speedPad);

            // Last line of defense: if the terminal is too narrow even for the
            // truncated row, cut the row instead of letting it wrap.
            const std::string line = clipToWidth(row.str(), usableCols);
            lastRowWidths_.push_back(std::min(visibleWidth(line), usableCols));
            frame += line;
            frame += "\033[0m\n";
        }

        if (finished) {
            frame += summaryText_;
            frame += promptText_;
            frame += "\033[?25h";            // show the cursor again
        }
        frame += "\033[?2026l";              // end synchronized update
        std::cout << frame << std::flush;
    }

    // ---- state -----------------------------------------------------------------

    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr_;
    std::vector<ProgressInfo>&                 progressData_;
    std::shared_ptr<WriteSync>                 syncPtr_;
    const WriteTheme                           wt_;
    const std::chrono::steady_clock::time_point startTime_;

    // Written once in computeConstants() before the keeper thread starts.
    std::vector<std::string> rowShortNames_;
    size_t maxDeviceLen_         = 0;
    size_t maxNameWidth_         = 0;
    size_t maxFullFilenameWidth_ = 0;   // widest name with no truncation applied

    // Everything below is guarded by renderMutex_.
    std::mutex          renderMutex_;
    Phase               phase_ = Phase::Running;
    std::string         statusLine_;
    std::string         summaryText_;
    std::string         promptText_;
    // Visible width (in columns) of every row printed by the previous frame. Used to
    // work out how far to move the cursor back up, even if the terminal was resized
    // (and therefore re-wrapped the old rows) in between.
    std::vector<size_t> lastRowWidths_;
    // Terminal size at the time of the previous frame. A change in either
    // dimension triggers a full wipe + repaint instead of a cursor-up redraw.
    size_t              lastCols_ = 0, lastRows_ = 0;
    bool                hasDrawn_ = false;

    std::atomic<bool>   stop_{false};
    std::thread         thread_;
};

/**
 * @brief Enqueues ISO-to-device write tasks and drives a live progress display.
 *
 * The input pairs are used to build a @ref ProgressInfo vector, sorted by ISO
 * filename via @ref prepareWriteData (case-insensitive, numeric-aware), with the
 * device path as a tie-break when the same ISO targets several devices.
 * Task index, @ref progressData index and display row all share this sorted
 * order, so the live progress tracks always appear in natural order by ISO name.
 *
 * For each validated (IsoInfo, device) pair, in sorted order:
 * - A @ref ProgressInfo entry (filename, device, sizeStr, sourcePath) is
 *   appended to @ref progressData by @ref prepareWriteData — reserved up front,
 *   one entry per task, same order as the sort
 * - Drive names are looked up once per device. The shortened drive names and the
 *   device, drive-name and full-filename column widths are then computed once per
 *   operation (by @ref WriteScreen), because none of them can change while the
 *   operation runs. Only the filename truncation and the speed column width are
 *   recomputed per frame, since they depend on the current terminal width and the
 *   live speeds.
 * - One @ref writeIsoToDevice call per task is submitted to the global @ref ThreadPool
 *
 * All drawing is done by a @ref WriteScreen. Its keeper thread redraws per-task
 * status, waking immediately whenever a task completes (via a condition variable
 * in the shared @c WriteSync state, notified from each pooled task) and checking
 * the terminal size every 100 ms. The main thread waits on the same condition
 * variable to detect overall completion, also waking instantly on task completion.
 *
 * @par Structure
 * The body is split into phases so the invariants are named once and reused:
 * -# @ref prepareWriteData builds a @c std::shared_ptr to a stable-sorted vector
 *    of @ref ProgressInfo entries, one per input pair, in sorted order (task
 *    index @c i matches the display row). The sort is done on an index vector,
 *    so neither the caller's pairs nor a copy of them is ever materialised. The
 *    shared pointer is captured by every pooled task, by the @ref WriteScreen and
 *    by the cancellation drain, so the data outlives this function if needed.
 * -# The remainder of the function creates the @ref WriteScreen, enqueues the
 *    tasks, waits for completion or cancellation, switches the screen to its
 *    result layout with @ref WriteScreen::finish, and returns the screen. The
 *    completion counters, mutex and condition variable live in a @c WriteSync
 *    object owned by a @c std::shared_ptr, which every pooled task and the screen
 *    capture by value (like the progress vector). Nothing refers to this
 *    function's stack, so everything stays valid for tasks that outlive the
 *    function after cancellation.
 *
 * @par Ordering
 * Rows are sorted by ISO filename using @c naturalCompare (case-insensitive,
 * numeric-aware: "archlinux-9.iso" before "archlinux-10.iso"), with the device
 * path under ordinary @c std::string ordering as the secondary key. The sort is
 * stable and performed once by @ref prepareWriteData on a temporary index vector,
 * so the caller's vector is never modified and no copy of it is ever taken. The
 * pooled tasks share the progress vector through a @c std::shared_ptr, which keeps
 * it alive for any detached drain that outlives this function after cancellation.
 *
 * @par Filename Truncation
 * ISO names are shown in full unless the widest row (full names, device, drive name,
 * bar, status and speed columns) would not fit the current terminal width. Only then
 * is every name longer than 15 columns (".iso" included) truncated: a truncated name
 * keeps the first 10 stem columns, followed by "…" (U+2026, one column) and the
 * untouched ".iso" extension, so a truncated ".iso" name is exactly 10 + 1 + 4 = 15
 * columns, e.g. "archlinux-2024.10.01-x86_64.iso" becomes "archlinux-….iso". The
 * extension is therefore always visible. Names of 15 columns or fewer are always
 * shown in full. Names without a ".iso" suffix are treated the same way, with the
 * whole name as the stem, so they are cut to 10 stem columns plus "…" (11 columns).
 * The decision is made once per render for all rows together, so either every long
 * name is truncated or none is, and the columns stay aligned. The terminal width is
 * re-queried on every render, and the keeper thread checks it every 100 ms, so a
 * resize takes effect within about 100 ms — during the write and also afterwards,
 * while the result screen is waiting for the user (see "Resize Handling"). If the
 * width cannot be determined (stdout is not a tty), 80 columns is assumed. The width
 * check reserves room for the " (avg)" suffix, so the decision does not flip when
 * tasks complete. Truncation is UTF-8 safe (never splits a multi-byte sequence) and
 * affects the display only; the real path passed to @ref writeIsoToDevice is
 * untouched. Drive names are shortened separately, once per operation: a name longer
 * than 20 bytes is cut (again without splitting a multi-byte sequence) and ends in
 * "…". Padding is computed from displayed widths so columns stay aligned: filenames
 * and drive names are padded from their displayed (code point) width, since they may
 * contain non-ASCII text, while the ASCII-only device path is padded through the
 * shared @c padRight helper.
 *
 * @par Resize Handling
 * No signal handler is involved. The @ref WriteScreen keeper thread compares the
 * terminal size with the size of the last frame every 100 ms (a cheap @c ioctl) and
 * additionally wakes on every task completion. It lives from the start of the
 * operation until the caller stops it, so it is still running while the caller
 * blocks in the "press Enter" prompt after this function has returned. While
 * running, a size change triggers a full repaint; once finished, a size change
 * repaints the whole result screen (rows, status, summary and prompt). The size is
 * the only trigger in the finished phase, so an idle result screen costs one
 * @c ioctl every 100 ms.
 *
 * @par Redraw
 * Each frame is built as one string and written once, wrapped in a synchronized
 * update. Rows are never allowed to wrap: the last terminal column is kept free and
 * any row still too wide after truncation is clipped (ANSI-escape and UTF-8 aware).
 * The terminal size (columns and rows) is remembered between frames. If it is
 * unchanged while running, the previous frame's row widths are used to compute how
 * many physical lines it occupies at the current width; the cursor moves up exactly
 * that far and the screen is cleared from there down before redrawing. Relative
 * movement is used instead of saving/restoring an absolute cursor position, which
 * goes stale when a terminal re-wraps its content. If either dimension has changed,
 * cursor-up is not trusted: the terminal may have reflowed or scrolled old rows into
 * scrollback, where cursor movement cannot reach, and a partial clear would leave
 * duplicated frames behind. Instead the screen and scrollback are wiped (home, clear
 * screen, clear scrollback), the header is reprinted, and the frame is drawn from
 * the top. The result screen is always painted this way. Header on lines 1-3, rows
 * from line 4. Frames are serialized by one mutex, so the keeper thread and
 * @ref WriteScreen::finish can never interleave output.
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
 * -# The main thread's condition-variable wait re-checks
 *    @ref GlobalState::g_operationCancelled at least every 500 ms and breaks as
 *    soon as it observes the flag set (the flag is set from a signal handler, which
 *    cannot safely notify the condition variable).
 * -# SIGINT is ignored and @ref WriteScreen::finish paints the result screen
 *    (rows marked CXL, status INTERRUPTED). The keeper thread keeps running.
 * -# Terminal properties, standard input buffers, and layout configurations are restored @b instantly.
 * -# Operating system control is returned to the caller without visual lag.
 * -# @b Out-of-Band Draining: Ownership of the still-running @c std::future objects,
 *    together with the shared progress vector, is transferred to
 *    @ref drainManagerForWrite2UsbUI, which moves them into a detached background
 *    thread. This allows OS page caches (e.g. buffered NTFS flushes) to drain onto
 *    the hardware safely in the background, entirely avoiding stack-use-after-free
 *    crashes. Tasks may still be running when this function returns, so everything
 *    they touch afterwards (the progress vector and the @c WriteSync state) is owned
 *    by @c shared_ptr copies captured in each task; nothing is captured by reference
 *    from this function's locals. Each device is removed from the global
 *    dirty-device set as its own drain completes, allowing the device selection
 *    screen to clear the @b [FLUSHING] indicator per-device as soon as it is safe.
 *
 * @par Normal Completion
 * The main thread's condition-variable wait unblocks once every task has finished
 * (succeeded or failed), the screen is switched to its result layout, and the
 * futures are joined inline; the out-of-band drain helper is not invoked.
 *
 * @par Result Screen
 * On exit the screen shows a status overview on line 2 (the summary line below), the
 * final rows, and a summary line. The successful count comes from a single load of
 * the completion counter (reused for both the status decision and the final
 * successful-count line); the failed count is obtained by scanning the per-task
 * @c failed flags in the progress vector:
 * - @b COMPLETED — All targeted block operations succeeded.
 * - @b PARTIAL   — At least one task succeeded and at least one task failed.
 * - @b FAILED    — Total failure across all enqueued storage block streams.
 * - @b INTERRUPTED — Operation aborted by explicit user termination before total completion.
 *
 * Final execution metrics (elapsed duration and a successful/total count ratio) follow
 * the rows.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs targeted for writing. Used
 *                     to build a sorted @ref ProgressInfo vector by
 *                     @ref prepareWriteData; the caller's vector is never modified.
 *
 * @return The screen, still alive and still reflowing on resize. The caller keeps it
 *         while it waits for the user (after calling @ref WriteScreen::setPrompt
 *         with the prompt text) and then calls @ref WriteScreen::stop, or simply
 *         lets it go out of scope.
 */
std::shared_ptr<WriteScreen> performWriteOperation(
        const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn) {
    // One call builds the sorted per-task progress vector and establishes the
    // ordering invariant (task i ↔ progress row i). No sorted-pair vector is
    // materialised, so only one shared pointer travels through the tasks and drain.
    auto progressDataPtr = prepareWriteData(validPairsIn);
    auto& progressData   = *progressDataPtr;

    const size_t totalTasks = progressData.size();

    auto syncPtr = std::make_shared<WriteSync>();
    auto& finishedTasks   = syncPtr->finishedTasks;
    auto& progressCvMutex = syncPtr->mutex;
    auto& progressCv      = syncPtr->cv;

    ThreadPool& pool = getStaticThreadPool();

    disableInput();
    clearScrollBuffer();

    // The screen prints the header, hides the cursor and starts its keeper thread.
    auto screen = std::make_shared<WriteScreen>(progressDataPtr, syncPtr, getWriteTheme());
    screen->start();

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < totalTasks; ++i) {
        futures.push_back(pool.enqueue([progressDataPtr, syncPtr, i]() {
            auto& progress = (*progressDataPtr)[i];
            const bool success = writeIsoToDevice(
                progress.sourcePath, progress.device, i, *progressDataPtr);

            if (success) {
                progress.completed.store(true);
                syncPtr->completedTasks.fetch_add(1);
            } else if (!GlobalState::g_operationCancelled.load()) {
                progress.failed.store(true);
            }

            syncPtr->finishedTasks.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(syncPtr->mutex);
            }
            // notify_all: the screen's keeper thread and the main thread wait on
            // this same condition variable, and notify_one could wake the wrong one.
            syncPtr->cv.notify_all();
        }));
    }

    // Wait for all tasks to finish, woken instantly by each task's
    // notify_all() rather than polling each future individually. Still
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

    signal(SIGINT, SIG_IGN);

    // Paint the result screen (rows, status, summary). The keeper thread stays
    // alive so the screen keeps reflowing on resize after this function returns.
    screen->finish();

    // Instantly hand focus and console command lines back to the caller
    flushStdin();
    restoreInput();

    // On cancellation the Drain manager for write2usb takes over
    if (GlobalState::g_operationCancelled.load()) {
        drainManagerForWrite2UsbUI(
            std::move(futures),
            progressData,
            progressDataPtr);
    } else {
        // Normal fast block cleanup sequence path if everything finished successfully
        for (auto& future : futures) {
            if (future.valid()) {
                future.wait();
            }
        }
    }

    return screen;
}

/**
 * @brief Entry point for the write-to-USB workflow.
 *
 * This function orchestrates the full flashing process:
 * 1. Parses the raw @p input string into unique ISO indices.
 * 2. Resolves filesystem metadata (size, name) for each selected ISO,
 *    skipping any files that are inaccessible or no longer present.
 * 3. Prompts the user for target device mappings via @ref collectDeviceMappings.
 * 4. Executes the flashing process via @ref performWriteOperation, which returns
 *    the live @ref WriteScreen.
 * 5. Keeps that screen alive while waiting for the user to press Enter, so the
 *    result screen still re-truncates and re-wraps when the terminal is resized,
 *    then stops it.
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

    // The screen keeps its keeper thread running after the write finishes.
    auto screen = performWriteOperation(validPairs);

    // Cleanup and wait for user acknowledgment
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    // A resize repaint wipes the screen, so the screen
    // reprints this text to put the prompt back under the summary.
    screen->setPrompt(std::string(color) + "\n↵ to continue...");


    pressEnterToContinue();

    screen->stop();   // no more repaints once the user moves on
}
