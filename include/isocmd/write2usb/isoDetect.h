// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ISODETECT_H
#define ISODETECT_H

// C++ Standard Library Headers
#include <string>

// C / System Headers
#include <sys/types.h>

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

#endif // ISODETECT_H
