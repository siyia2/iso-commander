// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

#include "write2usb.h"
#include <unordered_set>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>

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
 * 
 * Maps physical keys (PageUp/PageDown) and character keys (*, /, P, C, R, ~, <, ?)
 * to internal handlers, enabling a "hotkey" style interface that triggers 
 * actions immediately without the Enter key.
 */
void setup_custom_keybindingsForSelect(void);

/**
 * @brief Restores standard Readline behavior after file selection.
 * 
 * Reverts navigation keys to history scrolling and character keys back to 
 * standard text insertion to prevent "hotkeys" from leaking into other prompts.
 */
void reset_custom_keybindingsForSelect(void);

/**
 * @brief Initializes and binds custom key sequences for the Settings Editor.
 * 
 * Maps 's' (save), 'r' (reset), and '?' (help) to their respective handlers 
 * for quick single-key configuration management.
 */
void setup_custom_keybindingsForSettingsEditor(void);

/**
 * @brief Restores standard Readline behavior after exiting the Settings Editor.
 * 
 * Resets 's', 'r', and '?' to standard character insertion.
 */
void reset_custom_keybindingsForSettingsEditor(void);

/**
 * @brief Restores standard Readline behavior after Copy, Move, or USB operations.
 * 
 * Cleans up specific character bindings (*, /, P, R, C, ~) used during 
 * high-level file operations to ensure the terminal returns to normal input mode.
 */
void reset_custom_keybindingsForCpMvWrite2Usb(void);

int prevent_readline_keybindings(int, int);
int clear_screen_and_buffer(int, int);

#endif
#endif // READLINE_H
