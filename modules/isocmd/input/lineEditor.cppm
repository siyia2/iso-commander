// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stdio.h>

#ifdef ISOCMD_EDITOR_STANDALONE
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#endif

export module isocmd.input.lineEditor;

#ifndef ISOCMD_EDITOR_STANDALONE
import std;
#endif

export extern "C++" {

	// Editor callback types
	using EditorCommand = int(int, int);
	using EditorHook = int();
	using EditorRedisplay = void();
	using EditorCompletion = std::vector<std::string>(std::string_view, int, int);
	using EditorInputReader = int(FILE*);
	using EditorCompletionDisplay = void(const std::vector<std::string>&, int, int);

	// History management
	void editorClearHistory();
	void editorAddHistory(std::string_view);
	const std::vector<std::string>& editorHistoryList();

	// Borrowed views, valid until the next buffer/prompt mutation or session exit.
	extern const char* editorLineBuffer;
	extern const char* editorPrompt;
	extern const char* editorDisplayPrompt;

	// Editing state
	extern int editorCursor, editorBufferEnd, editorSelectionMark, editorDone;
	extern int editorCatchSigwinch;

	// Completion configuration
	extern int editorCompletionAppendCharacter, editorCompletionHandled;
	extern const char* editorWordBreakCharacters;

	// Input and output configuration
	extern FILE* editorOutputStream;
	extern EditorInputReader* editorInputReader;

	// Editor hooks and command tracking
	extern EditorHook *editorEventHook, *editorStartupHook;
	/// Optional resize callback, invoked on the editor thread.
	extern EditorRedisplay* editorResizeHook;
	/// Borrowed nonblocking resize notification pipe; -1 selects timer fallback.
	extern int editorResizeWakeFd;
	extern EditorRedisplay* editorRedisplayHook;
	extern EditorCompletion* editorCompletionHook;
	extern EditorCompletionDisplay* editorCompletionDisplayHook;
	extern EditorCommand* editorLastCommand;

	/**
	 * @brief Run an interactive line-editing session.
	 * @param prompt Prompt displayed before the input buffer.
	 * @return Submitted input, or std::nullopt on EOF or an active session.
	 */
	std::optional<std::string> editLine(std::string_view prompt);

	/**
	 * @brief Initialize UTF-8 character decoding.
	 * @note Call at process startup, before creating worker threads.
	 */
	void editorInitializeLocale();

	// Editor configuration
	void editorSetEscapeTimeout(int milliseconds);
	void editorSetBracketedPaste(bool enabled);

	// Keybinding management
	int editorBindKey(int, EditorCommand*);
	int editorBindKeySequence(std::string_view, EditorCommand*);
	EditorCommand* editorKeySequenceHandler(std::string_view);

	// Input and buffer manipulation
	int editorReadByte(FILE*);
	int editorInsertKey(int, int);
	void editorInsertText(std::string_view);
	void editorReplaceLine(std::string_view);
	std::string editorCopyText(int, int);
	int editorSelectionActive();
	int editorSubmit(int, int);

	// History navigation
	int editorPreviousHistory(int, int);
	int editorNextHistory(int, int);

	// Prompt rendering and terminal dimensions
	void editorSetPrompt(std::string_view);
	void editorSetScreenSize(int, int);
	void editorRedisplay();
	int editorForcedUpdateDisplay();

	/**
	 * @brief Sort and deduplicate completion candidates.
	 *
	 * When multiple candidates remain, the first element contains
	 * their common prefix, followed by the individual matches.
	 *
	 * @return Normalized completion candidates.
	 */
	std::vector<std::string> editorCompletionMatches(std::vector<std::string>);

	// Completion commands
	int editorCompleteInternal(int);
	int editorComplete(int, int);

	/**
	 * @brief Query the terminal cursor row without losing user keystrokes.
	 * @return One-based cursor row, or zero if unavailable.
	 */
	int editorQueryCursorRow();

	// Pending input management
	void editorDiscardPendingInput() noexcept;
	int editorTakePendingInput() noexcept;

} // extern "C++"
