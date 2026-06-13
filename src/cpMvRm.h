// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CPMVRM_H
#define CPMVRM_H

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./readline.h"

/**
 * @namespace RetainAndRestoreReadlineBuffer
 * @brief Global state management for Readline buffer persistence during completion cycles.
 */
namespace RetainAndRestoreReadlineBuffer {
    /// @brief Mode flag to determine if completion text should be persisted across Readline calls.
    inline int g_rl_complete_mode = 0;
    /// @brief Storage for the completed line text to be restored after a reset.
    inline std::string g_rl_pending_text = "";
}

/**
 * @brief Clears the terminal screen and the current Readline buffer.
 * @param count Numeric argument (standard Readline hook signature).
 * @param key Triggering key code.
 * @return 0 on success.
 */
int clear_screen_and_buffer(int count, int key);

/**
 * @brief TAB-completion handler that extends @c rl_complete with pagination reset.
 *
 * Delegates to @c rl_complete_internal('?') on double-TAB (list all matches)
 * or @c rl_complete_internal('!') on single TAB (insert/complete). If the
 * cursor position or line buffer changed after completion, clears the scroll
 * buffer. When @c g_rl_complete_mode is 1, additionally saves the
 * completed text to @c g_rl_pending_text, clears the input buffer,
 * and sets @c rl_done to exit Readline — allowing the next loop iteration to
 * redisplay with the pending text restored via the startup hook.
 *
 * @param ignore Numeric argument (unused).
 * @param invoking_key Triggering key (unused).
 * @return Return value of @c rl_complete_internal.
 */
int my_rl_complete(int ignore, int invoking_key);

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
