// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>

import isocmd.input.lineEditor;
import isocmd.input.inputHandling;
import isocmd.input.promptCompletion;
import isocmd.write2usb.util.terminalGuards;

namespace {

	/// Line editor bindings shared by the write-mapping prompt and confirmation mode.
	void applyMappingPromptBindings() {

		editorCompletionDisplayHook = [](const std::vector<std::string>&, int, int) {};

		editorCompletionHook = completion_cb;
		editorBindKey('\t', editorComplete);
		editorBindKey('\f', clear_screen_and_buffer);
		editorBindKeySequence("\033[A", editorPreviousHistory);
		editorBindKeySequence("\033[B", editorNextHistory);
		editorBindKeySequence("\033[5~", editorPreviousHistory);
		editorBindKeySequence("\033[6~", editorNextHistory);

	}

} // namespace

TerminalStateGuard::TerminalStateGuard() {

	applyMappingPromptBindings();

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();
	clearScrollBuffer();

}

TerminalStateGuard::~TerminalStateGuard() {

	restoreLineEditor();

}

ConfirmationModeGuard::ConfirmationModeGuard() {

	disableLineEditorForConfirmation();

}

ConfirmationModeGuard::~ConfirmationModeGuard() {

	setupLineEditor();

}

void ConfirmationModeGuard::setupLineEditor() {

	applyMappingPromptBindings();

}
