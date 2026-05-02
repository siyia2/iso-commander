// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

// C++ Standard Library Headers
#include <vector>
#include <string>
#include <cstdio>

/**
 * C-LINKAGE INTERFACE
 * Required for callbacks passed to the GNU Readline C library.
 */
#ifdef __cplusplus
extern "C" {
#endif

// Bridge function for Readline tab-completion
char** my_special_completion_entry(const char* text, int start, int end);

// Callback invoked by Readline for context-aware completions
char** completion_cb(const char* text, int start, int end);

// Keybinding utility functions
int prevent_readline_keybindings(int count, int key);
int clear_screen_and_buffer(int count, int key);

#ifdef __cplusplus
}
#endif

/**
 * C++ DATA STRUCTURES
 */

struct IsoInfo; // Forward declaration

/**
 * @struct CompleterData
 * @brief Encapsulates datasets required for tab-completion.
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
 * KEYBINDING MANAGEMENT
 * Functions to toggle between "Hotkeys" and "Standard Typing" modes.
 */

// --- File Selection Mode ---
void setup_custom_keybindingsForSelect(void);
void reset_custom_keybindingsForSelect(void);

// --- Settings Editor Mode ---
void setup_custom_keybindingsForSettingsEditor(void);
void reset_custom_keybindingsForSettingsEditor(void);

// --- File Operations Mode (Cp/Mv/USB) ---
void reset_custom_keybindingsForCpMvWrite2Usb(void);

#endif // READLINE_H
