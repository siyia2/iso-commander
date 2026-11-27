// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.inputHandling;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @file
 * @brief Application-specific line editor configuration and completion display.
 */

// --- Line Editor Interface & UI Customization ---
// High-level wrapper functions for the line editor interactive prompt.
/// Displays paginated completion candidates using the active UI theme.
void customListingsFunction(const std::vector<std::string>& matches, int num_matches, int max_length);

/// Resets completion pagination, the cached common prefix and the repaint snapshot.
void resetLineEditorPagination();
void restoreLineEditor();
void disableLineEditorForConfirmation();

} // extern "C++"
