// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks separate line editor histories, persistence, deduplication and limits.
 */

#include <stdio.h>
#include "../common/testHarness.h"

import isocmd.input.lineEditor;
import isocmd.app.main;
import isocmd.core.paths;
import isocmd.input.history;

namespace isocmd_tests {

	void historyPersistence() {

		const char* home = std::getenv("HOME"), *sandbox = std::getenv("ISOCMD_TEST_SANDBOX");
		CHECK(home && sandbox && fs::path(home) == fs::path(sandbox)/"home");
		const fs::path path = GlobalState::historyFilePath, filter = GlobalState::filterHistoryFilePath;
		CHECK(path.parent_path() == fs::path(home)/".local/share/isocmd/database");
		CHECK(filter.parent_path() == path.parent_path());
		struct Restore { int paths = GlobalState::MAX_HISTORY_LINES, patterns = GlobalState::MAX_HISTORY_PATTERN_LINES;

			~Restore() {

				editorClearHistory(); GlobalState::MAX_HISTORY_LINES = paths; GlobalState::MAX_HISTORY_PATTERN_LINES = patterns;

			}
		} restore;
		editorClearHistory();
		bool mode = false; GlobalState::MAX_HISTORY_LINES = 2;
		CHECK(isHistoryFileEmpty(path.string()));
		editorAddHistory("/first"); editorAddHistory("/second"); editorAddHistory("/third"); editorAddHistory("/second");
		saveHistory(mode); CHECK(readFile(path) == "/third\n/second\n");
		editorClearHistory(); editorAddHistory("stale"); loadHistory(mode);
		CHECK(editorHistoryList().size() == 2 && std::string(editorHistoryList()[0]) == "/third" && std::string(editorHistoryList()[1]) == "/second");
		CHECK(!isHistoryFileEmpty(path.string()));
		mode = true; GlobalState::MAX_HISTORY_PATTERN_LINES = 1;
		editorClearHistory(); editorAddHistory("foo"); editorAddHistory("bar"); saveHistory(mode);
		CHECK(readFile(filter) == "bar\n" && readFile(path) == "/third\n/second\n");
		loadHistory(mode); CHECK(editorHistoryList().size() == 1 && std::string(editorHistoryList()[0]) == "bar");
		GlobalState::MAX_HISTORY_PATTERN_LINES = 0; editorAddHistory("ignored"); saveHistory(mode);
		CHECK(editorHistoryList().size() == 0 && readFile(filter) == "bar\n");
		editorAddHistory("ignored"); loadHistory(mode); CHECK(editorHistoryList().size() == 0);
		mode = false; writeFile(path, " \nrelative\n /leading-space\n"); CHECK(isHistoryFileEmpty(path.string()));
		editorClearHistory(); saveHistory(mode); CHECK(readFile(path).empty());
		fs::remove(path); fs::remove(filter); editorAddHistory("stale"); loadHistory(mode); CHECK(editorHistoryList().size() == 0);

	}
}
