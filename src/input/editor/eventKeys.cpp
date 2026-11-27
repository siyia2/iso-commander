// SPDX-License-Identifier: GPL-3.0-or-later

// Handles interactive line editor hotkeys and turns them into commands for the active screen.

#include <stdio.h>

import isocmd.input.lineEditor;
import isocmd.input.inputHandling;
import isocmd.input.promptCompletion;

//=============================================================================
// Event Driven Keys
//=============================================================================

/* --- Hotkey submit helper ---------------------------------------------------
 *
 * Every hotkey submits a command by replacing the line buffer and finishing
 * the line. Finishing it with editorSubmit() uses the same native submit path as Enter.
 * The fixed prompt renderer advances one row after either kind of submission.
 * Callers no longer need any "step below the prompt row" fix-up after a hotkey.
 */

static void stashTypedInput() {

	RetainAndRestoreLineEditorBuffer::g_editorPendingText.assign(
	    editorLineBuffer ? editorLineBuffer : "", editorBufferEnd);

}

/// Replaces the line with @p command and submits it exactly as Enter would.
/// @param stash true to remember what the user had typed (restored by
///              armPendingInputRestore on the next prompt).
static int submitHotkey(const char* command, bool stash) {

	if (stash) stashTypedInput();
	editorReplaceLine(command);
	return editorSubmit(1, '\n');

}

/* --- Navigation & View Handlers --- */

int pgup_handler(int, int) {

	return submitHotkey("PgUp", true);

}

int pgdn_handler(int, int) {

	return submitHotkey("PgDn", true);

}

int toggleList_handler(int, int) {

	return submitHotkey("~",    true);

}

/* --- Action & Command Handlers --- */

int proc_handler(int, int) {

	return submitHotkey("P",   true);

}

int clr_handler(int, int) {

	return submitHotkey("clr", true);

}

int refresh_handler(int, int) {

	return submitHotkey("R",   false);

}

int filter_handler(int, int) {

	return submitHotkey("/",   false);

}

int toggleFlno_handler(int, int) {

	return submitHotkey("*",   true);

}

/* --- Settings & State Handlers --- */

int reset_handler(int, int) {

	return submitHotkey("r", false);

}

/* --- Global Utility Handlers --- */

int help_handler(int, int) {

	return submitHotkey("?",     true);

}

int exit_handler(int, int) {

	return submitHotkey("\x1b",  false);

}

/**
 * @brief Installs the custom completion display hook and restores history navigation keybindings.
 *
 * Sets @c editorCompletionDisplayHook to @c customListingsFunction,
 * clears @c editorCompletionHook, and rebinds the Up/Down arrows and
 * Page Up/Page Down to history navigation.
 *
 * @note Ctrl+L (@c '\\f') and Tab (@c '\\t') are bound to
 *       @c ignoreEditorKeybinding here, i.e. they are disabled. Use
 *       @c RestoreLineEditorBuffer to bind them to @c clear_screen_and_buffer and
 *       @c completePrompt for prompts that support completion.
 */
void restoreLineEditor() {

	editorCompletionDisplayHook = customListingsFunction;
	editorCompletionHook = nullptr;
	editorBindKeySequence("\033[A", editorPreviousHistory);
	editorBindKeySequence("\033[B", editorNextHistory);
	editorBindKeySequence("\033[5~", editorPreviousHistory);
	editorBindKeySequence("\033[6~", editorNextHistory);
	editorBindKey('\f', ignoreEditorKeybinding);
	editorBindKey('\t', ignoreEditorKeybinding);

}

/// Disables standard completion and history navigation for confirmation prompts.
void disableLineEditorForConfirmation() {

	editorBindKey('\f', ignoreEditorKeybinding);
	editorBindKey('\t', ignoreEditorKeybinding);
	editorBindKeySequence("\033[A", ignoreEditorKeybinding);
	editorBindKeySequence("\033[B", ignoreEditorKeybinding);
	editorBindKeySequence("\033[5~", ignoreEditorKeybinding);
	editorBindKeySequence("\033[6~", ignoreEditorKeybinding);

}

/**
 * @brief Keybindings for the main File Selection interface.
 *
 * Binds Page Up/Page Down (navigation), `*` (toggle file-number display),
 * `/` (filter), `P` (process), `C` (clear), `R` (refresh), `~` (toggle
 * list view), Esc (exit), and `?` (help).
 */
void setup_custom_keybindingsForSelect(void) {

	// Navigation
	editorBindKeySequence("\033[5~", pgup_handler);
	editorBindKeySequence("\033[6~", pgdn_handler);

	// Commands
	editorBindKeySequence("*", toggleFlno_handler);
	editorBindKeySequence("/", filter_handler);
	editorBindKeySequence("P", proc_handler);
	editorBindKeySequence("C", clr_handler);
	editorBindKeySequence("R", refresh_handler);
	editorBindKeySequence("~", toggleList_handler);
	editorBindKeySequence("\033", exit_handler);
	editorBindKeySequence("?", help_handler);

}

/// Keybindings for the Settings Editor.
void setup_custom_keybindingsForSettingsEditor(void) {

	editorBindKeySequence("r", reset_handler);
	editorBindKeySequence("?", help_handler);
	editorBindKeySequence("\033", exit_handler);

}

/// Keybindings for the Search Prompts.
void setup_custom_keybindingsForSearches(void) {

	editorBindKeySequence("\033[5~", editorPreviousHistory);
	editorBindKeySequence("\033[6~", editorNextHistory);

	editorBindKeySequence("?", help_handler);
	editorBindKeySequence("\033", exit_handler);

}

/// Restores all bindings used in the Selection UI to defaults.
void reset_custom_keybindingsForSelect(void) {

	// Restore navigation to history
	editorBindKeySequence("\033[5~", editorPreviousHistory);
	editorBindKeySequence("\033[6~", editorNextHistory);

	// Restore characters to standard insertion
	editorBindKeySequence("*", editorInsertKey);
	editorBindKeySequence("/", editorInsertKey);
	editorBindKeySequence("P", editorInsertKey);
	editorBindKeySequence("C", editorInsertKey);
	editorBindKeySequence("R", editorInsertKey);
	editorBindKeySequence("~", editorInsertKey);
	editorBindKeySequence("?", editorInsertKey);

	// Esc special case
	editorBindKeySequence("\033", exit_handler);

}

/**
 * @brief Restores bindings used in File Operations (Cp/Mv/USB).
 *
 * Reverts the single-character hotkeys (`*`, `/`, `P`, `R`, `C`, `~`) to
 * plain text insertion so they can be typed into path/name prompts. Esc is
 * intentionally left untouched here.
 */
void reset_custom_keybindingsForCpMvWrite2Usb(void) {

	editorBindKeySequence("*", editorInsertKey);
	editorBindKeySequence("/", editorInsertKey);
	editorBindKeySequence("P", editorInsertKey);
	editorBindKeySequence("R", editorInsertKey);
	editorBindKeySequence("C", editorInsertKey);
	editorBindKeySequence("~", editorInsertKey);

}

/**
 * @brief Restores bindings used in File Operations (Rm).
 *
 * Reverts the single-character hotkeys (`*`, `/`, `P`, `C`, `R`, `~`, `?`) to
 * plain text insertion and rebinds Esc to @c exit_handler.
 */
void reset_custom_keybindingsForRm(void) {

	// Restore characters to standard insertion
	editorBindKeySequence("*", editorInsertKey);
	editorBindKeySequence("/", editorInsertKey);
	editorBindKeySequence("P", editorInsertKey);
	editorBindKeySequence("C", editorInsertKey);
	editorBindKeySequence("R", editorInsertKey);
	editorBindKeySequence("~", editorInsertKey);
	editorBindKeySequence("?", editorInsertKey);

	// Esc special case
	editorBindKeySequence("\033", exit_handler);

}

/// Restores bindings used in the Settings Editor.
void reset_custom_keybindingsForSettingsEditor(void) {

	editorBindKeySequence("r", editorInsertKey);
	editorBindKeySequence("?", editorInsertKey);

	// Esc special case
	editorBindKeySequence("\033", exit_handler);

}

/**
 * @brief Restores bindings used in the Search Prompts.
 *
 * Reverts `?` to plain text insertion and rebinds Esc to @c exit_handler.
 */
void reset_custom_keybindingsForSearches(void) {

	editorBindKeySequence("?", editorInsertKey);

	// Esc special case
	editorBindKeySequence("\033", exit_handler);

}
