// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <sys/types.h>

export module isocmd.write2usb.util.isoDetect;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// ------------------- ISO detection (write2usbDetection.cpp) -------------------

namespace iso_detect {

	/// Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
	bool isValidIso9660(int fd);

	/// Probes an ISO image for signatures associated with Windows installation media.
	bool isWindowsIsoInitialCheck(int fd, off_t fileSize);

} // namespace iso_detect

} // extern "C++"
