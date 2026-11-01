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

// ------------------------------------------------------------------------- //
// Terminal / UTF-8 helpers (shared by WriteScreen and ConfirmResizeWatcher) //
// ------------------------------------------------------------------------- //

/**
 * @brief Visible columns of a UTF-8 string, counted by code point.
 *
 * Multi-byte UTF-8 sequences count as one column each, so a 3-byte
 * codepoint such as "…" contributes 1. Continuation bytes (top two bits
 * 10) are skipped.
 *
 * This is a code-point count, not Unicode display width: East-Asian wide
 * characters (CJK, emoji) are counted as 1 column here even though terminals
 * render them as 2. Correct for the ASCII and common Latin-1 filenames this
 * file handles.
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
 * Like utf8Width, but skips ANSI CSI sequences (ESC [ … final byte in
 * 0x40..0x7E), which the terminal interprets rather than renders. The
 * escapes count as 0 columns; every other UTF-8 code point counts as 1.
 *
 * @param s String that may contain colour/formatting escapes.
 * @return Visible columns @p s occupies when printed.
 */
static size_t visibleWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            if (i < s.size()) ++i;
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
 * toward the budget, so colours opened before the cut remain effective past
 * it (the caller handles any reset it needs). UTF-8 multi-byte sequences
 * are never split: an escape or code point that would cross the limit is
 * dropped whole.
 *
 * @param s       Input string, possibly with ANSI escapes and UTF-8.
 * @param maxCols Maximum visible columns to keep. 0 returns an empty string;
 *                escapes appearing before the first visible character are
 *                still emitted.
 * @return The longest prefix of @p s whose visible width is <= @p maxCols.
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
 * @brief Shortens an ISO filename to fit a column budget, keeping ".iso".
 *
 * If @p name already fits in @p maxCols columns it is returned unchanged.
 * Otherwise a trailing ".iso" extension (case-insensitive) is separated off,
 * the stem is cut to maxCols minus (width of "…" + ".iso") columns (floored
 * at 4), and the result is reconstructed as "stem….iso". Truncation walks
 * whole UTF-8 code points, so a multi-byte sequence is never split.
 *
 * @par Stem floor
 * MIN_STEM is 4 columns, so on very narrow terminals the returned string
 * can exceed @p maxCols by up to 5 columns (4 stem + 1 "…") when an
 * extension is present, or by 1 column when it is not. Callers should clip
 * the containing line with clipToWidth to prevent wrap.
 *
 * @param name    Filename to shorten (may be empty, with or without a ".iso"
 *                extension).
 * @param maxCols Column budget the result should fit within; 0 forces the
 *                stem floor and returns "….iso" or "…".
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

    constexpr size_t MIN_STEM = 4;
    const size_t overhead = 1 + utf8Width(ext);
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
    if (i >= stem.size()) return name;

    out += "\u2026";
    out += ext;
    return out;
}

// ------------------------------------------------------------------------- //
// Resize watcher (main-thread repaint, event-driven)                        //
// ------------------------------------------------------------------------- //

/**
 * @brief State used by the confirmation prompt's resize repaint.
 *
 * Everything here is main-thread-only. The SIGWINCH handler never touches
 * this struct: it only writes one byte to a self-pipe (see
 * confirm_resize_detail::g_fd). The read end of that pipe is stored in
 * @ref wakeRd and is waited on by confirmResizeGetc() together with stdin,
 * so the main thread sleeps in poll() until a key or a resize arrives.
 *
 * @par Ownership
 * Instances are owned by the caller, normally the single file-scope object
 * g_confirmResize. ConfirmResizeWatcher takes the state by reference, so a
 * watcher cannot exist without one.
 */
struct ResizeRepaintState {
    /// Width the block was last drawn at; used to ignore height-only resizes.
    size_t lastCols = 0;

    std::function<std::vector<size_t>(size_t)> paint;
    std::vector<size_t> lineWidths;
    size_t promptWidth = 0;

    /// Read end of the wake-up pipe (set by ConfirmResizeWatcher, -1 if none).
    int wakeRd = -1;
};

/// The one state used by the confirmation prompt (see ResizeRepaintState
/// for the "one watcher at a time" assumption).
static ResizeRepaintState g_confirmResize;

/**
 * @brief Repaints the warning block for the current terminal size.
 *
 * Runs on the main thread, from inside readline's character read, after a
 * SIGWINCH wake-up. It:
 * -# Measures the terminal itself. A failed query (zero cols or rows) and a
 *    height-only change (same width as the last drawn block) are ignored; the
 *    terminal's own reflow repositions the block, and the skip guard below
 *    handles the case where it no longer fits. The row arithmetic below
 *    divides by cols, so the zero guard stays.
 * -# Tells readline the new screen size via rl_set_screen_size() so its own
 *    line-wrapping agrees with the layout computed here.
 * -# Computes the wrapped height of the warning block plus the wrapped height
 *    of the prompt + typed buffer (rl_point gives the cursor's offset inside
 *    that buffer).
 * -# If that height does not fit on screen (>= rows), skips the repaint
 *    entirely, without emitting any terminal escape (so no dangling ?2026h
 *    is ever sent).
 * -# Otherwise, opens a DEC mode 2026 synchronized-output envelope, lets the
 *    painter redraw the whole screen at the new width (it homes the cursor
 *    and wipes the screen and scrollback itself), and calls rl_on_new_line()
 *    + rl_redisplay() so readline reprints the prompt and re-echoes whatever
 *    the user has typed, cursor position included, then closes the 2026
 *    envelope to commit the frame atomically.
 *
 * @par Why no cursor-up / erase here
 * An earlier version moved the cursor up and erased to the end of the screen
 * before calling the painter. The painter's own full-screen erase then made
 * VTE-based terminals push the half-erased screen into scrollback, leaving one
 * stale copy per resize. The painter alone owns the wipe now.
 *
 * The envelope is owned by an RAII guard (SyncOutput), so a throwing paint()
 * callback still emits the closing ?2026l. The envelope closes before this
 * function returns.
 *
 * Restoring the typed input is possible here because rl_line_buffer /
 * rl_end / rl_point are mutated by readline on the same thread.
 *
 * @param s Shared state populated by ConfirmResizeWatcher.
 */
static void confirmResizeRepaint(ResizeRepaintState& s) {
    const TermSize sz = terminalSize();
    const size_t cols = sz.cols;
    const size_t rows = sz.rows;

    if (cols == 0 || rows == 0) return;
    if (cols == s.lastCols) return;
    s.lastCols = cols;

    rl_set_screen_size(static_cast<int>(rows), static_cast<int>(cols));

    size_t warningRows = 0;
    for (size_t w : s.lineWidths)
        warningRows += std::max<size_t>(1, (w + cols - 1) / cols);

    const size_t cursorCols = s.promptWidth + rl_point;
    const size_t cursorLine = cursorCols / cols;

    const size_t up = warningRows + cursorLine;

    if (up >= rows) return;

    SyncOutput sync;

    // No cursor-up / erase-to-end here. The painter homes the cursor and wipes
    // the screen and scrollback itself. Erasing part of the screen first would
    // be harmful: terminals such as VTE push the screen into scrollback on a
    // full erase, so a half-erased copy would be left behind on every resize.
    s.lineWidths = s.paint(cols);

    std::cout.flush();

    rl_on_new_line();
    rl_redisplay();
}

/**
 * @brief rl_getc_function: blocks until a key or a resize arrives.
 *
 * Readline calls this whenever it needs the next input byte (and
 * rl_event_hook is null; ConfirmResizeWatcher nulls it while active). Instead
 * of waking on a timer, it sleeps in poll() on two descriptors:
 * - stdin: a byte is available, so the call is handed to readline's own
 *   rl_getc(), which keeps its EOF / EINTR / EAGAIN handling and returns the
 *   byte.
 * - the wake pipe: the SIGWINCH handler wrote to it. The pipe is drained,
 *   the warning block is repainted via confirmResizeRepaint(), and the wait
 *   resumes.
 *
 * A signal that interrupts poll() (EINTR) simply loops; because the handler's
 * byte stays in the pipe until it is read, a resize that lands between two
 * poll() calls is never lost. CPU use while idle is zero.
 *
 * This is a plain function pointer (int (*)(FILE*)) and so closes over
 * g_confirmResize, the only state a watcher is ever created for in this
 * program. If a second modal prompt ever needs its own watcher, this is the
 * one place that has to change.
 *
 * @param stream readline's input stream (stdin).
 * @return The next input byte, or EOF, as rl_getc() would.
 */
static int confirmResizeGetc(FILE* stream) {
    ResizeRepaintState& s = g_confirmResize;
    const int inFd = fileno(stream);

    for (;;) {
        struct pollfd fds[2] = {
            { inFd,     POLLIN, 0 },
            { s.wakeRd, POLLIN, 0 },
        };

        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return rl_getc(stream);          // let readline report the failure
        }

        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (::read(s.wakeRd, buf, sizeof buf) > 0) {}   // non-blocking drain
            confirmResizeRepaint(s);
        }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
            return rl_getc(stream);
    }
}

namespace confirm_resize_detail {

/// Write end of the self-pipe the SIGWINCH handler pokes. -1 = no watcher active.
inline std::atomic<int> g_fd{-1};

static_assert(std::atomic<int>::is_always_lock_free,
              "g_fd must be lock-free to be read from a signal handler");

/// Handler that was installed before ours; forwarded to so Readline's own
/// SIGWINCH handling (rl_catch_sigwinch) keeps working.
inline struct sigaction g_prev {};
inline bool             g_installed = false;

inline void onConfirmWinch(int sig, siginfo_t* info, void* ctx) {
    const int savedErrno = errno;
    const int fd = g_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char b = 1;
        [[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);   // async-signal-safe; non-blocking
    }
    errno = savedErrno;

    if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) return;
    if (g_prev.sa_flags & SA_SIGINFO) {
        if (g_prev.sa_sigaction) g_prev.sa_sigaction(sig, info, ctx);
    } else {
        g_prev.sa_handler(sig);
    }
}

} // namespace confirm_resize_detail

/**
 * @brief Repaints the confirmation screen on terminal resize, without polling.
 *
 * While alive it:
 * - creates a self-pipe and installs a SIGWINCH handler that writes one byte
 *   to it (async-signal-safe) and forwards to the previous handler;
 * - sets rl_event_hook to null and rl_getc_function to confirmResizeGetc, so
 *   readline blocks in poll() on stdin + the pipe instead of waking on its
 *   own timer (readline's idle loop re-runs rl_event_hook roughly ten times
 *   a second). The previous hook and getc function are restored by stop().
 *
 * The repaint itself runs on the main thread inside confirmResizeGetc(), the
 * only place rl_line_buffer / rl_end / rl_point can be read without a data
 * race, so the user's typed input survives via rl_redisplay().
 *
 * Reacts to width changes only. Height changes are ignored: the terminal's
 * own reflow repositions the block, and the skip guard in
 * confirmResizeRepaint() handles the case where the block no longer fits.
 *
 * If the pipe or handler cannot be set up, the watcher stays inactive: no
 * readline hooks are touched and resizes are simply not repainted (still no
 * polling).
 *
 * Only one watcher may be active at a time (handler and readline hooks are
 * process-global). Must be stopped before the prompt's readline() result is
 * used (see stop()) so no repaint can fire after the prompt is answered.
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
     * @brief Populates @p state, installs the SIGWINCH handler and the
     *        readline getc function.
     *
     * @param state       State shared with confirmResizeGetc. Must be
     *                    g_confirmResize and outlive this watcher.
     * @param paint       Callback that (re)prints the warning block for a
     *                    given width and returns its per-line widths.
     * @param drawnCols   Terminal width the block was last drawn at.
     * @param drawnWidths Per-line visible widths of the block currently on
     *                    screen, as returned by @p paint.
     * @param promptText  Confirmation prompt as plain text (no ANSI escapes).
     *                    Used only to measure its visible width.
     */
    ConfirmResizeWatcher(ResizeRepaintState& state, Painter paint,
                         size_t drawnCols, std::vector<size_t> drawnWidths,
                         const std::string& promptText)
        : state_(state) {
        using namespace confirm_resize_detail;
        assert(g_fd.load() < 0 && "only one ConfirmResizeWatcher may be active");

        state_.paint       = std::move(paint);
        state_.lineWidths  = std::move(drawnWidths);
        state_.promptWidth = utf8Width(promptText);
        state_.lastCols    = drawnCols;

        int fds[2];
        if (::pipe(fds) != 0) return;
        for (int fd : fds) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, O_NONBLOCK);   // handler must never block; drain must not block
        }
        wakeWr_       = fds[1];
        state_.wakeRd = fds[0];
        g_fd.store(wakeWr_, std::memory_order_release);

        struct sigaction sa {};
        sa.sa_sigaction = onConfirmWinch;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGWINCH, &sa, &g_prev) != 0) {
            closePipe();
            return;
        }
        g_installed = true;

        // The event hook must be null: while it is set, readline's idle loop
        // reads input itself and never calls rl_getc_function.
        savedEventHook_  = rl_event_hook;
        savedGetc_       = rl_getc_function;
        rl_event_hook    = nullptr;
        rl_getc_function = confirmResizeGetc;

        active_ = true;
    }

    /// Restores the hooks and handler. Safe to call more than once.
    ~ConfirmResizeWatcher() { stop(); }

    ConfirmResizeWatcher(const ConfirmResizeWatcher&)            = delete;
    ConfirmResizeWatcher& operator=(const ConfirmResizeWatcher&) = delete;

    /// Restores readline's hooks and the previous SIGWINCH handler, then
    /// closes the pipe, discarding any unprocessed resize event so no repaint
    /// fires after the prompt is answered. Safe to call more than once.
    void stop() {
        using namespace confirm_resize_detail;
        if (!active_) return;
        active_ = false;

        rl_getc_function = savedGetc_;
        rl_event_hook    = savedEventHook_;

        if (g_installed) {
            sigaction(SIGWINCH, &g_prev, nullptr);
            g_installed = false;
        }
        closePipe();
    }

    /// The state the watcher writes to.
    ResizeRepaintState& state() { return state_; }

private:
    void closePipe() {
        confirm_resize_detail::g_fd.store(-1, std::memory_order_release);
        if (wakeWr_ >= 0)        { ::close(wakeWr_);       wakeWr_ = -1; }
        if (state_.wakeRd >= 0)  { ::close(state_.wakeRd); state_.wakeRd = -1; }
    }

    ResizeRepaintState& state_;
    bool                active_ = false;
    int                 wakeWr_ = -1;
    rl_hook_func_t*     savedEventHook_ = nullptr;
    rl_getc_func_t*     savedGetc_      = nullptr;
};

/**
 * @brief Interactive loop that collects and validates ISO-to-device mappings
 * from the user.
 *
 * Shows the sorted list of selected ISOs (largest first) alongside detected
 * removable USB devices, then reads mapping input via readline. Repeats until
 * the user provides a valid, confirmed set of mappings or cancels with Esc
 * ("\x1b").
 *
 * Readline tab-completion is configured for both ISO indices and device
 * paths. "?" triggers the help screen. After a valid mapping is entered, a
 * destructive-write warning is shown and must be confirmed with y/Y.
 *
 * @par Typed input: reprint and pre-fill
 * The mapping line the user typed is kept in a std::string copy and used in
 * two ways:
 * - Reprint: every repaint that follows the prompt (the parse/validation
 *   error screens and the confirmation screen after a resize) re-emits the
 *   IDX>DEVICE prompt line together with the typed mapping, so the screen
 *   matches what the user saw before pressing Enter.
 * - Pre-fill: after a parse error, a validation error or a declined
 *   "Proceed? (y/n)", the same line is inserted into the next readline
 *   buffer (via a temporary rl_startup_hook chained to the hook installed by
 *   armPendingInputRestore()), so it can be edited instead of retyped. The
 *   empty-input and "?" paths start with a clean line.
 *
 * @par Device prompt and resize
 * The ISO/device screen is embedded in the mappings prompt itself, so
 * readline() is wrapped by readlineWithResize(): on a width change the
 * screen is cleared and the prompt's header text re-printed (see
 * promptHeaderForRepaint), then Readline redraws the last prompt line and
 * the typed input (including any pre-filled text). The list is not rebuilt,
 * and error messages from displayErrors() are not repeated. This watcher is
 * stopped before the confirmation prompt starts, which uses its own
 * ConfirmResizeWatcher.
 *
 * @par Warning block and resize
 * ISO names in the warning are truncated to fit the current terminal width,
 * recomputed on every repaint so the block re-lays out on a resize. The
 * confirmation screen is redrawn from scratch on SIGWINCH: the callback
 * installed on ConfirmResizeWatcher clears the whole screen with
 * "\033[H\033[2J\033[3J", reprints the ISO/device prompt header via
 * promptHeaderForRepaint(devicePrompt), then the IDX>DEVICE prompt line with
 * the typed mapping, then the warning block; ConfirmResizeWatcher redraws its
 * "Proceed? (y/n):" line afterwards. Because "\033[H" homes regardless of
 * the cursor position, the watcher's own in-place cursor-up math cannot
 * cause partial erases — the same scheme the rest of the app uses for
 * repaint-aware prompts, and the only one that works on terminals that do
 * not reflow the live cursor (VTE-based ones such as xfce4-terminal and
 * gnome-terminal). The initial draw uses clearFirst=false so the ISO/device
 * list already on screen from readlineWithResize is preserved; the warning
 * prints below it. Every resize uses clearFirst=true, so the whole
 * confirmation screen (device prompt, typed mapping, warning block, prompt
 * line) is reconstructed from a known top and no copies of the
 * "WARNING: This will *ERASE ALL DATA* on:" header accumulate over
 * successive resizes.
 *
 * A note on the initial draw versus the resize path: on the first pass,
 * readlineWithResize has already printed the device prompt and the typed
 * input, so only a blank separator line is added before the warning. On
 * resize, readline does not reprint its earlier prompt, so the resize
 * callback itself re-emits promptHeaderForRepaint(devicePrompt), the
 * IDX>DEVICE line with the typed mapping, and the same blank separator
 * (the clearFirst=true branch); both paths therefore produce the same
 * layout.
 *
 * Detected devices are annotated with status indicators:
 * - [MOUNTED]  — device or a partition is currently mounted; cannot be
 *                written to.
 * - [FLUSHING] — device was part of a cancelled operation and is still
 *                draining in the background; cannot be written to until
 *                draining finishes.
 *
 * @param selectedIsos ISOs chosen by the user for writing.
 * @return Validated (IsoInfo, device) pairs ready for
 *         performWriteOperation, or an empty vector if the user aborted or
 *         if the number of selected ISOs exceeds the thread pool size.
 */
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(const std::vector<IsoInfo>& selectedIsos) {
    PendingReadlineTextClearGuard pendingTextGuard;
    const WriteTheme wt = getWriteTheme();

    auto buildIdxPrompt = [&]() -> std::string {
        std::ostringstream s;
        s << "\n" << UI::Palette::RL_idxGreen << "IDX>DEVICE"
          << wt.rl_primaryCol << " ↵ for "
          << UI::Palette::RL_Yellow << "write2usb"
          << wt.rl_primaryCol << ", ? for help: "
          << wt.rl_resetCol;
        return s.str();
    };

    const bool noPermissions = (geteuid() != 0);

    // Hoisted so the last typed mapping survives a declined confirmation and
    // can be pre-filled into the next readline prompt. Stays empty on the
    // empty-input, "?", and parse/validation-error paths, so those start with
    // a clean line.
    std::string pendingInput;

    // State for the readline pre-fill hook. Function-local statics so the
    // captureless startup-hook lambda below (readline needs a plain function
    // pointer) can reach them without a separate named function.
    static std::string prefillText;
    static rl_hook_func_t* prevStartupHook = nullptr;

    while (true) {
        TerminalStateGuard termGuard;

        ThreadPool& pool = getStaticThreadPool();
        if (selectedIsos.size() > pool.threadCount()) {
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
            return {};
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

        devicePromptStream << buildIdxPrompt();

        std::string devicePrompt = devicePromptStream.str();

        const auto repaintDevices = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << promptHeaderForRepaint(devicePrompt);
        };

        armPendingInputRestore();

        // Pre-fill the previous mapping (only set after a declined confirmation)
        // into the readline buffer. Installed AFTER armPendingInputRestore() and
        // chained to its hook, so both keep working.
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

        if (!deviceInput) {
            return {};
        }

        if (deviceInput.get()[0] == '\0') {
            continue;
        }

        // Copy now; this std::string is used for the error/confirmation
        // repaints and handed back to the next prompt as pre-fill.
        std::string mainInputString(deviceInput.get());
        if (mainInputString == "\x1b") {
            return {};
        }

        if (mainInputString == "?") {
            helpMappings();
            continue;
        }

        if (deviceInput && *deviceInput) add_history(deviceInput.get());

        // Single renderer for both parse and validation errors. promptHeaderForRepaint
        // only emits the header (no prompt line), so we re-emit buildIdxPrompt()
        // plus the typed input to match the pre-error screen. buildIdxPrompt()
        // starts with "\n", which is exactly the blank line between the device
        // list and the prompt, so it is kept as-is.
        // Nothing is printed after this: pressEnterTo*WithRepaint appends its
        // own "↵ to try again..." / "Press Enter..." line as the final line.
        auto renderErrors = [&](const std::vector<std::string>& errs) {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << promptHeaderForRepaint(devicePrompt);
            std::cout << buildIdxPrompt() << mainInputString << "\n";
            std::cerr << "\n" << wt.rl_errorCol << "Errors:" << wt.rl_resetCol << "\n";
            for (const auto& e : errs) {
                std::cerr << color << " • " << e << "\n";
            }
        };

        std::vector<std::string> errors;
        std::vector<std::string> isoFilenames;
        for (const auto& iso : sortedIsos) {
            isoFilenames.push_back(iso.path);
        }

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
            // No errors reported but nothing validated — shouldn't happen;
            // avoid looping without repainting anything new.
            pendingInput = mainInputString;
            continue;
        }

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

        // Full redraw used by ConfirmResizeWatcher on SIGWINCH. On a resize,
        // BOTH the ISO/device list (drawn by readlineWithResize above) and
        // the warning block must be redrawn — readline does not reprint its
        // earlier prompt after we clear the screen, so the ISO/device list
        // must come from here. \033[H\033[2J homes regardless of cursor
        // position, which neutralizes ConfirmResizeWatcher's own cursor-up
        // math (drawnWidths goes stale after a reflow on terminals that
        // don't reflow the live cursor, e.g. VTE).
        //
        // On repaint we re-emit the device prompt header, then the IDX>DEVICE
        // prompt line plus the typed mapping (same as renderErrors), so the
        // user still sees what they entered. Layout:
        //
        //   <device prompt header lines>
        //   IDX>DEVICE ... ? for help: <typed input>
        //                                         <- blank line
        //   WARNING: This will *ERASE ALL DATA* on:
        //                                         <- blank line
        //    {...} ← {...}
        //                                         <- blank line
        //   Proceed? (y/n):
        auto renderConfirm = [&](size_t cols, bool clearFirst) -> std::vector<size_t> {
            const size_t usableCols = cols > 1 ? cols - 1 : 1;
            std::vector<size_t> widths;

            if (clearFirst) {
                // Home, clear screen, clear scrollback. The \033[3J matters:
                // terminals such as VTE push the screen into scrollback on
                // \033[2J, so without it every resize leaves a stale copy of
                // the confirmation screen behind (same sequence WriteScreen uses).
                std::cout << "\033[H\033[2J\033[3J";
                std::cout << promptHeaderForRepaint(devicePrompt);
                std::cout << buildIdxPrompt() << mainInputString << "\n";
                std::cout << "\n";   // separator between device prompt and warning
            } else {
                // Initial draw: device prompt is already on screen; add the
                // same extra blank line so the layout matches the repaint path.
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
        };

        const size_t drawnCols = terminalSize().cols;
        std::vector<size_t> drawnWidths;
        {
            SyncOutput sync;
            // Initial draw: the device prompt is already on screen from
            // readlineWithResize, so we must NOT clear. Print only the warning
            // block beneath it.
            drawnWidths = renderConfirm(drawnCols, /*clearFirst=*/false);
        }

        const std::string promptText = "Proceed? (y/n): ";
        const std::string confirmPrompt =
            "\001" + std::string(color) + "\002" + promptText + wt.rl_resetCol;

        std::unique_ptr<char, decltype(&std::free)> confirmation(nullptr, &std::free);
        {
            ConfirmationModeGuard confirmGuard;
            // The watcher's repaint always clears first and redraws both the
            // device prompt header and the warning block. ConfirmResizeWatcher's
            // own cursor-up math (based on drawnWidths) is bypassed because
            // \033[H\033[2J homes regardless of cursor position.
            // The watcher installs and restores readline's hooks itself, so
            // readline() is called plainly while it is alive.
            ConfirmResizeWatcher watcher(g_confirmResize,
                                         [&](size_t cols) { return renderConfirm(cols, /*clearFirst=*/true); },
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
            return validPairs;
        } else {
            rl_bind_keyseq("?", help_handler);
        }

        // Declined (or empty/EOF): bring the mapping back so it can be tweaked
        // instead of retyped.
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

namespace write_screen_detail {

/// Write end of the self-pipe the SIGWINCH handler pokes. -1 = no watcher active.
inline std::atomic<int> g_fd{-1};

/// Handler that was installed before ours; forwarded to so Readline's own
/// SIGWINCH handling (rl_catch_sigwinch) keeps working.
inline struct sigaction g_prev {};
inline bool             g_installed = false;

static_assert(std::atomic<int>::is_always_lock_free,
              "g_fd must be lock-free to be read from a signal handler");

inline void onWriteWinch(int sig, siginfo_t* info, void* ctx) {
    const int savedErrno = errno;
    const int fd = g_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char b = 'w';
        [[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);   // async-signal-safe; non-blocking
    }
    errno = savedErrno;

    if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) return;
    if (g_prev.sa_flags & SA_SIGINFO) {
        if (g_prev.sa_sigaction) g_prev.sa_sigaction(sig, info, ctx);
    } else {
        g_prev.sa_handler(sig);
    }
}

/// SIGINT handler that was installed before ours (the program's cancellation
/// handler, which sets g_operationCancelled). Always called first.
inline struct sigaction g_prevInt {};
inline bool             g_intInstalled = false;

/// Chained SIGINT handler. Forwards to the cancellation handler FIRST, so
/// g_operationCancelled is already set when the relay thread wakes the
/// waiters, then pokes the same self-pipe as SIGWINCH (byte 'c').
inline void onWriteInt(int sig, siginfo_t* info, void* ctx) {
    const int savedErrno = errno;

    if (g_prevInt.sa_flags & SA_SIGINFO) {
        if (g_prevInt.sa_sigaction) g_prevInt.sa_sigaction(sig, info, ctx);
    } else if (g_prevInt.sa_handler != SIG_DFL && g_prevInt.sa_handler != SIG_IGN) {
        g_prevInt.sa_handler(sig);
    }

    const int fd = g_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char b = 'c';
        [[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);   // async-signal-safe; non-blocking
    }
    errno = savedErrno;
}

} // namespace write_screen_detail

/**
 * @brief Owns the live progress screen and keeps it correctly laid out.
 *
 * The screen has two phases:
 * - Running  — a keeper thread redraws the per-task rows.
 * - Finished — after finish() it shows the final rows, the status line and
 *   the summary. The keeper keeps running and repaints on terminal resize,
 *   unless a prompt was registered with setPrompt(): then the program's
 *   resize hook owns repainting (it calls repaint() and lets Readline redraw
 *   the prompt and typed input), so the keeper stays out of the way.
 *
 * @par Rendering
 * All drawing happens under one mutex (renderMutex_), from either the keeper
 * thread, finish() or repaint(), so frames never interleave.
 *
 * @par Waking the keeper (no polling)
 * The keeper blocks on the condition variable in the shared WriteSync state
 * and wakes only for:
 * - a task completion,
 * - a terminal resize (SIGWINCH),
 * - Ctrl+C (SIGINT), which also wakes the thread waiting in
 *   performWriteOperation(), and
 * - while running, the 500 ms pulse deadline (live percentages and speeds).
 * Once finished it sleeps indefinitely until a resize or stop().
 *
 * A signal handler cannot notify a condition variable, so SIGWINCH and SIGINT
 * are relayed: each handler writes one byte to a self-pipe (async-signal-safe)
 * and a small relay thread blocked in read() on that pipe notifies the
 * condition variable (and, for SIGWINCH, sets resizeSignalled_). The relay
 * thread consumes no CPU while idle.
 *
 * The SIGWINCH handler forwards to the previously installed handler. The
 * SIGINT handler is chained in front of the program's cancellation handler:
 * it calls that handler first (so g_operationCancelled is set) and only then
 * pokes the pipe. It is installed only if a real handler is present, and is
 * restored by stop() only if it is still the active one (performWriteOperation
 * sets SIGINT to SIG_IGN after the wait, which must not be overwritten).
 *
 * If the pipe or handler cannot be set up, the keeper falls back to checking
 * the terminal size every 100 ms, and cancelWakeable() reports false so the
 * cancellation wait falls back to a 500 ms recheck.
 *
 * Only one WriteScreen may be started at a time (the handler state is
 * process-global).
 *
 * @par Row layout
 * @code
 * filename → /dev/sdX <drive name> [▇▇▄░░] percentage speed
 * @endcode
 * Device path, drive name and speed are padded to the widest value across
 * rows so the bars line up. Drive names are always shown in full. The bar
 * has 5 cells and 10 steps of 10% (empty ░, half ▄, full ▇), floored from
 * the percentage, so it is full only at 100%. The status column shows the
 * live percentage, or DONE (speed shown as " (avg)"), FAIL or CXL
 * (cancelled).
 *
 * @par Filename truncation
 * Names are shown in full unless the widest row exceeds the terminal width.
 * Only then is each name shortened to the space the terminal actually leaves
 * for the filename column (terminal width minus the rest of the row) as
 * "stem….iso": the ".iso" extension is always kept, the stem never shrinks
 * below 4 columns, and truncation is UTF-8 safe. On very narrow terminals
 * the stem floor can make the row slightly wider than the budget; the
 * per-row clip then prevents wrap.
 *
 * @par Redraw and result screen
 * At an unchanged size, a frame moves the cursor up by the previous frame's
 * physical line count and redraws. After a size change, and always for the
 * result screen, the screen and scrollback are wiped and redrawn from the
 * top. Rows are clipped to avoid wrapping, and each frame is written once
 * inside a synchronized update. Frames are skipped while the terminal size
 * is unknown (0 columns or rows). The result screen looks like this:
 * @code
 * (line 1: empty)
 * Status: write2usb → COMPLETED        (line 2)
 * (line 3: empty)
 * <rows...>                            (from line 4)
 * (empty)
 * Successful: n/m | Time Elapsed: x.xs
 * @endcode
 * The prompt itself is not printed by this class: after repaint() the
 * program's resize hook calls rl_redisplay(), which redraws the prompt and
 * the user's typed input.
 *
 * @par Lifetime
 * Holds shared_ptr copies of the progress vector and the WriteSync state, so
 * it never refers to the stack of the function that created it. The
 * destructor stops and joins the keeper and relay threads and restores the
 * previous SIGWINCH handler.
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
    /// data, installs the resize signal relay and starts the keeper thread.
    void start() {
        {
            std::lock_guard<std::mutex> rl(renderMutex_);
            std::cout << headerText(false) << "\033[?25l" << std::flush;
        }
        computeConstants();
        installResizeSignal();
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

    /// Registers the program's "press Enter" prompt. Once set, the keeper no
    /// longer repaints on resize by itself: the program's resize hook calls
    /// repaint() instead and Readline redraws the prompt and typed input.
    void setPrompt(std::string prompt) {
        std::lock_guard<std::mutex> rl(renderMutex_);
        promptText_ = std::move(prompt);
    }

    /// True if Ctrl+C is relayed into the shared condition variable, i.e. a
    /// waiter on that condition variable is woken immediately on cancellation
    /// and needs no timed recheck. Valid after start().
    bool cancelWakeable() const { return cancelChained_; }

    /// Re-emits the result screen (summary) at the current terminal width.
    /// Called by the pressEnterTo*WithRepaint() machinery on resize so the
    /// summary survives a terminal wipe. Safe to call from the main thread
    /// after finish(); takes the same lock the keeper uses.
    void repaint() {
        std::lock_guard<std::mutex> rl(renderMutex_);
        renderLocked();
    }

    /// Stops and joins the keeper and relay threads and restores the previous
    /// SIGWINCH handler. Safe to call more than once.
    void stop() {
        if (!thread_.joinable()) return;
        stop_.store(true, std::memory_order_release);

        if (wakeWr_ >= 0) {                         // wake the relay thread out of read()
            const char b = 1;
            [[maybe_unused]] const ssize_t r = ::write(wakeWr_, &b, 1);
        }
        if (relay_.joinable()) relay_.join();

        { std::lock_guard<std::mutex> lk(syncPtr_->mutex); }
        syncPtr_->cv.notify_all();
        thread_.join();

        uninstallResizeSignal();
    }

private:
    enum class Phase { Running, Finished };

    static constexpr int  BAR_WIDTH = 5;
    static constexpr int  BAR_STEPS = BAR_WIDTH * 2;
    static constexpr auto kPulse = std::chrono::milliseconds(500);
    static constexpr auto kPoll  = std::chrono::milliseconds(100);   // fallback only

    // Right-pads an ASCII string to the given byte width. Used for device
    // paths and other ASCII-only columns.
    static std::string padRight(std::string s, size_t target) {
        if (s.size() < target) s.append(target - s.size(), ' ');
        return s;
    }

    // ---- Resize signal relay -------------------------------------------------

    void installResizeSignal() {
        using namespace write_screen_detail;
        assert(g_fd.load() < 0 && "only one WriteScreen may be started at a time");

        int fds[2];
        if (::pipe(fds) != 0) { pollFallback_ = true; return; }
        for (int fd : fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFL, O_NONBLOCK);       // handler must never block
        wakeRd_ = fds[0];
        wakeWr_ = fds[1];

        g_fd.store(wakeWr_, std::memory_order_release);

        struct sigaction sa {};
        sa.sa_sigaction = onWriteWinch;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGWINCH, &sa, &g_prev) != 0) {
            g_fd.store(-1, std::memory_order_release);
            ::close(wakeRd_); ::close(wakeWr_);
            wakeRd_ = wakeWr_ = -1;
            pollFallback_ = true;
            return;
        }
        g_installed = true;

        relay_ = std::thread([this] { relayLoop(); });

        installCancelSignal();
    }

    // Chains SIGINT in front of the program's cancellation handler so Ctrl+C
    // wakes cv waiters immediately. Skipped when no real handler is installed
    // (nothing would set g_operationCancelled anyway). The previous handler's
    // SA_RESTART bit and mask are kept so its behaviour towards blocking
    // syscalls in the worker threads does not change.
    void installCancelSignal() {
        using namespace write_screen_detail;

        struct sigaction cur {};
        if (sigaction(SIGINT, nullptr, &cur) != 0) return;
        if (!(cur.sa_flags & SA_SIGINFO) &&
            (cur.sa_handler == SIG_DFL || cur.sa_handler == SIG_IGN)) return;

        g_prevInt = cur;

        struct sigaction sa {};
        sa.sa_sigaction = onWriteInt;
        sa.sa_flags     = SA_SIGINFO | (cur.sa_flags & SA_RESTART);
        sa.sa_mask      = cur.sa_mask;
        if (sigaction(SIGINT, &sa, nullptr) != 0) return;

        g_intInstalled = true;
        cancelChained_ = true;
    }

    void uninstallResizeSignal() {
        using namespace write_screen_detail;
        if (g_intInstalled) {
            // Restore only if our handler is still the active one; after
            // performWriteOperation's signal(SIGINT, SIG_IGN) it is not, and
            // the ignore must stay.
            struct sigaction cur {};
            if (sigaction(SIGINT, nullptr, &cur) == 0 &&
                (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == onWriteInt)
                sigaction(SIGINT, &g_prevInt, nullptr);
            g_intInstalled = false;
            cancelChained_ = false;
        }
        if (g_installed) {
            sigaction(SIGWINCH, &g_prev, nullptr);
            g_installed = false;
        }
        if (wakeWr_ >= 0) {
            g_fd.store(-1, std::memory_order_release);
            ::close(wakeWr_);
            wakeWr_ = -1;
        }
        if (wakeRd_ >= 0) { ::close(wakeRd_); wakeRd_ = -1; }
    }

    // Blocks in read() until the handler (or stop()) writes a byte. Zero CPU
    // while idle. Turns the signal into a condition-variable wakeup.
    void relayLoop() {
        char buf[64];
        for (;;) {
            const ssize_t n = ::read(wakeRd_, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || stop_.load(std::memory_order_acquire)) break;

            // 'w' = SIGWINCH, 'c' = SIGINT. Either way the waiters are
            // notified below; only a resize sets the resize flag.
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == 'w') {
                    resizeSignalled_.store(true, std::memory_order_release);
                    break;
                }
            }
            { std::lock_guard<std::mutex> lk(syncPtr_->mutex); }   // pairs with the keeper's predicate check
            syncPtr_->cv.notify_all();
        }
    }

    // ---- Layout --------------------------------------------------------------

    // Device names, full (untruncated) drive names and column widths cannot
    // change while the operation runs, so they are computed once, before the
    // keeper starts.
    void computeConstants() {
        std::unordered_map<std::string, std::string> deviceNames;
        for (const auto& prog : progressData_) {
            if (deviceNames.find(prog.device) == deviceNames.end())
                deviceNames[prog.device] = getDriveName(prog.device);
        }

        rowShortNames_.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            rowShortNames_.push_back(deviceNames[prog.device]);
            maxDeviceLen_ = std::max(maxDeviceLen_, prog.device.size());
            maxNameWidth_ = std::max(maxNameWidth_, utf8Width(rowShortNames_.back()));
        }
    }

    // Header text: reprinted after every full-screen wipe. While running it is
    // the "Processing..." banner; once finished, line 2 holds the status line.
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

    void keeperLoop() {
        using clock = std::chrono::steady_clock;
        clock::time_point lastDraw{};
        size_t seenFinished = static_cast<size_t>(-1);

        while (!stop_.load(std::memory_order_acquire)) {
            bool finishedPhase = false;
            {
                std::lock_guard<std::mutex> rl(renderMutex_);

                const bool   resizeEvt   = resizeSignalled_.exchange(false, std::memory_order_acquire);
                const size_t finishedNow = syncPtr_->finishedTasks.load();
                const bool   completion  = (finishedNow != seenFinished);
                seenFinished = finishedNow;
                finishedPhase = (phase_ == Phase::Finished);

                if (!finishedPhase) {
                    const auto size = terminalSize();
                    if (size.cols == 0 || size.rows == 0) {
                        lastDraw = clock::now();            // size unknown: skip, don't spin
                    } else {
                        const bool sizeChanged = hasDrawn_ &&
                            (size.cols != lastCols_ || size.rows != lastRows_);
                        const bool pulse = (clock::now() - lastDraw) >= kPulse;
                        if (sizeChanged || completion || pulse) {
                            renderLocked(size, sizeChanged);
                            lastDraw = clock::now();
                        }
                    }
                } else if ((resizeEvt || pollFallback_) && promptText_.empty()) {
                    // Finished and no prompt registered: the keeper owns resize
                    // repaints. With a prompt, the resize hook calls repaint().
                    const auto size = terminalSize();
                    if (size.cols != 0 && size.rows != 0 &&
                        hasDrawn_ && (size.cols != lastCols_ || size.rows != lastRows_))
                        renderLocked(size, true);
                }
            }

            std::unique_lock<std::mutex> lk(syncPtr_->mutex);
            const auto pred = [&] {
                return stop_.load(std::memory_order_acquire) ||
                       resizeSignalled_.load(std::memory_order_acquire) ||
                       syncPtr_->finishedTasks.load() != seenFinished;
            };

            // Running: wake for the pulse. Finished: sleep until an event.
            // Fallback (no signal relay): also wake every kPoll to check size.
            if (!finishedPhase || pollFallback_) {
                auto deadline = finishedPhase ? clock::now() + kPoll : lastDraw + kPulse;
                if (pollFallback_) deadline = std::min(deadline, clock::now() + kPoll);
                syncPtr_->cv.wait_until(lk, deadline, pred);
            } else {
                syncPtr_->cv.wait(lk, pred);
            }
        }
    }

    // Convenience overload: queries the terminal size and derives the resize
    // flag itself. Used by finish() and repaint(), which render once and do
    // not already hold this information. The caller must hold renderMutex_.
    void renderLocked() {
        const auto size = terminalSize();
        if (size.cols == 0 || size.rows == 0) return;       // size unknown: skip
        const bool resized = hasDrawn_ &&
            (size.cols != lastCols_ || size.rows != lastRows_);
        renderLocked(size, resized);
    }

    // Builds the whole frame in one string and writes it once (less flicker).
    // Terminal size and the resize flag are passed in so the renderer and the
    // keeper share exactly one source of truth for "did the size change?".
    // The caller must hold renderMutex_.
    void renderLocked(TermSize termSize, bool resized) {
        if (termSize.cols == 0 || termSize.rows == 0) return;   // also guards the division below

        const bool finished = (phase_ == Phase::Finished);

        size_t maxFilenameWidth = 0;
        size_t maxSpeedWidth    = 0;

        std::vector<std::string> speedStrs;
        speedStrs.reserve(progressData_.size());
        for (const auto& prog : progressData_) {
            speedStrs.push_back(formatSpeed(prog.speed));
            maxSpeedWidth = std::max(maxSpeedWidth, utf8Width(speedStrs.back()));
        }

        bool anyCompleted = false;
        for (const auto& prog : progressData_)
            if (prog.completed.load()) { anyCompleted = true; break; }
        const size_t avgExtra = anyCompleted ? 6 : 0;

        const size_t fixedWidth = 3 + maxDeviceLen_ + (maxNameWidth_ + 3) + 2 + BAR_WIDTH
                                + 2 + 5 + maxSpeedWidth + avgExtra;

        const size_t cols = termSize.cols;
        const size_t rows = termSize.rows;
        const size_t usableCols = cols > 1 ? cols - 1 : 1;

        const size_t allowedFilenameWidth =
            usableCols > fixedWidth ? usableCols - fixedWidth : 0;

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
            // running screen is too: after a resize the terminal may have
            // reflowed or scrolled old content into scrollback, and cursor-up
            // cannot move above the visible screen.
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
            // Each cell is empty (░), half (▄) or full (▇). Both filled glyphs
            // are bottom-aligned and stop short of the cell top, so bars on
            // consecutive rows never touch.
            int pct = std::clamp(prog.progress.load(), 0, 100);
            int steps = (pct * BAR_STEPS) / 100;
            int full  = steps / 2;
            bool half = (steps % 2) != 0;

            std::string bar, empty;
            for (int j = 0; j < full; ++j) bar += "▇";
            if (half)                      bar += "▄";
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
            frame += "\033[?25h";            // show the cursor again
        }
        frame += "\033[?2026l";              // end synchronized update
        std::cout << frame << std::flush;
    }

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
    std::string         promptText_;     // non-empty = the resize hook owns repainting
    // Visible width (in columns) of every row printed by the previous frame.
    // Used to work out how far to move the cursor back up, even if the
    // terminal was resized (and therefore re-wrapped the old rows) in between.
    std::vector<size_t> lastRowWidths_;
    // Terminal size at the time of the previous frame. A change in either
    // dimension triggers a full wipe + repaint instead of a cursor-up redraw.
    size_t              lastCols_ = 0, lastRows_ = 0;
    bool                hasDrawn_ = false;

    // Resize relay. wakeRd_/wakeWr_/pollFallback_ are set in start() before
    // the threads run and only cleared after they are joined.
    int                 wakeRd_ = -1, wakeWr_ = -1;
    bool                pollFallback_ = false;
    bool                cancelChained_ = false;   // SIGINT relayed into the cv
    std::atomic<bool>   resizeSignalled_{false};
    std::thread         relay_;

    std::atomic<bool>   stop_{false};
    std::thread         thread_;
};

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
 * screen while waiting for user input and call WriteScreen::stop when
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

        if (screen->cancelWakeable()) {
            progressCv.wait(lock, done);
        } else {
            while (!done())
                progressCv.wait_for(lock, std::chrono::milliseconds(500), done);
        }
    }

    signal(SIGINT, SIG_IGN);

    screen->finish();

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
 * 4. Executes the flashing process via performWriteOperation, which returns
 *    the live WriteScreen.
 * 5. Keeps that screen alive while waiting for the user to press Enter, so
 *    the result screen still re-truncates and re-wraps when the terminal is
 *    resized, then stops it.
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

    auto screen = performWriteOperation(validPairs);

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    screen->setPrompt(std::string(color) + "\n↵ to continue...");

    pressEnterToContinueWithRepaint([&] { screen->repaint(); });

    screen->stop();
}
