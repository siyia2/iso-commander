// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.windows.windowsWriter;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/windows/windowsWriter ----

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
 * @brief Writes a Windows-family ISO to a block device by partitioning,
 *        formatting and copying files.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             std::size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData);

} // extern "C++"
