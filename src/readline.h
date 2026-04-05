// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

#include "write.h"

#ifdef __cplusplus
extern "C" {
#endif

char** my_special_completion_entry(const char* text, int start, int end);

#ifdef __cplusplus
} // end extern "C"

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

#endif // __cplusplus
#endif // READLINE_H
