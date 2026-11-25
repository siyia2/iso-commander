// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>
#include <readline/readline.h>

import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.write2usb.util.terminalGuards;

namespace {

/// Readline bindings shared by the write-mapping prompt and confirmation mode.
void applyMappingPromptBindings() {
    rl_completion_display_matches_hook = [](char**, int, int) {};

    rl_attempted_completion_function = completion_cb;
    rl_bind_key('\t', rl_complete);
    rl_bind_key('\f', clear_screen_and_buffer);
    rl_bind_keyseq("\033[A", rl_get_previous_history);
    rl_bind_keyseq("\033[B", rl_get_next_history);
    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
}

} // namespace

TerminalStateGuard::TerminalStateGuard() {
    applyMappingPromptBindings();

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();
}

TerminalStateGuard::~TerminalStateGuard() {
    restoreReadline();
}

ConfirmationModeGuard::ConfirmationModeGuard() {
    disableReadlineForConfirmation();
}

ConfirmationModeGuard::~ConfirmationModeGuard() {
    setupReadline();
}

void ConfirmationModeGuard::setupReadline() {
    applyMappingPromptBindings();
}
