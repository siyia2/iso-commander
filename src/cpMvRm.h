// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CPMVRM_H
#define CPMVRM_H

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./readline.h"

/**
 * @struct RlFormFeedGuard
 * @brief RAII guard to restore the '\f' key binding to @c prevent_readline_keybindings.
 * * Use in functions that bind '\f' but do not modify '\t' to ensure scope integrity.
 */
struct RlFormFeedGuard {
    ~RlFormFeedGuard() { rl_bind_key('\f', prevent_readline_keybindings); }
};

/**
 * @struct RlKeyBindGuard
 * @brief RAII guard to restore both '\f' and '\t' key bindings.
 * * Typically used around paginated display logic to ensure bindings are reverted
 * even on early returns or exceptions.
 */
struct RlKeyBindGuard {
    ~RlKeyBindGuard() {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
    }
};

/**
 * @struct RlCompleteModeGuard
 * @brief RAII guard to preserve the state of @c g_rl_complete_mode.
 */
struct RlCompleteModeGuard {
    int saved;
    RlCompleteModeGuard() : saved(RetainAndRestoreReadlineBuffer::g_rl_complete_mode) {}
    ~RlCompleteModeGuard() { RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved; }
};

/**
 * @struct RlStartupHookGuard
 * @brief RAII guard to clear @c rl_startup_hook on scope exit.
 * * Prevents stale hooks from persisting into subsequent Readline calls.
 */
struct RlStartupHookGuard {
    ~RlStartupHookGuard() { rl_startup_hook = nullptr; }
};

#endif // CPMVRM_H
