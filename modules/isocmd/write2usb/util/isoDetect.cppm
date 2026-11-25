// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <sys/types.h>

export module isocmd.write2usb.util.isoDetect;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/isoDetect ----

// C / System Headers

// ------------------- ISO detection (isoDetect.cpp) -------------------

namespace iso_detect {

	/**
	 * @brief Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
	 */
	bool isValidIso9660(int fd);

	/**
	 * @brief Probes an ISO image to detect Windows 10/11 installation media.
	 */
	bool isWindowsIsoInitialCheck(int fd, off_t fileSize);

} // namespace iso_detect

} // extern "C++"
