// SPDX-License-Identifier: GPL-3.0-or-later

// Restores Readline session state when a directory-search prompt ends.

#include <stdio.h>
#include <readline/history.h>
#include <readline/readline.h>

import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.search.search;

SearchReadlineGuard::SearchReadlineGuard()
    : oldCompletion_(rl_attempted_completion_function) {}

SearchReadlineGuard::~SearchReadlineGuard() {

	rl_attempted_completion_function = oldCompletion_;
	rl_bind_key('\t', prevent_readline_keybindings);
	rl_bind_key('\f', rl_insert);
	clear_history();
	resetReadlinePagination();
	reset_custom_keybindingsForSearches();

}
