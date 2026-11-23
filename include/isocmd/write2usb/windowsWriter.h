// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WINDOWSWRITER_H
#define WINDOWSWRITER_H

// C++ Standard Library Headers
#include <cstddef>
#include <string>
#include <vector>

// ------------------- Forward declarations -------------------

/**
 * @brief Per-device progress slot (progress, speed, bytes written, and
 *        completed/failed flags).
 *
 * Only forward-declared here so this header stays independent of include
 * order. The full definition lives elsewhere in the project (e.g.
 * write2usbUI.h or state.h); any translation unit that actually accesses
 * its members must include that header.
 */
struct ProgressInfo;

// ------------------- Windows ISO writer (windowsWriter.cpp) -------------------

/**
 * @brief Detects whether an ISO image is Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath);

/**
 * @brief Writes a Windows-family ISO to a block device by partitioning,
 *        formatting and copying files.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData);

#endif // WINDOWSWRITER_H
