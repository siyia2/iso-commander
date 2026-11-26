// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import isocmd.core.uiState;
import isocmd.ui.display.themes;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.drainingState;

namespace fs = std::filesystem;

// --------------------------------------------------------------------------- //
// namespace WriteValidation                                                   //
// --------------------------------------------------------------------------- //

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
	 * @param paths Filesystem roots for the in-flight counters.
	 * @return true if one or more write requests are still pending.
	 */
	bool isDeviceBusy(const std::string& device, const DeviceDiscoveryPaths& paths) {

		const std::string name = std::filesystem::path(device).filename().string();
		std::ifstream in(paths.sysBlock / name / "inflight");
		unsigned long reads = 0, writes = 0;
		if (!(in >> reads >> writes))
		    return false;
		return writes > 0;

	}

	std::string_view deviceStatusLabel(const std::string& device, const DeviceDiscoveryPaths& paths) {

		if (isDeviceMounted(device, paths)) return "[MOUNTED]";
		if (g_drainingManager.isDeviceDraining(device) || isDeviceBusy(device, paths)) return "[BUSY]";
		return {};

	}

	/**
	 * @brief Runs the device-level checks, independent of the paired ISO.
	 *
	 * Checks, in order:
	 * -# The device is a removable USB flash drive (isFlashDrive).
	 * -# The device is not reserved by the drain manager, which tracks writes still
	 *    being drained by this process after cancellation.
	 * -# The device and its partitions are not mounted (isDeviceMounted).
	 * -# The device has no write I/O in flight (interactive and non-interactive modes)
	 *    (@ref isDeviceBusy). This also catches pending writes from another
	 *    process, which the in-process set cannot track.
	 * -# The device size can be determined (getBlockDeviceSize).
	 *
	 * @param wt     Active write theme.
	 * @param device Path used for the checks (canonical /dev/sdX).
	 * @param shown  Path displayed in error messages (what the user typed).
	 * @param[out] size Device size in bytes; only valid when no error is returned.
	 * @param paths Filesystem roots for device discovery and validation.
	 * @return An error message for the first failed check, or std::nullopt if the
	 *         device is ready for writing.
	 */
	std::optional<std::string> checkDevice(const WriteTheme& wt, const std::string& device,
	                                       const std::string& shown, std::uint64_t& size,
	                                       const DeviceDiscoveryPaths& paths) {

		if (!isFlashDrive(device, paths))
		    return pathErr(wt, shown, " is not a removable USB flash device");

		if (g_drainingManager.isDeviceDraining(device))
		    return pathErr(wt, shown, " is still draining a cancelled write");

		if (isDeviceMounted(device, paths))
		    return pathErr(wt, shown, " or its partitions are mounted");

		if (isDeviceBusy(device, paths))
		    return pathErr(wt, shown, " has pending kernel-level tasks");

		size = getBlockDeviceSize(device, paths);
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
	                                         const std::string& device, std::uint64_t deviceSize) {

		if (iso.size <= deviceSize)
		    return std::nullopt;

		const std::string deviceSizeStr = formatFileSize(deviceSize);
		const std::string driveName = getDriveName(device);

		constexpr auto isoLabelPadded   = "    ISO   = ";
		constexpr auto driveLabelPadded = "    Drive = ";

		std::string e;
		e.append(wt.errLabel).append("ISO exceeds target drive capacity:\n")
		 .append(wt.errLabel).append(isoLabelPadded)
		 .append(wt.sizeCol).append(iso.sizeStr).append(" ").append(wt.rl_resetCol)
		 .append(UI::Palette::DimGray).append("(").append(wt.fileCol).append(iso.filename)
		 .append(UI::Palette::DimGray).append(")\n")
		 .append(wt.errLabel).append(driveLabelPadded)
		 .append(wt.sizeCol).append(deviceSizeStr).append(" ").append(wt.rl_resetCol)
		 .append(UI::Palette::DimGray).append("(").append(wt.warnLabel).append(device)
		 .append(wt.infoLabel).append(" <").append(driveName).append(">")
		 .append(UI::Palette::DimGray).append(")")
		 .append(wt.rl_resetCol);

		return e;

	}

} // namespace WriteValidation

// --------------------------------------------------------------------------- //
// Validation and mapping parser (public API)                                  //
// --------------------------------------------------------------------------- //

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
        const std::vector<std::pair<std::size_t, std::string>>& deviceMap,
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

		std::uint64_t deviceSize = 0;
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
std::vector<std::pair<std::size_t, std::string>> parseDeviceMappings(const std::string& pairString,
                                                                const std::vector<std::string>& selectedIsos,
                                                                std::vector<std::string>& errors) {

	std::vector<std::pair<std::size_t, std::string>> deviceMap;
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

		std::size_t sepPos = pair.find('>');
		if (sepPos == std::string::npos) {
			errors.push_back(wt.warnLabel + "Invalid pair format: '" + wt.errLabel + pair + wt.warnLabel + "'");
			continue;
		}

		std::string indexStr = pair.substr(0, sepPos);
		std::string device   = pair.substr(sepPos + 1);
		trim(indexStr);
		trim(device);

		try {
			std::size_t index = std::stoul(indexStr);

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

	std::unordered_set<std::size_t> mappedIndices;
	for (const auto& [index, device] : deviceMap) {
		mappedIndices.insert(index);
	}

	for (std::size_t i = 1; i <= selectedIsos.size(); ++i) {
		if (mappedIndices.find(i) == mappedIndices.end()) {
			errors.push_back(wt.warnLabel + "Missing mapping for ISO '" + wt.errLabel + std::to_string(i) + wt.warnLabel + "'");
		}
	}

	return deviceMap;

}
