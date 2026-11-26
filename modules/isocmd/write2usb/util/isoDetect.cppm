// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <sys/types.h>

export module isocmd.write2usb.util.isoDetect;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// C / System Headers

// ------------------- ISO detection (isoDetect.cpp) -------------------

namespace iso_detect {

	/// Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
	bool isValidIso9660(int fd);

	/// Probes an ISO image to detect Windows 10/11 installation media.
	bool isWindowsIsoInitialCheck(int fd, off_t fileSize);

} // namespace iso_detect

} // extern "C++"
