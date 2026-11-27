// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.input.promptCompletion;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// Native C++ prompt callbacks.
// Bridge function for line-editor tab-completion
std::vector<std::string> my_special_completion_entry(std::string_view text, int start, int end);

// Callback invoked by the line editor for context-aware completions
std::vector<std::string> completion_cb(std::string_view text, int start, int end);

// Keybinding utility functions
int ignoreEditorKeybinding(int count, int key);
int clear_screen_and_buffer(int count, int key);

/**
 * @brief Clears the screen and dismisses any visible completion list.
 *
 * Drops the saved completion-repaint snapshot first, then delegates to
 * @c clear_screen_and_buffer. Use it (instead of @c clear_screen_and_buffer)
 * wherever a visible completion list must disappear while a body-repaint hook
 * (@c SettingsEditorRepaint::g_fn) is active: otherwise the hook's call to
 * @c repaintCompletionListing() would redraw the list that is being dismissed.
 *
 * Bound to <kbd>Esc</kbd> while a completion list is shown, and to
 * <kbd>Ctrl+L</kbd> on the Settings Editor's value prompt. The search prompt
 * keeps @c clear_screen_and_buffer on <kbd>Ctrl+L</kbd>.
 */
int clear_list_and_screen(int count, int key);

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

/** @brief Global access point for the completion data used by the line-editor callback. */
extern CompleterData g_completerData;

/**
 * @namespace RetainAndRestoreLineEditorBuffer
 * @brief Global state management for line-editor buffer persistence during completion cycles.
 */
namespace RetainAndRestoreLineEditorBuffer {
	/// Mode flag to determine if completion text should be persisted across line-editor calls.
	inline int g_editorCompletionMode = 0;
	/// Storage for the completed line text to be restored after a reset.
	extern std::string g_editorPendingText;
}

/// Clears pending line-editor text when the guard goes out of scope.
struct PendingLineEditorTextClearGuard {
	~PendingLineEditorTextClearGuard();
};

/**
 * @brief Active candidate list for Settings Editor value completion.
 *
 * Populated by the Settings Editor (see @c editSetting) immediately before a
 * value-entry @c editLineWithResize call and cleared afterwards. When non-null
 * and non-empty, @c setting_value_completion_entry consumes it as the match
 * source. Using a pointer keeps switching per setting cheap.
 */
inline std::vector<std::string>* g_activeSettingValues;

/**
 * @brief Completion entry point for Settings Editor value prompts.
 *
 * Sets @c editorCompletionHandled and a single-space append character,
 * then delegates to @c editorCompletionMatches over @c g_activeSettingValues.
 * Returns an empty vector (leaving the line editor's default completion in place) when the
 * active list is null or empty.
 *
 * @param text  The partial value text.
 * @param start Start index in the line buffer (unused).
 * @param end   End index in the line buffer (unused).
 * @return Vector of matches (empty if no match).
 */
std::vector<std::string> setting_value_completion_entry(std::string_view text, int start, int end);

/**
 * @namespace SettingsEditorRepaint
 * @brief Body-repaint hook for the Settings Editor's line editor prompts.
 *
 * The completion subsystem's <kbd>Esc</kbd> / <kbd>Ctrl+L</kbd> path
 * (@c clear_screen_and_buffer, reached through @c clear_list_and_screen)
 * normally only clears the terminal and redraws the line editor prompt line. On
 * the Settings Editor screens that is not enough: the header, current value,
 * description and navigation footer are printed by the editor's own draw
 * lambdas (passed to @c editLineWithRepaint, which also forwards them to
 * @c editLineWithResize as the resize repaint callback), so a bare clear would
 * leave only the "Value …" prompt visible after dismissing a Tab completion
 * list.
 *
 * While a @c editLineWithRepaint call in the Settings Editor is active, the
 * draw lambda that was supplied as its repaint callback is stored in
 * @c g_fn. @c clear_screen_and_buffer consults it before clearing and — when
 * set — uses it to repaint the whole screen body instead of just clearing.
 *
 * The hook is installed and removed around each Settings Editor prompt by
 * @c editLineWithRepaint (settingsEditor.cpp), so it is only ever live on
 * those screens. The callback and accessors are defined once in promptCompletion.cpp.
 *
 * @par Repaint callback contract
 * The stored callback is the same one the resize watcher runs, so it must end
 * ONE LINE SHORT of the prompt's input line (the caller of the hook prints the
 * final '\n'). If it repaints the completion listing, it must first step onto
 * the input line and back (see @c repaintCompletionListing). Because
 * @c clear_screen_and_buffer clears the listing snapshot only after the hook
 * returns, handlers that mean to dismiss a visible list must clear the
 * snapshot first (@c clear_list_and_screen).
 */
namespace SettingsEditorRepaint {
	/// Active body-repaint callback; empty when no editor prompt is live.
	extern std::function<void()> g_fn;

	/// Installs (or, with an empty function, clears) the hook.
	void set(std::function<void()> fn);

	/// Returns the active hook; empty when none is installed.
	const std::function<void()>& get();
}

/** Hook for reloading line editor async style. */
int checkPendingRefresh();

/** @brief Clears line editor list completions on exact matches. */
int completePrompt(int ignore, int invoking_key);

/**
 * KEYBINDING MANAGEMENT
 * Functions to toggle between "Hotkeys" and "Standard Typing" modes.
 */

// --- Restore Prompt After Completions ---
void RestoreLineEditorBuffer();

// --- Arms a one-shot line editor startup hook that restores stashed input ---
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
 * PROMPT COMPLETION SUBSYSTEM
 * Declarations for the pagination, settings-value completion, and completion
 * listing functions implemented in promptCompletion.cpp.
 */

/*
 * The completion-list pagination state and repaint snapshot are reset by
 * resetLineEditorPagination(), declared in inputHandling.cppm.
 */

// Legacy declarations; neither has a definition or call site in the current tree.
// Settings-value completion is configured by term::setupSettingLineEditor()
// and cleaned up by term::CompletionGuard in settingsEditor.cpp.
void beginSettingsValueCompletion(const std::string& key);
void endSettingsValueCompletion();

/**
 * @brief Repaints the last displayed completion listing after a terminal resize,
 *        or clears the saved snapshot if the input buffer has changed.
 *
 * The listing saves/restores an absolute cursor position (ESC[s / ESC[u), so
 * call this with the cursor ON the prompt's input line. Repaint callbacks that
 * end one line short of it (as the resize watcher requires) must step down
 * with "\n" before the call and back up with "\033[A" after it.
 */
void repaintCompletionListing();

} // extern "C++"
