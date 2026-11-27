// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Application signal handling and low-level terminal input control.
 */

export module isocmd.input.terminalControl;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// --- 1. Signal & OS Interrupt Handling ---
// Should be initialized first to ensure app stability.
void setupLineEditorToIgnoreCtrlC();
void setupSignalHandlerCancellations();
void signalHandler(int signum);

// --- 2. Low-Level Terminal State Management ---
// Direct interaction with termios and standard input buffers.
void disableInput();
void restoreInput();
void flushStdin();
void clearScrollBuffer();

// --- 3. Input Stream Logic (Ctrl-D / EOF) ---
// Specialized toggles for handling the End-Of-File character.
void disable_ctrl_d();
void enable_ctrl_d();

} // extern "C++"
