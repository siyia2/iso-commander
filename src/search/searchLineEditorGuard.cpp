// SPDX-License-Identifier: GPL-3.0-or-later

// Restores line-editor session state when a directory-search prompt ends.

#include <stdio.h>

import isocmd.input.lineEditor;
import isocmd.input.inputHandling;
import isocmd.input.promptCompletion;
import isocmd.search.search;

SearchLineEditorGuard::SearchLineEditorGuard()
    : oldCompletion_(editorCompletionHook) {}

SearchLineEditorGuard::~SearchLineEditorGuard() {

	editorCompletionHook = oldCompletion_;
	editorBindKey('\t', ignoreEditorKeybinding);
	editorBindKey('\f', editorInsertKey);
	editorClearHistory();
	resetLineEditorPagination();
	reset_custom_keybindingsForSearches();

}
