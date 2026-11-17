// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../inputHandling.h"
#include "../rendering.h"
#include "../pausePrompts.h"
#include "../readline.h"
#include "../select.h"
#include "../sort.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../threadpool.h"
#include "../tokenize.h"
#include "../write2usbUI.h"

namespace fs = std::filesystem;

/**
 * @brief Reads a block device's model name from sysfs.
 *
 * Reads /sys/block/<name>/device/model and trims surrounding whitespace.
 *
 * @param device Absolute block-device path (e.g. /dev/sdc).
 * @return Model name, or "Unknown Drive" if the file is absent or empty.
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
 * @brief Returns true if @p devicePath is a removable USB block device.
 *
 * Resolves symlinks (e.g. /dev/disk/by-id/...), requires the sysfs
 * "removable" attribute to read "1", then walks up the device's canonical
 * sysfs path looking for a USB device node (identified by its "idVendor"
 * file). Fails closed on any error.
 *
 * @param devicePath Block-device path (must start with /dev/).
 * @return true if the device is a removable USB block device.
 */
bool isFlashDrive(const std::string& devicePath) {
    namespace fs = std::filesystem;
    try {
        if (devicePath.size() <= 5 || devicePath.rfind("/dev/", 0) != 0) {
            return false;
        }

        std::error_code ec;

        const fs::path devNode = fs::canonical(devicePath, ec);
        if (ec) return false;

        const std::string name = devNode.filename().string();
        if (name.empty()) return false;

        const fs::path sysPath = fs::path("/sys/block") / name;
        if (!fs::exists(sysPath, ec) || ec) return false;

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
 * Skips loop devices, RAM disks, zram devices, and partition entries (names
 * ending in a digit). A device is included only when its removable sysfs
 * attribute reads "1" and isFlashDrive() confirms it is USB-attached.
 *
 * @return Absolute device paths such as /dev/sdb.
 */
std::vector<std::string> getRemovableDevices() {
    std::vector<std::string> devices;

    try {
        for (const auto& entry : fs::directory_iterator("/sys/block")) {
            const std::string deviceName = entry.path().filename().string();

            if (deviceName.rfind("loop", 0) == 0 ||
                deviceName.rfind("ram",  0) == 0 ||
                deviceName.rfind("zram", 0) == 0) {
                continue;
            }

            if (!deviceName.empty() && std::isdigit(static_cast<unsigned char>(deviceName.back()))) {
                continue;
            }

            std::ifstream removableFile(entry.path() / "removable");
            std::string removable;
            if (!(removableFile >> removable) || removable != "1") {
                continue;
            }

            const std::string devPath = "/dev/" + deviceName;

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
 * @brief Returns true if @p device or any of its partitions is mounted.
 *
 * Parses /proc/mounts and compares the base device name (without /dev/)
 * against each mounted entry, including partition nodes whose names start
 * with the same base and continue with a digit.
 *
 * @param device Absolute block-device path (e.g. /dev/sdb).
 * @return true if the device or a partition of it is mounted.
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
     * @param wt  Active write theme.
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
     * already themed (see plainErr and pathErr).
     *
     * @param wt   Active write theme.
     * @param errs Pre-formatted error messages.
     */
    void printValidationErrors(const WriteTheme& wt, const std::vector<std::string>& errs) {
        std::cerr << "\n" << wt.errLabel << "Validation errors:" << wt.rl_resetCol << wt.bold << "\n";
        for (const auto& e : errs)
            std::cerr << " \u2022 " << e << wt.bold << "\n";
        std::cerr << wt.rl_resetCol << (GlobalState::isNonInteractive ? "\n" : "");
    }

    /**
     * @brief Reports whether a block device still has write I/O in flight.
     *
     * Reads the read/write request counters in
     * /sys/block/<name>/inflight. This also detects writes still being flushed
     * by another process. Returns false if the counters cannot be read.
     *
     * @param device Whole-disk path (e.g. "/dev/sdb").
     * @return true if one or more write requests are still pending.
     */
    bool isDeviceBusy(const std::string& device) {
        const std::string name = std::filesystem::path(device).filename().string();
        std::ifstream in("/sys/block/" + name + "/inflight");
        unsigned long reads = 0, writes = 0;
        if (!(in >> reads >> writes))
            return false;
        return writes > 0;
    }

    /**
     * @brief Runs the device-level checks, independent of the paired ISO.
     *
     * Checks, in order:
     * -# The device is a removable USB flash drive (isFlashDrive).
     * -# The device and its partitions are not mounted (isDeviceMounted).
     * -# The device is not in @c g_drainingDevices, which tracks writes still
     *    being drained by this process after cancellation.
     * -# In non-interactive mode, the device has no write I/O in flight
     *    (@ref isDeviceBusy). This also catches pending writes from another
     *    process, which the in-process set cannot track.
     * -# The device size can be determined (getBlockDeviceSize).
     *
     * @param wt     Active write theme.
     * @param device Path used for the checks (canonical /dev/sdX).
     * @param shown  Path displayed in error messages (what the user typed).
     * @param[out] size Device size in bytes; only valid when no error is returned.
     * @return An error message for the first failed check, or std::nullopt if the
     *         device is ready for writing.
     */
    std::optional<std::string> checkDevice(const WriteTheme& wt, const std::string& device,
                                           const std::string& shown, uint64_t& size) {
        if (!isFlashDrive(device))
            return pathErr(wt, shown, " is not a removable USB flash device");

        if (isDeviceMounted(device))
            return pathErr(wt, shown, " or its partitions are mounted");

        {
            std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
            if (g_drainingDevices.count(device))
                return pathErr(wt, shown, " has pending kernel-level tasks");
        }

        if (GlobalState::isNonInteractive.load() && isDeviceBusy(device))
            return pathErr(wt, shown, " has pending kernel-level tasks");

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
     * @param device     Device path, used for the message and to look up the
     *                   drive name.
     * @param deviceSize Device capacity in bytes (as returned by checkDevice).
     * @return A themed validation error with aligned size details (filename,
     *         device path, drive name), or std::nullopt if the ISO fits.
     */
    std::optional<std::string> checkCapacity(const WriteTheme& wt, const IsoInfo& iso,
                                             const std::string& device, uint64_t deviceSize) {
        if (iso.size <= deviceSize)
            return std::nullopt;

        const std::string deviceSizeStr = formatFileSize(deviceSize);
        const std::string driveName = getDriveName(device);

        const auto isoLabelPadded   = std::format("    {:<5} = ", "ISO");
        const auto driveLabelPadded = std::format("    {:<5} = ", "Drive");

        std::string e;
        e.append(wt.errLabel).append("ISO exceeds target drive capacity:\n")
         .append(wt.errLabel).append(isoLabelPadded)
         .append(wt.sizeCol).append(iso.sizeStr).append(wt.rl_resetCol)
         .append(UI::Palette::DimGray).append(" (").append(wt.fileCol).append(iso.filename)
         .append(UI::Palette::DimGray).append(")\n")
         .append(wt.errLabel).append(driveLabelPadded)
         .append(wt.sizeCol).append(deviceSizeStr).append(wt.rl_resetCol)
         .append(UI::Palette::DimGray).append(" (").append(wt.warnLabel).append(device)
         .append(wt.infoLabel).append(" <").append(driveName).append(">")
         .append(UI::Palette::DimGray).append(")")
         .append(wt.rl_resetCol);

        return e;
    }

} // namespace WriteValidation

/**
 * @brief Validates a set of ISO-to-device mappings and returns only the
 * viable pairs.
 *
 * If @p noPermissions is true, validation is skipped and a
 * "Root privileges are required" error is reported. Otherwise each pair is
 * checked with WriteValidation::checkDevice and then
 * WriteValidation::checkCapacity.
 *
 * Errors are collected into @p validationErrors and an empty vector is
 * returned if any occur, even if some pairs were valid. Valid pairs are
 * returned only when every pair passes all checks. Reporting the errors
 * (printing, prompting, signal handling) is the caller's job.
 *
 * @param deviceMap        Pairs of (1-based ISO index, device path) to validate.
 * @param selectedIsos     Ordered ISOs corresponding to the indices.
 * @param noPermissions    True if the process lacks root; validation is
 *                         skipped and a privilege error is reported.
 * @param validationErrors Receives one message per failure.
 * @return The valid (IsoInfo, device) pairs, or an empty vector if any
 *         validation failed.
 */
std::vector<std::pair<IsoInfo, std::string>> validateDevices(
        const std::vector<std::pair<size_t, std::string>>& deviceMap,
        const std::vector<IsoInfo>& selectedIsos,
        bool noPermissions,
        std::vector<std::string>& validationErrors) {

    using namespace WriteValidation;
    const WriteTheme wt = getWriteTheme();

    std::vector<std::pair<IsoInfo, std::string>> validPairs;

    if (noPermissions) {
        validationErrors.push_back(plainErr(wt, "Root privileges are required"));
        return {};
    }

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

    return validPairs;
}

/**
 * @brief Parses a semicolon-delimited mapping string into (index, device) pairs.
 *
 * Expected format: "1>/dev/sdb;2>/dev/sdc", where each token is
 * INDEX>DEVICE_PATH. Whitespace around the index and the device path is
 * ignored (e.g. "1 > /dev/sdb"). Enforced:
 *  - Valid INDEX>DEVICE format per token.
 *  - Index within [1, selectedIsos.size()].
 *  - Device path starts with /dev/ and exists.
 *  - No device used more than once, compared after symlink resolution (e.g.
 *    /dev/sdb and its /dev/disk/by-id/usb-... alias count as the same device).
 *  - Every ISO index in @p selectedIsos has at least one mapping.
 *
 * @par Symlink resolution
 * Each device path is resolved with std::filesystem::canonical(), so the
 * returned paths are always the real device nodes (e.g. /dev/sdb), never the
 * alias the user typed. Callers must use the returned path for every later
 * check and for the write itself. Messages that show a device afterwards
 * therefore display the resolved path.
 *
 * Errors are appended to @p errors; the caller inspects this vector to
 * decide whether to re-prompt. Error messages for a rejected token show the
 * path as the user typed it, except for the duplicate-device error, which
 * shows the resolved path.
 *
 * This parser checks path existence and mapping uniqueness only. It does
 * not verify that a device is removable, unmounted, or large enough; those
 * checks are performed later by validateDevices().
 *
 * @param pairString   Raw input containing semicolon-separated mappings.
 * @param selectedIsos Ordered ISO list; only its size() is used, to bound
 *                     valid indices.
 * @param[out] errors  Cleared, then populated with human-readable error
 *                     messages.
 * @return Vector of valid (1-based index, canonical device path) pairs.
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

    auto trim = [](std::string& s) {
        s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
        s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    };

    while (std::getline(pairStream, pair, ';')) {
        trim(pair);
        if (pair.empty()) continue;

        size_t sepPos = pair.find('>');
        if (sepPos == std::string::npos) {
            errors.push_back(wt.warnLabel + "Invalid pair format: '" + wt.errLabel + pair + wt.warnLabel + "'");
            continue;
        }

        std::string indexStr = pair.substr(0, sepPos);
        std::string device   = pair.substr(sepPos + 1);
        trim(indexStr);
        trim(device);

        try {
            size_t index = std::stoul(indexStr);

            if (index < 1 || index > selectedIsos.size()) {
                errors.push_back(wt.warnLabel + "Invalid index '" + wt.errLabel + indexStr + wt.warnLabel + "'");
                continue;
            }

            if (device.rfind("/dev/", 0) != 0) {
                errors.push_back(wt.warnLabel + "Invalid device path: '" + wt.errLabel + device + wt.warnLabel + "'");
                continue;
            }

            std::error_code ec;
            const fs::path canon = fs::canonical(device, ec);
            if (ec) {
                errors.push_back(wt.warnLabel + "Device not found: '" + wt.errLabel + device + wt.warnLabel + "'");
                continue;
            }
            device = canon.string();

            if (usedDevices.count(device)) {
                errors.push_back(wt.warnLabel + "Device '" + wt.errLabel + device + wt.warnLabel + "' used multiple times");
                continue;
            }

            deviceMap.emplace_back(index, device);
            usedDevices.insert(device);

        } catch (...) {
            errors.push_back(wt.warnLabel + "Invalid index: '" + wt.errLabel + indexStr + wt.warnLabel + "'");
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
 * @brief Represents summary information for a physical removable storage device.
 */
struct DeviceInfo {
    std::string path;     ///< Device node path (e.g., /dev/sdb).
    uint64_t size;        ///< Total storage capacity in bytes.
    std::string driveName;///< Vendor or model drive name.
    std::string sizeStr;  ///< Human-readable formatted file size string.
    bool mounted;         ///< True if the device currently has active mounts.
    bool error;           ///< True if an error occurred while probing the device.
};

/**
 * @brief Represents a single row in the pre-burn warning confirmation table.
 */
struct WarnRow {
    std::string device;       ///< Target device path.
    std::string driveName;    ///< Drive model/name description.
    std::string deviceSizeStr;///< Formatted storage device size.
    std::string filename;     ///< Source ISO filename.
    std::string isoSizeStr;   ///< Formatted ISO image size.
};

/**
 * @brief Constructs the interactive index prompt string for device mapping input.
 *
 * @param wt The active write theme configuration.
 * @return std::string Formatted ANSI-colored prompt string.
 */
static std::string buildIdxPrompt(const WriteTheme& wt) {
    std::ostringstream s;
    s << "\n" << UI::Palette::RL_idxGreen << "IDX>DEVICE"
      << wt.rl_primaryCol << " ↵ for "
      << UI::Palette::RL_Yellow << "write2usb"
      << wt.rl_primaryCol << ", ? for help: "
      << wt.rl_resetCol;
    return s.str();
}

/**
 * @brief Validates whether the number of selected ISOs exceeds the thread pool capacity.
 *
 * @Reference pool Reference to the application thread pool.
 * @param selectedIsos Vector of ISO files chosen by the user.
 * @param wt The active write theme configuration.
 * @return true If the selection limit has been exceeded.
 * @return false If within safe execution limits.
 */
static bool checkThreadPoolLimit(ThreadPool& pool, const std::vector<IsoInfo>& selectedIsos, const WriteTheme& wt) {
    if (selectedIsos.size() <= pool.threadCount()) return false;
    auto render = [&] {
        SyncOutput sync;
        clearScrollBuffer();
        std::cout << "\n"
                  << wt.colorFailure << "Error: "
                  << UI::Palette::Yellow << "write2usb"
                  << wt.colorFailure << " selection limit" << wt.colorFailure << " is "
                  << wt.colorWarning << pool.threadCount()
                  << wt.colorFailure << " (thread pool size)."
                  << wt.speedCol << "\n";
    };
    pressEnterToTryWithRepaint(render);
    return true;
}

/**
 * @brief Collects probe information and sizes for a list of USB device paths.
 *
 * @param usbDevices Vector of detected USB device node paths.
 * @return std::vector<DeviceInfo> Sorted and populated device details.
 */
static std::vector<DeviceInfo> collectAndSortDeviceInfos(const std::vector<std::string>& usbDevices) {
    std::vector<DeviceInfo> deviceInfos;
    deviceInfos.reserve(usbDevices.size());
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
    return deviceInfos;
}

/**
 * @brief Builds the comprehensive device selection prompt string listing ISOs and targets.
 *
 * @param selectedIsos Original collection of selected ISO inputs.
 * @param[out] sortedIsos Output parameter containing size-sorted ISO metadata.
 * @param[out] usbDevices Output parameter containing detected removable block devices.
 * @param wt The active write theme configuration.
 * @return std::string The fully formatted multi-line console prompt.
 */
static std::string buildDevicePromptString(
    const std::vector<IsoInfo>& selectedIsos,
    std::vector<IsoInfo>& sortedIsos,
    std::vector<std::string>& usbDevices,
    const WriteTheme& wt
) {
    sortedIsos = selectedIsos;
    std::sort(sortedIsos.begin(), sortedIsos.end(), [](const IsoInfo& a, const IsoInfo& b) {
        return a.size > b.size;
    });

    std::ostringstream stream;
    stream << "\n" << color << "Selected " << wt.fileCol << "ISO" << color << ":\n\n";

    for (size_t i = 0; i < sortedIsos.size(); ++i) {
        auto [isoDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write2usb");
        stream << " " << wt.indexCol << (i + 1) << ">" << wt.bold << " ";
        if (!displayConfig::toggleNamesOnly) {
            stream << wt.pathCol << isoDir << "/";
        }
        stream << wt.fileCol << filename;
        stream << UI::Palette::DimGray << " (" << wt.sizeCol << sortedIsos[i].sizeStr
               << UI::Palette::DimGray << ")\n";
    }

    stream << "\n" << color << "Removable USB Flash Devices:" << wt.rl_resetCol << "\n\n";
    usbDevices = getRemovableDevices();
    auto deviceInfos = collectAndSortDeviceInfos(usbDevices);

    if (deviceInfos.empty()) {
        stream << " " << wt.colorFailure << "No USB flash drives detected!" << wt.rl_resetCol << "\n";
    } else {
        for (const auto& dev : deviceInfos) {
            if (dev.error) {
                stream << " " << wt.colorFailure << dev.path << " (error)" << wt.rl_resetCol << "\n";
            } else {
                stream << " " << wt.deviceCol << dev.path
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

    stream << buildIdxPrompt(wt);
    return stream.str();
}

/**
 * @brief Generates warning row dataset structures from validated ISO-to-device mapping pairs.
 *
 * @param validPairs Vector of validated ISO and target device mappings.
 * @return std::vector<WarnRow> Compact structural rows ready for confirmation formatting.
 */
static std::vector<WarnRow> buildWarnRows(const std::vector<std::pair<IsoInfo, std::string>>& validPairs) {
    std::vector<WarnRow> warnRows;
    warnRows.reserve(validPairs.size());
    for (const auto& [iso, device] : validPairs) {
        warnRows.push_back({ device,
                             getDriveName(device),
                             formatFileSize(getBlockDeviceSize(device)),
                             iso.filename,
                             iso.sizeStr });
    }
    return warnRows;
}

/**
 * @brief Renders the destructive action confirmation table layout for terminal UI.
 *
 * @param cols Current terminal column width constraint.
 * @param clearFirst If true, issues screen reset sequence codes before painting.
 * @param devicePrompt Original prompt content block string.
 * @param mainInputString User-supplied input string to echo back.
 * @param warnRows Structural warning metadata row items.
 * @param wt The active write theme configuration.
 * @return std::vector<size_t> Calculated character widths per rendered line for window resizing hooks.
 */
static std::vector<size_t> renderConfirmRows(
    size_t cols,
    bool clearFirst,
    const std::string& devicePrompt,
    const std::string& mainInputString,
    const std::vector<WarnRow>& warnRows,
    const WriteTheme& wt
) {
    const size_t usableCols = cols > 1 ? cols - 1 : 1;
    std::vector<size_t> widths;

    if (clearFirst) {
        std::cout << "\033[H\033[2J\033[3J";
        std::cout << promptHeaderForRepaint(devicePrompt);
        std::cout << buildIdxPrompt(wt) << mainInputString << "\n";
        std::cout << "\n";
    } else {
        std::cout << "\n";
        widths.push_back(0);
    }

    std::cout << wt.colorWarning << "WARNING: This will "
              << UI::Palette::Red << "*ERASE ALL DATA*"
              << wt.colorWarning << " on:" << wt.rl_resetCol << "\n\n";
    widths.push_back(0);
    widths.push_back(utf8Width("WARNING: This will *ERASE ALL DATA* on:"));
    widths.push_back(0);
    rl_bind_keyseq("?", rl_insert);
    for (const auto& r : warnRows) {
        const size_t fixedWidth = 17 + r.device.size() + utf8Width(r.driveName)
                                + utf8Width(r.deviceSizeStr) + utf8Width(r.isoSizeStr);
        const size_t availForName = usableCols > fixedWidth ? usableCols - fixedWidth : 0;
        const std::string shownName = truncateIsoName(r.filename, availForName);

        std::ostringstream row;
        row << UI::Palette::DimGray << " {" << wt.deviceCol << r.device
            << " " << wt.infoLabel << "<" << r.driveName << ">" << UI::Palette::DimGray << " ("
            << wt.sizeCol << r.deviceSizeStr
            << UI::Palette::DimGray << ")} " << color << "←" << UI::Palette::DimGray << " {" << wt.fileCol
            << shownName << UI::Palette::DimGray << " (" << wt.sizeCol
            << r.isoSizeStr << UI::Palette::DimGray << ")}";

        const std::string clipped = clipToWidth(row.str(), usableCols);
        std::cout << clipped << "\n";
        widths.push_back(std::min(visibleWidth(clipped), usableCols));
    }

    std::cout << "\n";
    widths.push_back(0);
    return widths;
}

/**
 * @brief Interactive loop to collect, parse, validate, and confirm mappings between ISO images and USB storage targets.
 *
 * @param selectedIsos Vector of selected ISO files designated for writing.
 * @param[out] pendingIndices Output container for tracking unassigned or pending index identifiers.
 * @return std::vector<std::pair<IsoInfo, std::string>> Vector of confirmed (ISO, Target Device) association pairs.
 */
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos, std::vector<std::string>& pendingIndices) {
    PendingReadlineTextClearGuard pendingTextGuard;
    const WriteTheme wt = getWriteTheme();
    const bool noPermissions = (geteuid() != 0);

    std::string pendingInput;
    static std::string prefillText;
    static rl_hook_func_t* prevStartupHook = nullptr;

    while (true) {
        TerminalStateGuard termGuard;
        ThreadPool& pool = getStaticThreadPool();
        if (checkThreadPoolLimit(pool, selectedIsos, wt)) {
            return {};
        }

        displayErrors();

        std::vector<IsoInfo> sortedIsos;
        std::vector<std::string> usbDevices;
        std::string devicePrompt = buildDevicePromptString(selectedIsos, sortedIsos, usbDevices, wt);

        const auto repaintDevices = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << promptHeaderForRepaint(devicePrompt);
        };

        armPendingInputRestore();

        prefillText = pendingInput;
        pendingInput.clear();
        if (!prefillText.empty()) {
            prevStartupHook = rl_startup_hook;
            rl_startup_hook = +[]() -> int {
                if (prevStartupHook) prevStartupHook();
                if (!prefillText.empty()) rl_insert_text(prefillText.c_str());
                return 0;
            };
        }

        RlLine deviceInput = readlineWithResize(devicePrompt, repaintDevices);
        rl_startup_hook = nullptr;
        prevStartupHook = nullptr;
        prefillText.clear();

        if (!deviceInput || deviceInput.get()[0] == '\0') {
            continue;
        }

        std::string mainInputString(deviceInput.get());
        if (mainInputString == "\x1b") {
            return {};
        }

        if (mainInputString == "?") {
            helpMappings();
            continue;
        }

        if (deviceInput && *deviceInput) add_history(deviceInput.get());

        auto renderErrors = [&](const std::vector<std::string>& errs) {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << promptHeaderForRepaint(devicePrompt);
            std::cout << buildIdxPrompt(wt) << mainInputString << "\n";
            std::cerr << "\n" << wt.rl_errorCol << "Errors:" << wt.rl_resetCol << "\n";
            for (const auto& e : errs) {
                std::cerr << color << " • " << e << "\n";
            }
        };

        std::vector<std::string> isoFilenames;
        isoFilenames.reserve(sortedIsos.size());
        for (const auto& iso : sortedIsos) {
            isoFilenames.push_back(iso.path);
        }

        std::vector<std::string> errors;
        auto deviceMap = parseDeviceMappings(deviceInput.get(), isoFilenames, errors);
        if (!errors.empty()) {
            pressEnterToTryWithRepaint([&] { renderErrors(errors); });
            continue;
        }

        std::vector<std::string> validationErrors;
        auto validPairs = validateDevices(deviceMap, sortedIsos, noPermissions, validationErrors);
        if (!validationErrors.empty()) {
            signal(SIGINT, SIG_IGN);
            disable_ctrl_d();

            if (noPermissions) {
                pressEnterToContinueWithRepaint([&] { renderErrors(validationErrors); });
            } else {
                pressEnterToTryWithRepaint([&] { renderErrors(validationErrors); });
            }
            continue;
        }

        if (validPairs.empty()) {
            pendingInput = mainInputString;
            continue;
        }

        auto warnRows = buildWarnRows(validPairs);
        const size_t drawnCols = terminalSize().cols;
        std::vector<size_t> drawnWidths;
        {
            SyncOutput sync;
            drawnWidths = renderConfirmRows(drawnCols, /*clearFirst=*/false, devicePrompt, mainInputString, warnRows, wt);
        }

        const std::string promptText = "Proceed? (y/n): ";
        const std::string confirmPrompt =
            "\001" + std::string(color) + "\002" + promptText + wt.rl_resetCol;

        std::unique_ptr<char, decltype(&std::free)> confirmation(nullptr, &std::free);
        {
            ConfirmationModeGuard confirmGuard;
            ConfirmResizeWatcher watcher(
                [&](size_t cols) { return renderConfirmRows(cols, /*clearFirst=*/true, devicePrompt, mainInputString, warnRows, wt); },
                drawnCols, drawnWidths, promptText);

            confirmation.reset(readline(confirmPrompt.c_str()));
            watcher.stop();
        }

        std::string ans;
        if (confirmation) {
            ans = confirmation.get();
            while (!ans.empty() && std::isspace(static_cast<unsigned char>(ans.back())))
                ans.pop_back();
        }

        if (ans == "y" || ans == "Y") {
            setupSignalHandlerCancellations();
            GlobalState::g_operationCancelled.store(false);
            g_isoPending.paths.clear();
            pendingIndices.clear();
            return validPairs;
        } else {
            rl_bind_keyseq("?", help_handler);
        }

        pendingInput = mainInputString;
    }
}

// ------------------------------------------------------------------------- //
// write2usbUI Block                                                        //
// ------------------------------------------------------------------------- //

/**
 * @brief Builds the per-task ProgressInfo vector from (IsoInfo, device)
 * pairs, in the order they would appear after a stable, case-insensitive
 * natural sort by ISO filename (device path as tie-break).
 *
 * The caller's vector is not modified or copied. A vector of indices into
 * @p validPairsIn is stable-sorted instead, and the pairs are visited in
 * that order; only the four fields ProgressInfo carries (filename, device,
 * sizeStr, sourcePath) are copied, directly into the progress vector.
 *
 * The progress vector is reserved to exactly @p validPairsIn.size() before
 * filling, so its backing storage never reallocates after construction
 * (ProgressInfo holds atomics and is therefore non-movable — the reserve is
 * load-bearing, not just an optimisation).
 *
 * The returned shared_ptr is captured by every pooled write task and by the
 * cancellation drain, so the entries outlive this function whenever a task
 * or drain thread still needs them.
 *
 * @par Ordering
 * Primary key: IsoInfo::filename via naturalCompare (case-insensitive,
 * numeric-aware: "archlinux-9.iso" before "archlinux-10.iso"). Secondary
 * key: the device path under ordinary std::string ordering. Equal
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
    std::vector<size_t> order(validPairsIn.size());
    std::iota(order.begin(), order.end(), size_t{0});

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const int c = naturalCompare(validPairsIn[a].first.filename,
                                     validPairsIn[b].first.filename);
        if (c != 0) return c < 0;
        return validPairsIn[a].second < validPairsIn[b].second;
    });

    auto progressPtr = std::make_shared<std::vector<ProgressInfo>>();
    auto& progress   = *progressPtr;
    progress.reserve(validPairsIn.size());

    for (size_t idx : order) {
        const auto& [iso, device] = validPairsIn[idx];
        progress.push_back(ProgressInfo{iso.filename, device, iso.sizeStr, iso.path});
    }
    return progressPtr;
}

/**
 * @brief Detaches in-flight write tasks into per-device background drain
 * threads after cancellation.
 *
 * Called by performWriteOperation() after Ctrl+C
 * (@c GlobalState::g_operationCancelled). Transfers the outstanding futures
 * to background drain threads managed by @c g_drainingManager, allowing
 * writes and pending cache flushes to finish after the operation returns to
 * the prompt.
 *
 * One managed background thread is started per future. Each waits on its
 * future and, on completion, erases that device from @c g_drainingDevices.
 * Because every pending device has an independent waiter, a device's
 * "[FLUSHING]" indicator clears the moment its task finishes, regardless of
 * how long the others take. When the last thread empties the set,
 * g_drainingCancelled is cleared.
 *
 * Only devices whose tasks had neither completed nor failed when
 * cancellation was handled are added to @c g_drainingDevices. Tasks that
 * have already returned from writeIsoToDevice() need no further draining.
 *
 * The progress vector is shared rather than copied or moved. Each drain
 * thread captures the shared pointer to keep the per-operation records alive
 * until its future completes. Device paths are paired with futures by index
 * through @p progressData[i].device.
 *
 * @param futures        Per-task futures, taken by value and moved into the
 *                       background drain threads.
 * @param progressData   Per-task progress records. Read only: .completed /
 *                       .failed are inspected to decide which devices still
 *                       need draining, and .device enumerates the draining
 *                       set.
 * @param progressDataPtr Shared ownership of the progress records; captured
 *                       by each detached thread to keep the operation data
 *                       alive until that device's drain completes.
 *
 * @note The caller's future vector is consumed by this call; ownership of
 *       each future is transferred to its background drain thread.
 *
 * @note @p futures and @p progressData must have the same size:
 *       drainDevices[i] is paired with futures[i], and each spawned thread
 *       indexes both. A mismatch is an out-of-bounds read.
 *
 * @note May add multiple threads to @c g_drainingManager in one call
 *       (one per future).
 *
 * @note Only for the cancellation path. The non-cancelled path in
 *       performWriteOperation waits on the futures inline instead, because
 *       nothing needs to outlive the function in that case.
 *
 * @see g_drainingDevices, g_drainingCancelled, g_drainingManager
 */
void drainManagerForWrite2UsbUI(
    std::vector<std::future<void>> futures,
    const std::vector<ProgressInfo>& progressData,
    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr)
{
    std::vector<std::string> drainDevices;
    drainDevices.reserve(progressData.size());

    bool anyDraining = false;
    {
        std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
        for (size_t i = 0; i < progressData.size(); ++i) {
            const std::string& device = progressData[i].device;
            drainDevices.push_back(device);

            if (!progressData[i].completed.load() && !progressData[i].failed.load()) {
                g_drainingDevices.insert(device);
                anyDraining = true;
            }
        }
    }
    g_drainingCancelled.store(anyDraining);

    for (size_t i = 0; i < futures.size(); ++i) {
        g_drainingManager.add(std::thread([
            f = std::move(futures[i]),
            dev = drainDevices[i],
            progressDataPtr
        ]() mutable {
            (void)progressDataPtr;

            if (f.valid()) {
                f.wait();
            }

            std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
            g_drainingDevices.erase(dev);

            if (g_drainingDevices.empty()) {
                g_drainingCancelled.store(false);
            }
        }));
    }
}


/**
 * @brief Executes validated ISO-to-device writes and manages the progress
 * screen plus completion / cancellation handling.
 *
 * Creates the per-task progress vector with prepareWriteData, which
 * establishes the ordering by natural, case-insensitive ISO filename sort,
 * device path as tie-breaker. Task index and display-row index stay aligned.
 *
 * Submits one writeIsoToDevice call per task to the global ThreadPool. A
 * shared WriteSync state coordinates task completion between worker threads,
 * the main thread and the WriteScreen keeper.
 *
 * @par Normal completion
 * Waits until all tasks finish, disables further SIGINT handling, and paints
 * the result screen. The returned screen stays alive so the caller can
 * display a prompt and keep resize-triggered repainting. All valid futures
 * are waited on before returning.
 *
 * @par Cancellation (Ctrl+C)
 * The cancellation flag is part of the wait predicate. The signal handler
 * cannot safely notify the condition variable, so WriteScreen chains its own
 * SIGINT handler in front of the cancellation handler and relays the signal
 * through its self-pipe; the relay thread then notifies the condition
 * variable and the main thread wakes immediately, with no periodic recheck.
 * Only if that relay could not be installed does the wait fall back to
 * rechecking every 500 ms.
 *
 * When cancellation is detected, the result screen is painted and terminal
 * input state is restored. Any tasks still running are transferred to
 * drainManagerForWrite2UsbUI, which keeps their futures alive while they
 * finish in the background. This allows outstanding writes and OS page-cache
 * flushing to continue. The drain manager removes each device from
 * g_drainingDevices when its drain completes, clearing its [FLUSHING]
 * indicator.
 *
 * The result screen reflects task states at the time it is painted; tasks
 * that continue running after cancellation may finish later. The successful
 * count in the summary is the count recorded at that time.
 *
 * @par Lifetime and ownership
 * The progress vector and synchronization state are held by shared pointers
 * captured by worker tasks and retained by the screen. Cancellation transfers
 * the outstanding futures and the required shared state to the drain manager,
 * preventing those objects from being destroyed while background tasks still
 * depend on them.
 *
 * The caller owns the returned shared_ptr<WriteScreen>. It should retain the
 * screen while waiting for user input and stop the screen when
 * resize-triggered repainting is no longer needed, or allow the screen to be
 * destroyed when its shared ownership ends.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs to write. Not
 *                     modified.
 *
 * @return A shared pointer to the live WriteScreen, allowing the caller to
 *         retain the result screen while waiting for user input.
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

    auto screen = createWriteScreen(progressDataPtr, syncPtr, getWriteTheme());
    startWriteScreen(*screen);

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
            // notify_all: the screen's keeper thread and the main thread wait
            // on this same condition variable, and notify_one could wake the
            // wrong one.
            syncPtr->cv.notify_all();
        }));
    }

    // Wait for all tasks to finish. Each task's notify_all() wakes this
    // thread on completion. Ctrl+C is also a wakeup: a SIGINT handler cannot
    // notify a condition variable itself, but WriteScreen chains into it and
    // relays the signal through its self-pipe, so its relay thread notifies
    // this same condition variable after g_operationCancelled is set. The
    // wait is therefore unbounded, with no periodic recheck.
    //
    // Only if that relay could not be set up (pipe/handler failure) do we
    // fall back to a 500 ms recheck so cancellation still works.
    {
        std::unique_lock<std::mutex> lock(progressCvMutex);
        const auto done = [&] {
            return finishedTasks.load() == totalTasks ||
                   GlobalState::g_operationCancelled.load();
        };

        if (writeScreenCancelWakeable(*screen)) {
            progressCv.wait(lock, done);
        } else {
            while (!done())
                progressCv.wait_for(lock, std::chrono::milliseconds(500), done);
        }
    }

    signal(SIGINT, SIG_IGN);

    finishWriteScreen(*screen);

    flushStdin();
    restoreInput();

    if (GlobalState::g_operationCancelled.load()) {
        drainManagerForWrite2UsbUI(
            std::move(futures),
            progressData,
            progressDataPtr);
    } else {
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
 * Orchestrates the full flashing process:
 * 1. Parses the raw @p input string into unique ISO indices.
 * 2. Resolves filesystem metadata (size, name) for each selected ISO,
 *    skipping any files that are inaccessible or no longer present.
 * 3. Prompts the user for target device mappings via collectDeviceMappings.
 * 4. Clears staged pending indices and paths upon successful confirmation.
 * 5. Executes the flashing process via performWriteOperation, which returns
 *    the live WriteScreen.
 * 6. Keeps that screen alive while waiting for the user to press Enter, so
 *    the result screen still re-truncates and re-wraps when the terminal is
 *    resized, then stops it.
 *
 * @param input          Raw selection string from the main menu (e.g. "1 3-5").
 * @param isoFiles       Full ordered list of available ISO paths.
 * @param pendingIndices Staged selection display tokens to be cleared on confirmation.
 */
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::vector<std::string>& pendingIndices) {
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

    auto validPairs = collectDeviceMappings(selectedIsos, pendingIndices);
    if (validPairs.empty()) {
        return;
    }

    auto screen = performWriteOperation(validPairs);

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    setWriteScreenPrompt(*screen, std::string(color) + "\n↵ to continue...");

    pressEnterToContinueWithRepaint([&] { repaintWriteScreen(*screen); });

    stopWriteScreen(*screen);
}
