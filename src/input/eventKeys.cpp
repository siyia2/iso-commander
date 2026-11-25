// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <readline/readline.h>

import isocmd.input.inputHandling;
import isocmd.input.readline;

//=============================================================================
// Event Driven Keys
//=============================================================================

/* --- Hotkey submit helper ---------------------------------------------------
 *
 * Every hotkey submits a command by replacing the line buffer and finishing
 * the line. Finishing it with rl_newline() (what Enter does) instead of just
 * setting rl_done makes readline run its final-update step, which moves to the
 * end of the prompt and prints the newline. As a result a hotkey submit and an
 * Enter submit leave the cursor in the same place: one row below the prompt.
 * Callers no longer need any "step below the prompt row" fix-up after a hotkey.
 */

static void stashTypedInput() {

	RetainAndRestoreReadlineBuffer::g_rl_pending_text.assign(
	    rl_line_buffer ? rl_line_buffer : "", rl_end);

}

/// Replaces the line with @p command and submits it exactly as Enter would.
/// @param stash true to remember what the user had typed (restored by
///              armPendingInputRestore on the next prompt).
static int submitHotkey(const char* command, bool stash) {

	if (stash) stashTypedInput();
	rl_replace_line(command, 0);
	return rl_newline(1, '\n');

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
 * Sets @c rl_completion_display_matches_hook to @c customListingsFunction,
 * clears @c rl_attempted_completion_function, and rebinds the Up/Down arrows and
 * PageUp/PageDown to history navigation.
 *
 * @note Ctrl+L (@c '\\f') and Tab (@c '\\t') are bound to
 *       @c prevent_readline_keybindings here, i.e. they are disabled. Use
 *       @c RestoreReadlineBuffer to bind them to @c clear_screen_and_buffer and
 *       @c my_rl_complete for prompts that support completion.
 */
void restoreReadline() {

	rl_completion_display_matches_hook = customListingsFunction;
	rl_attempted_completion_function = nullptr;
	rl_bind_keyseq("\033[A", rl_get_previous_history);
	rl_bind_keyseq("\033[B", rl_get_next_history);
	rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
	rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
	rl_bind_key('\f', prevent_readline_keybindings);
	rl_bind_key('\t', prevent_readline_keybindings);

}

/**
 * @brief Disables standard completion and history navigation for confirmation prompts.
 */
void disableReadlineForConfirmation() {

	rl_bind_key('\f', prevent_readline_keybindings);
	rl_bind_key('\t', prevent_readline_keybindings);
	rl_bind_keyseq("\033[A", prevent_readline_keybindings);
	rl_bind_keyseq("\033[B", prevent_readline_keybindings);
	rl_bind_keyseq("\\e[5~", prevent_readline_keybindings);
	rl_bind_keyseq("\\e[6~", prevent_readline_keybindings);

}

/**
 * @brief Keybindings for the main File Selection interface.
 *
 * Binds PageUp/PageDown (navigation), `*` (toggle file-number display),
 * `/` (filter), `P` (process), `C` (clear), `R` (refresh), `~` (toggle
 * list view), Esc (exit), and `?` (help).
 */
void setup_custom_keybindingsForSelect(void) {

	// Navigation
	rl_bind_keyseq("\\e[5~", pgup_handler);
	rl_bind_keyseq("\\e[6~", pgdn_handler);

	// Commands
	rl_bind_keyseq("*", toggleFlno_handler);
	rl_bind_keyseq("/", filter_handler);
	rl_bind_keyseq("P", proc_handler);
	rl_bind_keyseq("C", clr_handler);
	rl_bind_keyseq("R", refresh_handler);
	rl_bind_keyseq("~", toggleList_handler);
	rl_bind_keyseq("\\e", exit_handler);
	rl_bind_keyseq("?", help_handler);

}

/**
 * @brief Keybindings for the Settings Editor.
 */
void setup_custom_keybindingsForSettingsEditor(void) {

	rl_bind_keyseq("r", reset_handler);
	rl_bind_keyseq("?", help_handler);
	rl_bind_keyseq("\\e", exit_handler);

}

/**
 * @brief Keybindings for the Search Prompts.
 */
void setup_custom_keybindingsForSearches(void) {

	rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
	rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

	rl_bind_keyseq("?", help_handler);
	rl_bind_keyseq("\\e", exit_handler);

}

/**
 * @brief Restores all bindings used in the Selection UI to defaults.
 */
void reset_custom_keybindingsForSelect(void) {

	// Restore navigation to history
	rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
	rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

	// Restore characters to standard insertion
	rl_bind_keyseq("*", rl_insert);
	rl_bind_keyseq("/", rl_insert);
	rl_bind_keyseq("P", rl_insert);
	rl_bind_keyseq("C", rl_insert);
	rl_bind_keyseq("R", rl_insert);
	rl_bind_keyseq("~", rl_insert);
	rl_bind_keyseq("?", rl_insert);

	// Esc special case
	rl_bind_keyseq("\\e", exit_handler);

}

/**
 * @brief Restores bindings used in File Operations (Cp/Mv/USB).
 *
 * Reverts the single-character hotkeys (`*`, `/`, `P`, `R`, `C`, `~`) to
 * plain text insertion so they can be typed into path/name prompts. Esc is
 * intentionally left untouched here.
 */
void reset_custom_keybindingsForCpMvWrite2Usb(void) {

	rl_bind_keyseq("*", rl_insert);
	rl_bind_keyseq("/", rl_insert);
	rl_bind_keyseq("P", rl_insert);
	rl_bind_keyseq("R", rl_insert);
	rl_bind_keyseq("C", rl_insert);
	rl_bind_keyseq("~", rl_insert);

}

/**
 * @brief Restores bindings used in File Operations (Rm).
 *
 * Reverts the single-character hotkeys (`*`, `/`, `P`, `C`, `R`, `~`, `?`) to
 * plain text insertion and rebinds Esc to @c exit_handler.
 */
void reset_custom_keybindingsForRm(void) {

	// Restore characters to standard insertion
	rl_bind_keyseq("*", rl_insert);
	rl_bind_keyseq("/", rl_insert);
	rl_bind_keyseq("P", rl_insert);
	rl_bind_keyseq("C", rl_insert);
	rl_bind_keyseq("R", rl_insert);
	rl_bind_keyseq("~", rl_insert);
	rl_bind_keyseq("?", rl_insert);

	// Esc special case
	rl_bind_keyseq("\\e", exit_handler);

}

/**
 * @brief Restores bindings used in the Settings Editor.
 */
void reset_custom_keybindingsForSettingsEditor(void) {

	rl_bind_keyseq("r", rl_insert);
	rl_bind_keyseq("?", rl_insert);

	// Esc special case
	rl_bind_keyseq("\\e", exit_handler);

}

/**
 * @brief Restores bindings used in the Search Prompts.
 *
 * Reverts `?` to plain text insertion and rebinds Esc to @c exit_handler.
 */
void reset_custom_keybindingsForSearches(void) {

	rl_bind_keyseq("?", rl_insert);

	// Esc special case
	rl_bind_keyseq("\\e", exit_handler);

}
