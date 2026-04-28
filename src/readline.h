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
 * @brief Readline key handler for the Page Up key.
 * * Sets the line buffer to "PgUp" and forces an immediate return to signal
 * a request for the previous page of data.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int pgup_handler(int count, int key);

/**
 * @brief Readline key handler for the '~' key.
 * * Sets the line buffer to "~" and forces an immediate return to toggle
 * between different list view modes.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int toggleList_handler(int count, int key);

/**
 * @brief Readline key handler for the '?' key.
 * * Sets the line buffer to "?" and forces an immediate return to trigger
 * the display of the help or usage interface.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int toggleHelp_handler(int count, int key);

/**
 * @brief Readline key handler for the '<' key.
 * * Sets the line buffer to "<" and forces an immediate return to signal
 * an exit or "back" command from the current menu.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int toggleExit_handler(int count, int key);

/**
 * @brief Readline key handler for the '*' key.
 * * Sets the line buffer to "*" and forces an immediate return, typically 
 * used to toggle file numbering or select-all functionality.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int flno_handler(int count, int key);

/**
 * @brief Readline key handler for the Page Down key.
 * * Sets the line buffer to "PgDn" and forces an immediate return to signal
 * a request for the next page of data.
 * * @param count Unused readline repeat count.
 * @param key   The key code that triggered the handler.
 * @return Always returns 0.
 */
int pgdn_handler(int count, int key);

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
