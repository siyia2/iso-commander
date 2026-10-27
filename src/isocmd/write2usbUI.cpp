// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
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
        std::cerr << " \u2022 " << e << wt.bold << "\n";
    std::cerr << wt.rl_resetCol << (GlobalState::isNonInteractive ? "\n" : "");
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
bool isDeviceBusy(const std::string& device) {
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
 * -# The device is a removable USB flash drive (@ref isFlashDrive).
 * -# The device and its partitions are not mounted (@ref isDeviceMounted).
 * -# The device is not in @ref g_drainingDevices (pending kernel-level tasks
 *    tracked by the interactive UI).
 * -# In non-interactive mode only, the device has no write I/O still in
 *    flight (@ref isDeviceBusy). This catches flushing started by another
 *    process, such as a previously cancelled write, which the in-process
 *    @ref g_drainingDevices set cannot see.
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
 * @param device     Device path, used for the message and to look up the drive name.
 * @param deviceSize Device capacity in bytes (as returned by @ref checkDevice).
 * @return A themed validation error message with aligned size details (including
 *         filenames, device paths, and drive names), or std::nullopt if the ISO fits.
 */
std::optional<std::string> checkCapacity(const WriteTheme& wt, const IsoInfo& iso,
                                         const std::string& device, uint64_t deviceSize) {
    if (iso.size <= deviceSize)
        return std::nullopt;

    const std::string deviceSizeStr = formatFileSize(deviceSize);
    const std::string driveName = getDriveName(device);

    // Format just the padding and arrow with a fixed width of 10 for the label
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
 * @c INDEX>DEVICE_PATH. Whitespace around the index and the device path is
 * ignored (e.g. @c "1 > /dev/sdb"). The function enforces:
 *  - Valid @c INDEX>DEVICE format for each token.
 *  - Index within the range @c [1, selectedIsos.size()].
 *  - Device path starts with @c /dev/ and exists.
 *  - No device used more than once, compared after symlink resolution
 *    (e.g. @c /dev/sdb and its @c /dev/disk/by-id/usb-... alias count as
 *    the same device).
 *  - Every ISO index in @p selectedIsos has at least one mapping.
 *
 * @par Symlink resolution
 * Each device path is resolved with @c std::filesystem::canonical(), so the
 * returned paths are always the real device nodes (e.g. @c /dev/sdb), never
 * the alias the user typed. Callers must use the returned path for every
 * later check and for the write itself, so all of them refer to the same
 * device. Messages that show a device afterwards will therefore display the
 * resolved path.
 *
 * Errors are appended to @p errors; the caller inspects this vector to decide
 * whether to re-prompt the user. Error messages for a rejected token show the
 * path as the user typed it, except for the duplicate-device error, which
 * shows the resolved path.
 *
 * @note This function only checks that the path exists and is unique. It does
 *       not check that the device is a removable USB drive, unmounted, or large
 *       enough; that is done later by @ref validateDevices.
 *
 * @param pairString   Raw user input string containing semicolon-separated mappings.
 * @param selectedIsos Ordered ISO list; only its size() is used, to bound valid indices.
 * @param[out] errors  Vector that will be cleared and populated with human-readable error messages.
 * @return Vector of valid (1-based index, canonical device path) pairs extracted from the input.
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

            // Resolve symlinks (by-id, by-path, ...) to the real node. From here
            // on only the canonical path is used.
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
    }   // <-- end of while loop

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

// ------------------------------------------------------------------------- //
// Terminal / UTF-8 helpers (shared by WriteScreen and ConfirmResizeWatcher) //
// ------------------------------------------------------------------------- //

/**
 * @brief Visible columns of a UTF-8 string, counted by code point.
 *
 * Multi-byte UTF-8 sequences count as one column each, so a 3-byte
 * codepoint such as "…" contributes 1. Continuation bytes (the 0x80..0xBF
 * range, top two bits `10`) are skipped.
 *
 * @note This is a code-point count, not a Unicode display-width count:
 * East-Asian wide characters (CJK, emoji, etc.) are counted as 1 column
 * here even though terminals render them as 2. Correct for the ASCII and
 * common Latin-1 filenames this file handles.
 *
 * @param s UTF-8 string, possibly containing multi-byte sequences.
 * @return Number of visible columns @p s occupies.
 */
static size_t utf8Width(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++w;
    return w;
}

/**
 * @brief Visible width of a string that may contain ANSI CSI escapes.
 *
 * Like @ref utf8Width, but skips ANSI CSI sequences (`ESC [` … final byte
 * in 0x40..0x7E), which the terminal interprets rather than renders. The
 * escapes themselves count as 0 columns; every other UTF-8 code point
 * counts as 1.
 *
 * @param s String that may contain colour/formatting escapes.
 * @return Number of visible columns @p s occupies when printed.
 */
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

/**
 * @brief Cuts a string to at most @p maxCols visible columns.
 *
 * ANSI CSI escape sequences are copied through untouched and do not count
 * toward the budget, so colours that were opened before the cut remain
 * effective past it (the caller is responsible for any reset it needs).
 * UTF-8 multi-byte sequences are never split: an escape or code point that
 * would cross the limit is dropped whole.
 *
 * @param s       Input string, possibly with ANSI escapes and UTF-8.
 * @param maxCols Maximum number of visible columns to keep. 0 returns an
 *                empty string; ANSI escapes appearing before the first
 *                visible character are still emitted.
 * @return The longest prefix of @p s whose visible width is ≤ @p maxCols.
 */
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

/**
 * @brief Current terminal size in columns and rows.
 *
 * Queries @c TIOCGWINSZ on stdout. Falls back to {80, 24} when stdout is
 * not a tty, or when the ioctl fails or reports a zero dimension — the
 * usual "sane default" for a non-interactive caller.
 *
 * @return @c {columns, rows}.
 */
static std::pair<size_t, size_t> terminalSize() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
        return { ws.ws_col, ws.ws_row };
    return { 80, 24 };
}

/**
 * @brief Shortens an ISO filename to fit a column budget, keeping ".iso".
 *
 * If @p name already fits in @p maxCols columns it is returned unchanged.
 * Otherwise a trailing ".iso" extension (case-insensitive) is separated
 * off, the stem is cut to @c maxCols − (width of "…" + ".iso") columns
 * (floored at 4), and the result is reconstructed as @c "stem….iso".
 * Truncation walks whole UTF-8 code points, so a multi-byte sequence is
 * never split.
 *
 * @par Stem floor
 * @c MIN_STEM is 4 columns, so on very narrow terminals the returned
 * string can exceed @p maxCols by up to 5 columns (4 stem + 1 "…") when
 * an extension is present, or by 1 column when it is not. Callers should
 * clip the containing line with @ref clipToWidth to prevent wrap.
 *
 * @param name    Filename to shorten (may be empty, with or without a
 *                ".iso" extension).
 * @param maxCols Column budget the result should fit within; 0 forces the
 *                stem floor and therefore returns @c "….iso" or @c "…".
 * @return @p name unchanged if it fits, otherwise the shortened form.
 */
static std::string truncateIsoName(const std::string& name, size_t maxCols) {
    if (utf8Width(name) <= maxCols) return name;

    std::string ext;
    std::string stem = name;
    if (name.size() >= 4) {
        const std::string tail = name.substr(name.size() - 4);
        if (tail[0] == '.' &&
            std::tolower(static_cast<unsigned char>(tail[1])) == 'i' &&
            std::tolower(static_cast<unsigned char>(tail[2])) == 's' &&
            std::tolower(static_cast<unsigned char>(tail[3])) == 'o') {
            ext  = tail;
            stem = name.substr(0, name.size() - 4);
        }
    }

    constexpr size_t MIN_STEM = 4;                    // never shrink below this
    const size_t overhead = 1 + utf8Width(ext);       // "…" + ".iso"
    const size_t keep = std::max(MIN_STEM, maxCols > overhead ? maxCols - overhead : 0);

    std::string out;
    size_t cols = 0, i = 0;
    while (i < stem.size() && cols < keep) {
        size_t len = 1;
        while (i + len < stem.size() &&
               (static_cast<unsigned char>(stem[i + len]) & 0xC0) == 0x80) ++len;
        out.append(stem, i, len);
        i += len;
        ++cols;
    }
    if (i >= stem.size()) return name;                // stem fit after all, nothing to cut

    out += "\u2026";
    out += ext;
    return out;
}

// ------------------------------------------------------------------------- //
// Resize watcher (main-thread repaint via rl_event_hook)                     //
// ------------------------------------------------------------------------- //

/**
 * @brief Shared state between the resize watcher (background) and the
 *        repaint hook (main thread).
 *
 * The watcher thread only sets @ref pending (plus the new size); the
 * repaint hook on the main thread does all terminal I/O and readline
 * access, which is the only place where @c rl_line_buffer / @c rl_end
 * can be read without a data race.
 *
 * @par Threading
 * @ref pending, @ref pendingCols, @ref pendingRows and @ref lastCols are
 * @c std::atomic because both threads touch them. Everything else is
 * main-thread-only: @ref paint and @ref promptWidth are written once in
 * @ref ConfirmResizeWatcher's constructor (which runs on the main thread),
 * and @ref lineWidths is written only by @ref confirmResizeHook.
 *
 * @par Ownership
 * Instances are owned by the caller — normally a single file-scope object
 * (@ref g_confirmResize). The watcher and the hook both take it by
 * reference, so a `ConfirmResizeWatcher` cannot exist without a state, and
 * a hook cannot be installed without one. That makes the "one watcher at
 * a time" contract explicit at the type level.
 */
struct ResizeRepaintState {
    // Cross-thread signalling: written by watcher, consumed by hook.
    std::atomic<bool>   pending{false};
    std::atomic<size_t> pendingCols{0};
    std::atomic<size_t> pendingRows{0};
    std::atomic<size_t> lastCols{0};

    // Main-thread-only.
    std::function<std::vector<size_t>(size_t)> paint;
    std::vector<size_t> lineWidths;
    size_t promptWidth = 0;
};

/// The one state used by the confirmation prompt (see @ref ResizeRepaintState
/// for the "one watcher at a time" assumption).
static ResizeRepaintState g_confirmResize;

/**
 * @brief `rl_event_hook`: repaints the warning block on the main thread.
 *
 * Called by readline from its idle input loop when no keystroke is
 * pending. If the watcher has flagged a width change, this:
 * -# validates the pending resize (a zero `cols` or `rows` — a detached
 *    or transiently resized pty — is discarded so it cannot poison the
 *    row arithmetic below and cannot wedge a subsequent resize),
 * -# computes how far the cursor must travel to reach the top of the
 *    warning block: the block's own wrapped height **plus** the wrapped
 *    height of the prompt + typed buffer (@c rl_point tells us exactly
 *    where the cursor is inside that buffer),
 * -# if the cursor-up would clamp at row 1 (`up >= rows`), skips the
 *    repaint entirely — the terminal keeps the reflowed layout, the
 *    "ERASE ALL DATA" header stays visible, and the ISO / device lists
 *    above the block are preserved — **without** emitting any terminal
 *    escape (so no dangling `?2026h` is ever sent),
 * -# otherwise, opens a DEC mode 2026 synchronized-output envelope,
 *    moves the cursor up over the previous block + prompt, clears to
 *    end of screen, repaints the warning block at the new width, and
 *    calls @c rl_on_new_line() + @c rl_redisplay() so readline reprints
 *    the prompt and re-echoes whatever the user has typed, cursor
 *    position included, and
 * -# closes the 2026 envelope, which commits the whole repaint as a
 *    single atomic frame — the cursor moves, the clear, the new warning
 *    block, and the redrawn prompt + input all present together. Without
 *    this, a resize while the user is mid-type flashes through several
 *    intermediate states as each `<<` write lands.
 *
 * The envelope is owned by an RAII guard (@ref SyncOutput) rather than
 * two raw writes, so a throwing @c paint() callback — a @c std::function
 * invocation that can legitimately raise @c bad_alloc or propagate a
 * user exception — still emits the closing `?2026l`. Otherwise the
 * terminal would sit in sync mode until its watchdog fired (~1 s on
 * every implementation that supports 2026), turning a resize into a
 * one-second freeze.
 *
 * A @c std::cout flush is issued *inside* the envelope, immediately
 * before @c rl_redisplay(). With the default `sync_with_stdio(true)`
 * this is redundant (both streams share the C `stdout` buffer and
 * preserve ordering), but if anyone ever sets `sync_with_stdio(false)`
 * for throughput, `std::cout` gets its own buffer and readline's writes
 * could overtake ours on the fd — splitting the frame and reintroducing
 * the flash. The flush makes the ordering explicit and is free.
 *
 * Restoring the typed input is possible here — and only here — because
 * `rl_line_buffer` / `rl_end` / `rl_point` are mutated by readline on the
 * same thread. Reading them from the watcher thread would be a data race.
 *
 * @note The envelope must close before this hook returns. Readline may
 *       immediately re-enter its idle loop and block on input; a still-open
 *       `?2026h` at that point would leave the terminal holding a
 *       half-committed frame until the watchdog fires.
 *
 * @param s Shared state populated by @ref ConfirmResizeWatcher.
 * @return Always 0 (do not abort readline).
 */
static int confirmResizeHook(ResizeRepaintState& s) {
    if (!s.pending.load(std::memory_order_acquire))
        return 0;

    const size_t cols = s.pendingCols.load(std::memory_order_relaxed);
    const size_t rows = s.pendingRows.load(std::memory_order_relaxed);

    // A detached or transiently resized pty can report a zero dimension.
    // Consuming `pending` here (rather than after the guards) would be
    // wrong: a later, valid resize must still be able to fire. Bail
    // without clearing the flag so the next loop() iteration re-arms it.
    // `cols == 0` in particular would divide-by-zero in the row math below.
    if (cols == 0 || rows == 0) return 0;

    s.pending.store(false, std::memory_order_release);

    // Rows occupied by the warning block at the new width.
    size_t warningRows = 0;
    for (size_t w : s.lineWidths)
        warningRows += std::max<size_t>(1, (w + cols - 1) / cols);

    // Cursor is somewhere in the prompt+buffer block, which sits directly
    // below the warning block. rl_point is the cursor offset inside the
    // buffer; adding promptWidth gives the column from start of prompt.
    // Integer division by cols gives the physical line the cursor is on.
    const size_t cursorCols = s.promptWidth + rl_point;
    const size_t cursorLine = cursorCols / cols;

    const size_t up = warningRows + cursorLine;

    // Skip guard: if cursor-up would clamp at row 1, do nothing. Clamping
    // would overwrite the "ERASE ALL DATA" warning from the top and drop
    // the ISO / device lists out of view. Crucially, nothing has been
    // written to the terminal yet — a lone `?2026h` followed by an early
    // return would leave sync mode open and force the watchdog to commit
    // an empty frame.
    if (up >= rows) return 0;

    // One atomic frame: cursor movement + clear + warning block + prompt
    // + typed input all present together at `?2026l`.
    SyncOutput sync;

    if (up > 0) std::cout << "\033[" << up << "A";
    std::cout << "\r\033[J";

    // Repaint the warning block at the new width.
    s.lineWidths = s.paint(cols);

    // Drain our buffer before readline writes to the same fd. Redundant
    // under the default sync_with_stdio(true), load-bearing if anyone
    // ever disables it.
    std::cout.flush();

    // Restore the prompt + the user's typed text. This is the whole point
    // of moving the repaint to the main thread: readline's globals are
    // safe to touch here, and rl_redisplay puts the cursor back where it
    // was inside the buffer.
    rl_on_new_line();
    rl_redisplay();

    return 0;
}   // ~SyncOutput: `?2026l` + flush commits the frame

/**
 * @brief Signals a main-thread repaint when the terminal width changes.
 *
 * Runs as a background thread while readline waits at the confirmation
 * prompt. This watcher **never** touches the terminal or readline
 * directly: it only sets @ref ResizeRepaintState::pending, and the main
 * thread's @ref confirmResizeHook does the redraw **and** restores the
 * user's typed input via @c rl_redisplay().
 *
 * @details Reacts to width changes only. A height change is ignored: the
 * terminal's own reflow repositions the block, and the skip guard in the
 * hook handles the case where the block no longer fits.
 *
 * @note Must be stopped before @c readline returns (see @ref stop) so no
 * repaint can fire after the prompt has been answered.
 */
class ConfirmResizeWatcher {
public:
    /**
     * @brief Callback that prints the warning block at a given width.
     *
     * @param cols Terminal width in columns to lay the block out for.
     * @return Visible width of every line printed, in order.
     */
    using Painter = std::function<std::vector<size_t>(size_t cols)>;

    /**
     * @brief Populates @p state and starts the watcher thread.
     *
     * @param state       State shared with @ref confirmResizeHook.
     * @param paint       Callback that (re)prints the warning block for a
     *                    given width and returns its per-line widths.
     * @param drawnCols   Terminal width the block was last drawn at.
     * @param drawnWidths Per-line visible widths of the block currently on
     *                    screen, as returned by @p paint.
     * @param promptText  The confirmation prompt as plain text (no ANSI
     *                    escapes). Used only to measure its visible width.
     */
    ConfirmResizeWatcher(ResizeRepaintState& state, Painter paint,
                         size_t drawnCols, std::vector<size_t> drawnWidths,
                         const std::string& promptText)
        : state_(state) {
        state_.paint        = std::move(paint);
        state_.lineWidths   = std::move(drawnWidths);
        state_.promptWidth  = utf8Width(promptText);
        state_.lastCols.store(drawnCols, std::memory_order_relaxed);
        state_.pending.store(false, std::memory_order_relaxed);

        thread_ = std::thread([this] { loop(); });
    }

    /// Stops and joins the watcher thread. Safe to call more than once.
    ~ConfirmResizeWatcher() { stop(); }

    ConfirmResizeWatcher(const ConfirmResizeWatcher&)            = delete;
    ConfirmResizeWatcher& operator=(const ConfirmResizeWatcher&) = delete;

    /**
     * @brief Stops the watcher thread and waits for it to finish.
     *
     * Safe to call more than once.
     */
    void stop() {
        if (!thread_.joinable()) return;
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        thread_.join();
    }

    /**
     * @brief The state the watcher writes to. Pass to @ref confirmResizeHook
     *        when installing it as @c rl_event_hook (via the wrapper below).
     */
    ResizeRepaintState& state() { return state_; }

private:
    /**
     * @brief Poll loop: signals the main thread on a width change.
     *
     * Wakes every 100 ms (or immediately on @ref stop). Only width
     * changes set @ref ResizeRepaintState::pending; height changes are
     * ignored.
     */
    void loop() {
        constexpr auto kPoll = std::chrono::milliseconds(100);
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            cv_.wait_for(lk, kPoll, [&] { return stop_; });
            if (stop_) break;

            const auto size = terminalSize();
            const size_t cols = size.first;
            const size_t rows = size.second;
            if (cols == state_.lastCols.load(std::memory_order_relaxed))
                continue;
            state_.lastCols.store(cols, std::memory_order_relaxed);
            state_.pendingCols.store(cols, std::memory_order_relaxed);
            state_.pendingRows.store(rows, std::memory_order_relaxed);
            state_.pending.store(true, std::memory_order_release);
        }
    }

    ResizeRepaintState&     state_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    std::thread             thread_;
};

/**
 * @brief `rl_event_hook` trampoline for a specific @ref ResizeRepaintState.
 *
 * @c rl_event_hook is a plain function pointer (`int (*)(void)`), so it
 * cannot carry a state argument. This trampoline closes over
 * @ref g_confirmResize — the only state a hook is ever installed for in
 * this program. If a second modal prompt ever needs its own watcher, this
 * is the one place that has to change (e.g. a `std::function`-based
 * dispatcher).
 *
 * @return @ref confirmResizeHook's return value (always 0).
 */
static int confirmResizeHookTrampoline(void) {
    return confirmResizeHook(g_confirmResize);
}

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
 * @par Warning block and resize
 * ISO names in the warning are truncated to fit the current terminal width.
 * While the confirmation prompt is waiting, a resize re-lays out the block
 * (with truncation recomputed for the new width). The redraw runs on the
 * main thread via @c rl_event_hook — the watcher thread only signals — so
 * any half-typed answer survives the resize and the cursor stays where
 * the user left it. If the block cannot be redrawn without clamping at
 * row 1 (`up >= rows`), the redraw is skipped: the terminal keeps the
 * reflowed layout, the safety header stays visible, and the ISO / device
 * lists above the block are preserved.
 *
 * Detected devices are annotated with status indicators:
 * - @b [MOUNTED] — device or a partition is currently mounted; cannot be written to.
 * - @b [FLUSHING]   — device was part of a cancelled operation and is still draining
 *                  in the background; cannot be written to until draining finishes.
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
                std::cerr << color << " • " << err << "\n";
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

        // Collect the per-pair data once; a resize only re-renders it.
        struct WarnRow {
            std::string device, driveName, deviceSizeStr, filename, isoSizeStr;
        };
        std::vector<WarnRow> warnRows;
        warnRows.reserve(validPairs.size());
        for (const auto& [iso, device] : validPairs) {
            warnRows.push_back({ device,
                                 getDriveName(device),
                                 formatFileSize(getBlockDeviceSize(device)),
                                 iso.filename,
                                 iso.sizeStr });
        }

        auto printWarning = [&](size_t cols) -> std::vector<size_t> {
            const size_t usableCols = cols > 1 ? cols - 1 : 1;
            std::vector<size_t> widths;

            // Header is allowed to wrap naturally: clipping "ERASE ALL DATA"
            // would hide the safety message. repaint()'s up-count handles wrapping
            // via (w + cols - 1) / cols.
            std::cout << "\n" << wt.colorWarning << "WARNING: This will "
                      << UI::Palette::Red << "*ERASE ALL DATA*"
                      << wt.colorWarning << " on:" << wt.rl_resetCol << "\n\n";
            widths.push_back(0);                                                // blank line
            widths.push_back(utf8Width("WARNING: This will *ERASE ALL DATA* on:"));
            widths.push_back(0);                                                // blank line

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

                // Clip so an over-long row (truncateIsoName's MIN_STEM floor
                // on very narrow terminals) cannot wrap past usableCols, and record
                // the actual on-screen width so repaint()'s up-count stays accurate.
                const std::string clipped = clipToWidth(row.str(), usableCols);
                std::cout << clipped << "\n";
                widths.push_back(std::min(visibleWidth(clipped), usableCols));
            }

            std::cout << "\n";
            widths.push_back(0);                                                // blank line
            return widths;
        };

        const size_t drawnCols = terminalSize().first;
        std::vector<size_t> drawnWidths;
        {
            SyncOutput sync;                     // emits ?2026h
            drawnWidths = printWarning(drawnCols);
        }                                        // dtor: ?2026l + flush

        // Readline needs the \001/\002 markers around escape sequences so it
        // can measure the prompt; plain output must not have them.
        // No leading "\n": the blank line is already printed by printWarning.
        const std::string promptText = "Proceed? (y/n): ";
        const std::string confirmPrompt =
            "\001" + std::string(color) + "\002" + promptText + wt.rl_resetCol;

        std::unique_ptr<char, decltype(&std::free)> confirmation(nullptr, &std::free);
        {
            ConfirmationModeGuard confirmGuard;
            ConfirmResizeWatcher watcher(g_confirmResize, printWarning,
                                         drawnCols, drawnWidths, promptText);

            // Install the repaint hook only for this readline() call, and
            // save any hook the surrounding context had set.
            auto* savedHook = rl_event_hook;
            rl_event_hook = confirmResizeHookTrampoline;
            confirmation.reset(readline(confirmPrompt.c_str()));
            rl_event_hook = savedHook;

            watcher.stop();   // no repaint can fire after this returns
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

// ------------------------------------------------------------------------- //
// write2usbUI Block                                                        //
// ------------------------------------------------------------------------- //

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
 * @brief Detach each in-flight write task into its own background drain thread after cancellation.
 *
 * Called at the tail of performWriteOperation() when the user has interrupted the
 * operation via Ctrl+C (GlobalState::g_operationCancelled). Its job is to move the
 * still-running futures out of the local function scope into detached background
 * threads, so that any hardware tasks still flushing cache data to a device can
 * finish safely *after* control has been returned to the shell prompt.
 *
 * One detached thread is spawned per pending future. Each thread waits on its own
 * future and, as soon as it completes, erases that device from the global
 * g_drainingDevices set. Because every pending device has an independent waiter,
 * a device's "[FLUSHING]" indicator clears the moment *its* task finishes,
 * regardless of how long the other devices take — a slow device cannot hold
 * another device's indicator lit. When the last thread empties the set,
 * g_drainingCancelled is cleared so the indicator disappears entirely.
 *
 * Only devices whose tasks had not yet completed or failed at the moment of
 * cancellation are added to g_drainingDevices — tasks that had already returned
 * from writeIsoToDevice() are known to be flush-clean and are skipped.
 *
 * The non-copyable, non-movable progressData vector is deliberately *not* moved
 * or captured by value; the shared_ptr is captured instead so the per-operation
 * data stays alive for as long as the detached threads need it. The device path
 * for each pending future is read from progressData[i].device, so no separate
 * pair vector is required.
 *
 * @param futures        Pending per-task futures. Taken by value; each element is
 *                       moved into its own detached thread, so the caller's vector
 *                       is left empty after this call. Must not be used by the
 *                       caller afterwards.
 * @param progressData   Per-task progress records. Read only: .completed / .failed
 *                       are inspected to decide which devices still need draining,
 *                       and .device is used to enumerate the draining set.
 * @param progressDataPtr Shared ownership of the progress records; captured by each
 *                       detached thread to keep the operation data alive until that
 *                       device's drain completes.
 *
 * @note The caller must not touch @p futures after this returns — it has been
 *       moved from. This is intentional: the whole point is to transfer ownership
 *       of the pending work out of the local scope.
 *
 * @note @p futures and @p progressData must have the same size: drainDevices[i] is
 *       paired with futures[i], and each spawned thread indexes both. A mismatch is
 *       an out-of-bounds read.
 *
 * @note This may spawn multiple threads (one per pending future) for a single call.
 *       g_drainingManager must be able to accept more than one thread per operation.
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
    // local function context into safe, detached background engines.
    // This allows background hardware tasks to finish flushing cache data safely
    // without copying or moving the non-copyable 'progressData' structure.
    // One thread is spawned per pending future so each device's [FLUSHING] clears
    // independently as soon as that device's task finishes — a slow device cannot
    // hold another device's indicator lit.
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
            // Keep the per-operation progress data alive for any async task
            // that still holds references into it.
            (void)progressDataPtr;

            if (f.valid()) {
                f.wait();
            }

            std::lock_guard<std::mutex> lock(g_drainingDevicesMutex);
            g_drainingDevices.erase(dev); // no-op if never inserted

            if (g_drainingDevices.empty()) {
                g_drainingCancelled.store(false);
            }
        }));
    }
}

/**
 * @brief Owns the live progress screen and keeps it correctly laid out.
 *
 * The screen has two phases:
 * - @b Running  — a keeper thread redraws the per-task rows.
 * - @b Finished — after @ref finish() it shows the final rows, the status line,
 *   the summary and, once set with @ref setPrompt, the prompt. The keeper keeps
 *   running and repaints on terminal resize, so the screen stays correct while
 *   the program waits for input.
 *
 * @par Rendering
 * All drawing happens under one mutex (@c renderMutex_), from either the keeper
 * thread or @ref finish(), so frames never interleave. The keeper wakes on a task
 * completion (condition variable in the shared @c WriteSync state) or every
 * 100 ms to compare the terminal size with the last frame. While running it
 * redraws on completion, on a size change, or every 500 ms (live percentages and
 * speeds). Once finished it redraws only on a size change.
 *
 * @par Row layout
 * @code
 * filename → /dev/sdX <drive name> [▇▇▄░░] percentage speed
 * @endcode
 * Device path, drive name and speed are padded to the widest value across rows so
 * the bars line up. Drive names are always shown in full. The bar has 5 cells and
 * 10 steps of 10% (empty ░, half ▄, full ▇), floored from the percentage, so it is
 * full only at 100%. The status column shows the live percentage, or DONE (speed
 * shown as " (avg)"), FAIL or CXL (cancelled).
 *
 * @par Filename truncation
 * Names are shown in full unless the widest row exceeds the terminal width. Only
 * then is each name shortened to the space the terminal actually leaves for the
 * filename column (terminal width minus the rest of the row) as "stem….iso": the
 * ".iso" extension is always kept, the stem never shrinks below 4 columns, and
 * truncation is UTF-8 safe. On very narrow terminals the stem floor can make the
 * row slightly wider than the budget; the per-row clip below then prevents wrap.
 *
 * @par Redraw and result screen
 * At an unchanged size, a frame moves the cursor up by the previous frame's
 * physical line count and redraws. After a size change, and always for the result
 * screen, the screen and scrollback are wiped and redrawn from the top. Rows are
 * clipped to avoid wrapping, and each frame is written once inside a synchronized
 * update. The result screen looks like this:
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
 * Holds @c shared_ptr copies of the progress vector and the @c WriteSync state, so
 * it never refers to the stack of the function that created it. The destructor
 * stops and joins the keeper thread.
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
        renderLocked();   // wrapper overload: queries terminal, then dispatches
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

    static constexpr int  BAR_WIDTH = 5;                    // character cells
    static constexpr int  BAR_STEPS = BAR_WIDTH * 2;        // each cell has 2 states (half / full) -> 10 steps, 10% each
    static constexpr auto kPulse = std::chrono::milliseconds(500);   // live-value refresh while running
    static constexpr auto kPoll  = std::chrono::milliseconds(100);   // resize polling interval

    // ---- helpers ------------------------------------------------------------
    // utf8Width(), terminalSize() and truncateIsoName() live at file scope so
    // the confirmation warning (via ConfirmResizeWatcher) can share them.

    // Right-pads an ASCII string to the given byte width. Used for device
    // paths and other ASCII-only columns.
    static std::string padRight(std::string s, size_t target) {
        if (s.size() < target) s.append(target - s.size(), ' ');
        return s;
    }

    // ---- per-operation constants -------------------------------------------

    // Device names, full (untruncated) drive names and column widths cannot
    // change while the operation runs, so they are computed once, before the
    // keeper starts.
    void computeConstants() {
        std::unordered_map<std::string, std::string> deviceNames;
        for (const auto& prog : progressData_) {
            if (deviceNames.find(prog.device) == deviceNames.end())
                deviceNames[prog.device] = getDriveName(prog.device);
        }

        // Index i matches progressData_[i]. Drive names are stored verbatim,
        // never shortened.
        rowShortNames_.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            rowShortNames_.push_back(deviceNames[prog.device]);
            maxDeviceLen_ = std::max(maxDeviceLen_, prog.device.size());
            maxNameWidth_ = std::max(maxNameWidth_, utf8Width(rowShortNames_.back()));
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

    // ---- keeper thread -----------------------------------------------------

    void keeperLoop() {
        using clock = std::chrono::steady_clock;
        clock::time_point lastDraw{};                       // epoch: first pass draws at once
        size_t seenFinished = static_cast<size_t>(-1);      // differs from any real count: first pass draws

        while (!stop_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> rl(renderMutex_);

                // Query the terminal once per frame here and hand the result
                // to renderLocked(); the renderer no longer re-queries it.
                const auto   size        = terminalSize();
                const bool   sizeChanged = hasDrawn_ &&
                                           (size.first != lastCols_ || size.second != lastRows_);
                const size_t finishedNow = syncPtr_->finishedTasks.load();
                const bool   completion  = (finishedNow != seenFinished);
                seenFinished = finishedNow;   // updated in every phase so the wait below can't spin

                if (phase_ == Phase::Running) {
                    const bool pulse = (clock::now() - lastDraw) >= kPulse;
                    if (sizeChanged || completion || pulse) {
                        renderLocked(size, sizeChanged);
                        lastDraw = clock::now();
                    }
                } else if (sizeChanged) {
                    renderLocked(size, sizeChanged);   // result screen: repaint only when the size changed
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

    // ---- rendering ---------------------------------------------------------

    // Convenience overload: queries the terminal size and derives the
    // resize flag itself. Used by finish(), which renders once and does not
    // already hold this information.
    //
    // The caller must hold renderMutex_.
    void renderLocked() {
        const auto size    = terminalSize();
        const bool resized = hasDrawn_ &&
                             (size.first != lastCols_ || size.second != lastRows_);
        renderLocked(size, resized);
    }

    // Builds the whole frame in one string and writes it once (less flicker).
    // Terminal size and the resize flag are passed in so the renderer and the
    // keeper share exactly one source of truth for "did the size change?".
    //
    // The caller must hold renderMutex_.
    void renderLocked(std::pair<size_t, size_t> termSize, bool resized) {
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
            maxSpeedWidth = std::max(maxSpeedWidth, utf8Width(speedStrs.back()));
        }

        // Reserve room for " (avg)" only if some task is already showing it,
        // so names are truncated only when a row is really wider than the terminal.
        bool anyCompleted = false;
        for (const auto& prog : progressData_)
            if (prog.completed.load()) { anyCompleted = true; break; }
        const size_t avgExtra = anyCompleted ? 6 : 0;

        //   " → " (3) + device + " <name>" (maxNameWidth + 3) + " [" (2) + bar (5)
        //   + "] " (2) + status (5) + speed (+ " (avg)" once a task is done)
        // This is the width of everything except the filename column.
        const size_t fixedWidth = 3 + maxDeviceLen_ + (maxNameWidth_ + 3) + 2 + BAR_WIDTH
                                + 2 + 5 + maxSpeedWidth + avgExtra;

        const size_t cols       = termSize.first;
        const size_t rows       = termSize.second;
        const size_t usableCols = cols > 1 ? cols - 1 : 1;

        // Space the terminal leaves for the filename column. Filenames are
        // truncated to this budget, so the cut adapts to the actual terminal
        // size instead of a fixed width.
        const size_t allowedFilenameWidth =
            usableCols > fixedWidth ? usableCols - fixedWidth : 0;

        // Shortened filenames, computed once per frame. truncateIsoName() returns
        // the name unchanged when it already fits in allowedFilenameWidth, so no
        // explicit "should I truncate?" flag is needed.
        std::vector<std::string> shortFiles;
        shortFiles.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            shortFiles.push_back(truncateIsoName(prog.filename, allowedFilenameWidth));
            maxFilenameWidth = std::max(maxFilenameWidth, utf8Width(shortFiles.back()));
        }

        std::string frame;
        frame += "\033[?2026h";
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
            paddedFilename.append(maxFilenameWidth - utf8Width(fname), ' ');

            const std::string paddedDevice = padRight(prog.device, maxDeviceLen_);

            const std::string& name = rowShortNames_[i];
            const std::string namePad(maxNameWidth_ - utf8Width(name), ' ');

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
            const std::string speedPad(maxSpeedWidth - utf8Width(speedStr), ' ');

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

    // ---- state -------------------------------------------------------------

    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr_;
    std::vector<ProgressInfo>&                 progressData_;
    std::shared_ptr<WriteSync>                 syncPtr_;
    const WriteTheme                           wt_;
    const std::chrono::steady_clock::time_point startTime_;

    // Written once in computeConstants() before the keeper thread starts.
    // rowShortNames_ holds the full, never-truncated drive names.
    std::vector<std::string> rowShortNames_;
    size_t maxDeviceLen_ = 0;
    size_t maxNameWidth_ = 0;

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
 * @brief Executes validated ISO-to-device writes and manages their progress
 *        screen and completion or cancellation handling.
 *
 * Creates the per-task progress vector using @ref prepareWriteData, which
 * establishes the task ordering by natural, case-insensitive ISO filename
 * sort, with the device path as the tie-breaker. Task index and display-row
 * index therefore remain aligned.
 *
 * Submits one @ref writeIsoToDevice call per task to the global @ref ThreadPool.
 * A shared @ref WriteSync state coordinates task completion notifications
 * between the worker threads, the main thread and the @ref WriteScreen keeper.
 * The screen handles row rendering, column alignment, filename truncation,
 * terminal resizing and the final result layout.
 *
 * @par Normal completion
 * Waits until all tasks have finished, then disables further SIGINT handling
 * and paints the result screen. The returned screen remains alive so the
 * caller can display a prompt and retain resize-triggered repainting.
 * All valid futures are waited on before returning.
 *
 * @par Cancellation (Ctrl+C)
 * The cancellation flag is checked while waiting for task completion.
 * Because the signal handler cannot safely notify the condition variable,
 * the main thread rechecks cancellation at intervals of up to 500 ms while
 * waiting.
 *
 * When cancellation is detected, the result screen is painted and terminal
 * input state is restored. Any tasks still running are transferred to
 * @ref drainManagerForWrite2UsbUI, which keeps their futures alive while they
 * finish in the background. This allows outstanding writes and operating-system
 * page-cache flushing to continue. The drain manager removes each device from
 * @ref g_drainingDevices when its drain completes, clearing its [FLUSHING]
 * indicator.
 *
 * The result screen reflects task states at the time it is painted; tasks
 * that continue running after cancellation may finish later. The successful
 * count in the summary is the count recorded at that time.
 *
 * @par Lifetime and ownership
 * The progress vector and synchronization state are held by shared pointers
 * captured by worker tasks and retained by the screen. Cancellation transfers
 * the outstanding futures and the required shared state to the drain manager.
 * This prevents those objects from being destroyed while background tasks
 * still depend on them.
 *
 * The caller owns the returned @c shared_ptr<WriteScreen>. It should retain
 * the screen while waiting for user input and call @ref WriteScreen::stop
 * when resize-triggered repainting is no longer needed, or allow the screen
 * to be destroyed when its shared ownership ends.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs to write. The input
 *                     vector is not modified.
 *
 * @return A shared pointer to the live @ref WriteScreen, allowing the caller
 *         to retain the result screen while waiting for user input.
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
