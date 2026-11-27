// SPDX-License-Identifier: GPL-3.0-or-later

// Finds removable USB block devices and checks their properties through Linux sysfs.

import std;

import isocmd.write2usb.ui.write2usbUI;

namespace fs = std::filesystem;

// --------------------------------------------------------------------------- //
// Device discovery (sysfs / procfs)                                           //
// --------------------------------------------------------------------------- //

/**
 * @brief Reads a block device's model name from sysfs.
 *
 * Reads /sys/block/<name>/device/model and trims surrounding whitespace.
 *
 * @param device Absolute block-device path (e.g. /dev/sdc).
 * @return Model name, or "Unknown Drive" if the file is absent or empty.
 */
std::string getDriveName(const std::string& device, const DeviceDiscoveryPaths& paths) {

	std::string deviceName = device.substr(device.find_last_of('/') + 1);
	const auto sysfsPath = paths.sysBlock / deviceName / "device/model";

	std::ifstream modelFile(sysfsPath);
	std::string driveName;

	if (modelFile.is_open()) {
		std::getline(modelFile, driveName);
		driveName.erase(0, driveName.find_first_not_of(" \t"));
		driveName.erase(driveName.find_last_not_of(" \t") + 1);
	}

	return driveName.empty() ? "Unknown Drive" : driveName;

}

namespace {

	/**
	 * @brief Reads and right-trims the first line of a sysfs attribute file.
	 *
	 * @param attributePath Full path to the sysfs attribute file to read.
	 * @return Trimmed string contents of the first line, or std::nullopt on read failure.
	 */
	static std::optional<std::string> readTrimmedSysfsAttribute(const fs::path& attributePath) {

		std::ifstream file(attributePath);
		std::string value;
		if (!file || !std::getline(file, value)) {
			return std::nullopt;
		}

		while (!value.empty() &&
		       std::isspace(static_cast<unsigned char>(value.back()))) {
			value.pop_back();
		}

		return value;

	}

	/**
	 * @brief Resolves a device path under /dev/ to its /sys/block entry path.
	 *
	 * Validates the input prefix, resolves symlinks to a canonical device node,
	 * and verifies that a corresponding /sys/block/<name> entry exists.
	 *
	 * @param devicePath Block-device path (must start with /dev/).
	 * @param ec Output error code populated on filesystem errors.
	 * @return Canonical path under /sys/block if valid and existing, otherwise std::nullopt.
	 */
	static std::optional<fs::path> resolveSysBlockPath(const std::string& devicePath,
	                                                   std::error_code& ec, const DeviceDiscoveryPaths& paths) {

		const auto prefix = paths.dev.string() + "/";
		if (devicePath.size() <= prefix.size() || devicePath.rfind(prefix, 0) != 0) {
			return std::nullopt;
		}

		const fs::path devNode = fs::canonical(devicePath, ec);
		if (ec) {
			return std::nullopt;
		}

		const std::string name = devNode.filename().string();
		if (name.empty()) {
			return std::nullopt;
		}

		const fs::path sysPath = paths.sysBlock / name;
		if (!fs::exists(sysPath, ec) || ec) {
			return std::nullopt;
		}

		return sysPath;

	}

	/**
	 * @brief Walks up a canonical sysfs path looking for a USB device node.
	 *
	 * Identifies a USB device ancestor by checking for the presence of an
	 * "idVendor" file at each directory level up to the root path.
	 *
	 * @param canonicalSysPath Canonical path within the sysfs hierarchy.
	 * @param ec Working error code cleared across filesystem probes.
	 * @return true if an ancestor directory contains "idVendor", false otherwise.
	 */
	static bool hasUsbVendorAncestor(const fs::path& canonicalSysPath, std::error_code& ec) {

		for (fs::path p = canonicalSysPath; p != p.root_path(); p = p.parent_path()) {
			if (fs::exists(p / "idVendor", ec)) {
				return true;
			}
			ec.clear();
		}

		return false;

	}

} // namespace

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
bool isFlashDrive(const std::string& devicePath, const DeviceDiscoveryPaths& paths) {

	try {
		std::error_code ec;

		const auto sysPath = resolveSysBlockPath(devicePath, ec, paths);
		if (!sysPath) {
			return false;
		}

		const auto removableValue = readTrimmedSysfsAttribute(*sysPath / "removable");
		if (!removableValue || *removableValue != "1") {
			return false;
		}

		const fs::path resolved = fs::canonical(*sysPath, ec);
		if (ec) {
			return false;
		}

		return hasUsbVendorAncestor(resolved, ec);
	} catch (...) {
		return false;
	}

}

/**
 * @brief Enumerates removable USB block devices visible in /sys/block.
 *
 * Skips loop devices, RAM disks, zram devices, and any name ending in a
 * digit. A device is included only when its removable sysfs
 * attribute reads "1" and isFlashDrive() confirms it is USB-attached.
 *
 * @param paths Filesystem roots for device discovery.
 * @return Device paths under paths.dev (normally /dev).
 */
std::vector<std::string> getRemovableDevices(const DeviceDiscoveryPaths& paths) {

	std::vector<std::string> devices;

	try {
		for (const auto& entry : fs::directory_iterator(paths.sysBlock)) {
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

			const std::string devPath = (paths.dev / deviceName).string();

			if (isFlashDrive(devPath, paths)) {
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
 * @param paths Filesystem roots for mount and device discovery.
 * @return true on a matching mount entry; false if none is found or the mount
 *         table cannot be opened.
 */
bool isDeviceMounted(const std::string& device, const DeviceDiscoveryPaths& paths) {

	std::ifstream mountsFile(paths.mounts);
	if (!mountsFile.is_open()) {
		return false;
	}

	std::string line;
	std::string deviceName = device;

	const auto prefix = paths.dev.string() + "/";
	if (deviceName.rfind(prefix, 0) == 0) {
		deviceName = deviceName.substr(prefix.size());
	}

	while (std::getline(mountsFile, line)) {
		std::istringstream iss(line);
		std::string mountDevice;
		iss >> mountDevice;

		if (mountDevice.rfind(prefix, 0) == 0) {
			mountDevice = mountDevice.substr(prefix.size());
		}

		if (mountDevice == deviceName ||
		    (mountDevice.find(deviceName) == 0 &&
		     std::isdigit(static_cast<unsigned char>(mountDevice[deviceName.length()])))) {
			mountsFile.close();
			return true;
		}
	}

	mountsFile.close();
	return false;

}

// Production entry points retain their original system paths.
std::string getDriveName(const std::string& device) {

	return getDriveName(device, {});

}

bool isFlashDrive(const std::string& device) {

	return isFlashDrive(device, {});

}

std::vector<std::string> getRemovableDevices() {

	return getRemovableDevices({});

}

bool isDeviceMounted(const std::string& device) {

	return isDeviceMounted(device, {});

}
