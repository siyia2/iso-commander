// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.inputHandling;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * FUNCTION DECLARATIONS
 */

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

// --- 4. Line Editor Interface & UI Customization ---
// High-level wrapper functions for the line editor interactive prompt.
/// Displays paginated completion candidates using the active UI theme.
void customListingsFunction(const std::vector<std::string>& matches, int num_matches, int max_length);

/// Resets completion pagination, the cached common prefix and the repaint snapshot.
void resetLineEditorPagination();
void restoreLineEditor();
void disableLineEditorForConfirmation();

} // extern "C++"
