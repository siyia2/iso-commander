// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Optical-image recognition and heuristic Windows media detection.
 */

module;

#include <sys/types.h>

export module isocmd.write2usb.util.isoDetect;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

namespace iso_detect {

	/**
	 * @brief Probes sectors 16–23 for accepted ISO 9660 or UDF descriptors.
	 * @param fd Open, readable image descriptor; ownership remains with the caller.
	 * @return true on an accepted descriptor, false on an unrecognized entry,
	 *         short read or exhausted scan.
	 * @note Uses pread() without changing the file offset. Does not validate the
	 *       entire filesystem or establish bootability.
	 */
	bool isValidIso9660(int fd);

	/**
	 * @brief Probes an optical image for Windows installation-media signatures.
	 * @param fd Open, readable image descriptor; not closed by this function.
	 * @param fileSize Image length in bytes, normally obtained from fstat().
	 * @return true when sector 16 has an accepted header and the mapped window (up to 32 MiB)
	 *         contain boot.wim and bootmgr (or bootmgr.efi) signatures.
	 * @note This case-insensitive byte-pattern heuristic does not establish
	 *       the Windows version, file existence or bootability.
	 */
	bool isWindowsIsoInitialCheck(int fd, off_t fileSize);

} // namespace iso_detect

} // extern "C++"
