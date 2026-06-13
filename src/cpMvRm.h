// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CPMVRM_H
#define CPMVRM_H

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./readline.h"

namespace RetainAndRestoreReadlineBuffer {
    inline int g_rl_complete_mode         = 0;
    inline std::string g_rl_pending_text  = "";
}

int clear_screen_and_buffer(int, int);

/**
 * @brief TAB-completion handler that extends @c rl_complete with pagination reset.
 *
 * Delegates to @c rl_complete_internal('?') on double-TAB (list all matches)
 * or @c rl_complete_internal('!') on single TAB (insert/complete). If the
 * cursor position or line buffer changed after completion, clears the scroll
 * buffer. When @c GlobalState::g_rl_complete_mode is 1, additionally saves the
 * completed text to @c GlobalState::g_rl_pending_text, clears the input buffer,
 * and sets @c rl_done to exit readline — allowing the next loop iteration to
 * redisplay with the pending text restored via the startup hook.
 *
 * @param ignore       Numeric argument (unused).
 * @param invoking_key Triggering key (unused).
 * @return Return value of @c rl_complete_internal.
 */
int my_rl_complete(int ignore, int invoking_key)
{
    (void)ignore;
    (void)invoking_key;

    int old_point = rl_point;
    char *old_text = rl_copy_text(0, rl_end);

    int ret;
    if (rl_last_func == my_rl_complete)
        ret = rl_complete_internal('?');
    else
        ret = rl_complete_internal('!');

    char *new_text = rl_copy_text(0, rl_end);
    if (rl_point != old_point || strcmp(old_text, new_text) != 0) {
        clear_screen_and_buffer(0, 0);
        if (RetainAndRestoreReadlineBuffer::g_rl_complete_mode == 1) {
            RetainAndRestoreReadlineBuffer::g_rl_pending_text = new_text;  // save completed text
            rl_replace_line("", 0);                      // clear buffer so empty is submitted
            rl_done = 1;
            RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;
        }
    }

    free(old_text);
    free(new_text);
    return ret;
}

// ─── RAII guards (add near top of file / in a shared header) ──────────────────

// Restores only the '\f' key binding to prevent_readline_keybindings on scope exit.
// Use in functions that bind '\f' but never touch '\t', such as handleDeleteOperation,
// to avoid clobbering any '\t' binding set by the caller.
struct RlFormFeedGuard {
    ~RlFormFeedGuard() { rl_bind_key('\f', prevent_readline_keybindings); }
};

// Restores both '\f' and '\t' key bindings to prevent_readline_keybindings on scope exit.
// Placed around handlePaginatedDisplay so the rebind always fires, even on
// early returns or exceptions.
struct RlKeyBindGuard {
    ~RlKeyBindGuard() {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
    }
};

// Saves and restores RetainAndRestoreReadlineBuffer::g_rl_complete_mode on scope exit.
struct RlCompleteModeGuard {
    int saved;
    RlCompleteModeGuard() : saved(RetainAndRestoreReadlineBuffer::g_rl_complete_mode) {}
    ~RlCompleteModeGuard() { RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved; }
};

// Always clears rl_startup_hook on scope exit, preventing stale hooks from
// leaking into the next readline call.
struct RlStartupHookGuard {
    ~RlStartupHookGuard() { rl_startup_hook = nullptr; }
};


#endif // CPMVRM_H
