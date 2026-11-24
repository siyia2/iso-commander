// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstdio>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "isocmd/search/search.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/input/readline.h"

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
