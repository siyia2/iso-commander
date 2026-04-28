// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

#include "write.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bridge function for Readline tab-completion.
 */
char** my_special_completion_entry(const char* text, int start, int end);

#ifdef __cplusplus
}

struct IsoInfo;

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Encapsulates the datasets required for tab-completion, including 
 * available ISO images and detected USB devices.
 */
struct CompleterData {
    /** @brief Pointer to the processed list of ISO images. */
    const std::vector<IsoInfo>* sortedIsos;
    
    /** @brief Pointer to the list of available USB device paths. */
    const std::vector<std::string>* usbDevices;
};

/** @brief Global access point for the completion data used by the Readline callback. */
extern CompleterData g_completerData;

/**
 * @brief Callback function invoked by Readline to provide context-aware completions.
 * @param text The current word being typed.
 * @param start The start index of the word in the line buffer.
 * @param end The end index of the word in the line buffer.
 * @return An array of strings representing possible matches.
 */
char** completion_cb(const char* text, int start, int end);

/**
 * @brief Initializes and binds custom key sequences for file selection.
 * * Maps physical keys (like PageUp/PageDown) and character keys (*, <, ?, ~)
 * to their respective internal handlers to allow single-key navigation 
 * without requiring the Enter key.
 */
void setup_custom_keybindingsForSelect(void);

/**
 * @brief Restores standard Readline behavior for modified keys.
 * * Unbinds custom selection handlers and restores default functions, such 
 * as history navigation and standard character insertion, to ensure 
 * normal terminal behavior elsewhere in the application.
 */
void reset_custom_keybindingsForSelect(void);

#endif
#endif // READLINE_H
