// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CPMVRM_H
#define CPMVRM_H

#include "./readline.h"
#include "./state.h"

#include <readline/readline.h>

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

// Saves and restores GlobalState::g_rl_complete_mode on scope exit.
struct RlCompleteModeGuard {
    int saved;
    RlCompleteModeGuard() : saved(GlobalState::g_rl_complete_mode) {}
    ~RlCompleteModeGuard() { GlobalState::g_rl_complete_mode = saved; }
};

// Always clears rl_startup_hook on scope exit, preventing stale hooks from
// leaking into the next readline call.
struct RlStartupHookGuard {
    ~RlStartupHookGuard() { rl_startup_hook = nullptr; }
};


#endif // CPMVRM_H
