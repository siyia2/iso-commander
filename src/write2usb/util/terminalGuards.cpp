// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Implementation of USB prompt editor guards.
 */

#include <stdio.h>
#include <signal.h>

import isocmd.input.lineEditor;
import isocmd.input.inputHandling;
import isocmd.input.terminalControl;
import isocmd.input.promptCompletion;
import isocmd.write2usb.util.terminalGuards;

namespace {

	/**
	 * @brief Installs mapping-prompt completion and history keybindings.
	 * Suppresses completion listings, enables Tab completion and Ctrl+L clearing,
	 * and maps arrow and page keys to history navigation.
	 */
	void applyMappingPromptBindings() {

		editorCompletionDisplayHook = [](const std::vector<std::string>&, int, int) {};

		editorCompletionHook = completion_cb;
		editorBindKey('\t', completeMappingPrompt);
		editorBindKey('\f', clear_screen_and_buffer);
		editorBindKeySequence("\033[A", editorPreviousHistory);
		editorBindKeySequence("\033[B", editorNextHistory);
		editorBindKeySequence("\033[5~", editorPreviousHistory);
		editorBindKeySequence("\033[6~", editorNextHistory);

	}

} // namespace

/// Establishes the mapping-prompt configuration and terminal input policy.
TerminalStateGuard::TerminalStateGuard() {

	applyMappingPromptBindings();

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();
	clearScrollBuffer();

}

/// Resets editor hooks and bindings; signal and VEOF settings are not restored.
TerminalStateGuard::~TerminalStateGuard() {

	restoreLineEditor();

}

/// Disables completion, clearing and history keys for confirmation.
ConfirmationModeGuard::ConfirmationModeGuard() {

	disableLineEditorForConfirmation();

}

/// Reinstates the mapping-prompt configuration on scope exit.
ConfirmationModeGuard::~ConfirmationModeGuard() {

	setupLineEditor();

}

/// Uses the same binding configuration as TerminalStateGuard.
void ConfirmationModeGuard::setupLineEditor() {

	applyMappingPromptBindings();

}
