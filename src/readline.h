// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

#include "write.h"

// Forward declaration
struct IsoInfo;

// Global structure to hold completion data
struct CompleterData {
    const std::vector<IsoInfo>* sortedIsos;
    const std::vector<std::string>* usbDevices;
};

// Global instance declaration (extern means it's defined elsewhere)
extern CompleterData g_completerData;

// The completion callback function
char** completion_cb(const char* text, int start, int end);

#endif // READLINE_H
