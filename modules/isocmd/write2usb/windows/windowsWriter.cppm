// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.windows.windowsWriter;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// ------------------- Forward declarations -------------------

/**
 * @brief Per-device progress state; defined in isocmd.write2usb.util.writeTypes.
 * Import that module when accessing its members.
 */
struct ProgressInfo;

// ------------------- Windows ISO writer (windowsWriter.cpp) -------------------

/**
 * @brief Writes a Windows-family ISO to a block device by partitioning,
 *        formatting and copying files.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             std::size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData);

} // extern "C++"
