// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.inputHandling;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- input/inputHandling ----
/**
 * FUNCTION DECLARATIONS
 */

// --- 1. Signal & OS Interrupt Handling ---
// Should be initialized first to ensure app stability.
void setupReadlineToIgnoreCtrlC();
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

// --- 4. Readline Interface & UI Customization ---
// High-level wrapper functions for the Readline interactive prompt.
void customListingsFunction(char **matches, int num_matches, int max_length);
void resetReadlinePagination();
void restoreReadline();
void disableReadlineForConfirmation();

} // extern "C++"
