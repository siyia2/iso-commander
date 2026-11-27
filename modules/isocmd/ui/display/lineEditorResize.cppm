// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.display.lineEditorResize;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @brief Repaints a screen when the terminal size changes while a Line editor
 *        prompt waits.
 *
 * Construct it immediately before @c editLine() and let it go out of scope
 * immediately after (or use @ref editLineWithResize).
 *
 * While it exists:
 * - a @c SIGWINCH handler is installed;
 * - an @c editorEventHook checks for terminal-size changes;
 * - the line editor's screen dimensions are updated;
 * - the repaint callback redraws the screen;
 * - the line editor restores the prompt and any half-typed input.
 *
 * All terminal and Line editor access stays on the calling thread.
 *
 * @note Only one watcher may be active at a time: the event hook and
 *       the @c SIGWINCH handler are process-global.
 *
 * @see editLineWithResize
 */
class ListResizeWatcher {
public:
	/// Redraws the screen (see the callback contract above).
	using Repaint = std::function<void()>;

	/// Implementation detail; public only so the event-hook trampoline can name it.
	struct Impl;

	/**
	 * @brief Installs the @c SIGWINCH handler and the chained event hook.
	 * @param repaint Repaint callback.
	 */
	explicit ListResizeWatcher(Repaint repaint);

	/// Calls @ref stop.
	~ListResizeWatcher();

	ListResizeWatcher(const ListResizeWatcher&) = delete;
	ListResizeWatcher& operator=(const ListResizeWatcher&) = delete;

	/**
	 * @brief Restores the previous SIGWINCH handler and event hook.
	 *
	 * Safe to call more than once.
	 */
	void stop();

private:
	std::unique_ptr<Impl> impl_;
};

/// Draw only the prompt/input row, without invoking a live-preview hook.
/// Falls back to native Line editor when no fixed prompt is active.
void redisplayPrompt();

/// A submitted line, or no value on EOF.
using EditorLine = std::optional<std::string>;

/**
 * @brief Wraps @c editLine() with terminal resize handling and optional
 *        Tab-completion control.
 *
 * The prompt remains visible while the editable input scrolls horizontally,
 * preserving the surrounding UI and the complete input buffer. Existing
 * custom redisplay hooks (such as the live Filter preview) can retain control
 * of list repainting and delegate the input row to @c redisplayPrompt().
 *
 * @param prompt               Prompt passed to @c editLine().
 * @param repaint              Callback invoked on terminal resize; pass
 *                             nullptr to disable resize handling.
 * @param disableTabCompletion When true, suppresses Tab completion for the
 *                             duration of the call.
 *
 * @return The line read, or null on EOF (Ctrl+D).
 */
EditorLine editLineWithResize(
    const std::string& prompt,
    ListResizeWatcher::Repaint repaint = nullptr,
    bool disableTabCompletion = false);

} // extern "C++"
