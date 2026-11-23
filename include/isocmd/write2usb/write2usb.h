// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE2USB_H
#define WRITE2USB_H

/**
 * @file write2usb.h
 * @brief Umbrella header for the write2usb helper headers.
 *
 * @details Includes every header below for convenience. Prefer including
 * only the ones you use, so editing one does not rebuild unrelated files.
 *
 * | Header                  | Provides                                                  |
 * |-------------------------|-----------------------------------------------------------|
 * | isoDetect.h             | iso_detect::isValidIso9660, iso_detect::isWindowsIsoInitialCheck |
 * | windowsWriter.h         | isWindowsIso(), writeWindowsIsoToDevice()                 |
 * | alignedBuffer.h         | AlignedBuffer                                             |
 * | threadGuard.h           | ThreadGuard                                               |
 * | windowsWriterEngine.h   | IsoCopyEntry, IsoType, windowsWriter_engine::copy         |
 */

// Project Headers
#include "isocmd/write2usb/alignedBuffer.h"
#include "isocmd/write2usb/isoDetect.h"
#include "isocmd/write2usb/threadGuard.h"
#include "isocmd/write2usb/windowsWriter.h"
#include "isocmd/write2usb/windowsWriterEngine.h"

#endif // WRITE2USB_H
