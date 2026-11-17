// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef READLINE_H
#define READLINE_H

// C++ Standard Library Headers
#include <functional>
#include <string>
#include <vector>

/**
 * C-LINKAGE INTERFACE
 * Required for callbacks passed to the GNU Readline C library.
 */
#ifdef __cplusplus
extern "C" {
#endif

// Bridge function for Readline tab-completion
char** my_special_completion_entry(const char* text, int start, int end);

// Callback invoked by Readline for context-aware completions
char** completion_cb(const char* text, int start, int end);

// Keybinding utility functions
int prevent_readline_keybindings(int count, int key);
int clear_screen_and_buffer(int count, int key);

#ifdef __cplusplus
}
#endif

/**
 * C++ DATA STRUCTURES
 */

struct IsoInfo; // Forward declaration

/**
 * @struct CompleterData
 * @brief Encapsulates datasets required for tab-completion.
 */
struct CompleterData {
    /** @brief Pointer to the processed list of ISO images. */
    const std::vector<IsoInfo>* sortedIsos;

    /** @brief Pointer to the list of available USB device paths. */
    const std::vector<std::string>* usbDevices;
};

/** @brief Global access point for the completion data used by the Readline callback. */
extern CompleterData g_completerData;

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
 * @brief Clears pending Readline text when the guard goes out of scope.
 */
struct PendingReadlineTextClearGuard {
    ~PendingReadlineTextClearGuard() {
        RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
    }
};

/**
 * @brief Active candidate list for Settings Editor value completion.
 *
 * Populated by the Settings Editor (see @c editSetting) immediately before a
 * value-entry @c readlineWithResize call and cleared afterwards. When non-null
 * and non-empty, @c setting_value_completion_entry consumes it as the match
 * source. Using a pointer keeps switching per setting cheap.
 */
inline std::vector<std::string>* g_activeSettingValues;

/**
 * @brief Completion entry point for Settings Editor value prompts.
 *
 * Sets @c rl_attempted_completion_over and a single-space append character,
 * then delegates to @c rl_completion_matches over @c g_activeSettingValues.
 * Returns nullptr (leaving Readline's default completion in place) when the
 * active list is null or empty.
 *
 * @param text  The partial value text.
 * @param start Start index in the line buffer (unused).
 * @param end   End index in the line buffer (unused).
 * @return Array of matches, or nullptr.
 */
char** setting_value_completion_entry(const char* text, int start, int end);

/**
 * @namespace SettingsEditorRepaint
 * @brief Body-repaint hook for the Settings Editor's readline prompts.
 *
 * The completion subsystem's <kbd>Esc</kbd> / <kbd>Ctrl+L</kbd> path
 * (@c clear_screen_and_buffer) normally only clears the terminal and redraws
 * the readline prompt line. On the Settings Editor screens that is not enough:
 * the header, current value, description and navigation footer are printed by
 * the editor's own draw lambdas (passed to @c readlineWithResize as the resize
 * repaint callback), so a bare clear would leave only the "Value …" prompt
 * visible after dismissing a Tab completion list.
 *
 * While a @c readlineWithResize call in the Settings Editor is active, the
 * draw lambda that was supplied as its repaint callback is stored in
 * @c g_fn. @c clear_screen_and_buffer and the editor's own Esc handler
 * (@c settingsEscHandler, file-local to settingsEditor.cpp) consult it before
 * clearing and — when set — use it to repaint the whole screen body instead of
 * just clearing.
 *
 * The hook is installed and removed around each Settings Editor
 * @c readlineWithResize call by @c editSetting(), so it is only ever live on
 * those screens. Because the variable and accessors are @c inline, every
 * translation unit shares the same object without an out-of-line definition.
 */
namespace SettingsEditorRepaint {
    /// @brief Active body-repaint callback; empty when no editor prompt is live.
    inline std::function<void()> g_fn;

    /// @brief Installs (or, with an empty function, clears) the hook.
    inline void set(std::function<void()> fn) { g_fn = std::move(fn); }

    /// @brief Returns the active hook; empty when none is installed.
    inline const std::function<void()>& get() { return g_fn; }
}

/** Hook for reloading readline async style. */
int checkPendingRefresh();

/** @brief Clears readline list completions on exact matches. */
int my_rl_complete(int ignore, int invoking_key);

/**
 * KEYBINDING MANAGEMENT
 * Functions to toggle between "Hotkeys" and "Standard Typing" modes.
 */

// --- Restore Prompt After Completions ---
void RestoreReadlineBuffer();

//--- Arms a one-shot readline startup hook that restores stashed input ---
void armPendingInputRestore();

// --- Exit Handler ---
int exit_handler(int, int);
// --- Help Handler ---
int help_handler(int, int);

// --- File Selection Mode ---
void setup_custom_keybindingsForSelect(void);
void reset_custom_keybindingsForSelect(void);

// --- Settings Editor Mode ---
void setup_custom_keybindingsForSettingsEditor(void);
void reset_custom_keybindingsForSettingsEditor(void);

// --- File Operations Mode (Cp/Mv/USB) ---
void reset_custom_keybindingsForCpMvWrite2Usb(void);
// --- File Operations Mode (RM) ---
void reset_custom_keybindingsForRm(void);

// --- Search Prompts ---
void setup_custom_keybindingsForSearches(void);
void reset_custom_keybindingsForSearches(void);

/**
 * READLINE COMPLETION SUBSYSTEM
 * Declarations for the pagination, settings-value completion, and completion
 * listing functions implemented in readline_completion.cpp.
 */

/**
 * @brief Resets the completion-list pagination state.
 *
 * Sets the current page back to 0 and clears the cached common prefix used to
 * detect the start of a new completion session. Invoked by @c completion_cb at
 * the beginning of each completion attempt, and after a screen wipe via
 * @c clear_screen_and_buffer and @c my_rl_complete. It does not touch any
 * Settings Editor state — no such flag exists in the completion TU.
 */
void resetReadlinePagination();

/**
 * @brief Enables context-aware Tab completion for a Settings Editor value.
 *
 * Populates the candidate list from the setting's allowed values and, when
 * candidates exist, installs the value-completion entry point
 * (@c setting_value_completion_entry) and the shared listing display hook
 * (@c customListingsFunction), binds <kbd>Tab</kbd> to @c my_rl_complete, and
 * resets pagination so the listing starts on page 1.
 *
 * The Settings Editor listing is labelled "Valid Completion Matches" and uses
 * the settings palette; <kbd>Esc</kbd> is deliberately @b not rebound to
 * @c clear_screen_and_buffer on these prompts, so the editor's own Esc handler
 * can dismiss the listing and repaint the body.
 *
 * Must be paired with @c endSettingsValueCompletion() on every exit path; the
 * @c SettingsCompletionGuard RAII type in settingsEditor.cpp does this.
 *
 * @note Implemented in settingsEditor.cpp, not in the completion TU.
 *
 * @param key Configuration key whose value is being edited. Keys without a
 *            fixed candidate set (numeric limits, free-form strings) produce
 *            an empty list and leave completion disabled.
 */
void beginSettingsValueCompletion(const std::string& key);

/**
 * @brief Restores the completion callbacks and Tab binding after value input.
 *
 * Reverses everything @c beginSettingsValueCompletion installed: restores the
 * previously-active attempted-completion, display, and Tab functions, clears
 * the candidate list, and resets pagination/listing state. Safe to call when
 * no settings completion is active (it is a no-op then).
 *
 * @note Implemented in settingsEditor.cpp, not in the completion TU.
 */
void endSettingsValueCompletion();

/**
 * @brief Repaints the last displayed completion listing after a terminal resize,
 *        or clears the saved snapshot if the input buffer has changed.
 */
void repaintCompletionListing();

#endif // READLINE_H
