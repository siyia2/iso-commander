// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <readline/history.h>
#include "../common/testHarness.h"
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
        struct Restore { int paths=GlobalState::MAX_HISTORY_LINES, patterns=GlobalState::MAX_HISTORY_PATTERN_LINES;
            ~Restore() { clear_history(); GlobalState::MAX_HISTORY_LINES=paths; GlobalState::MAX_HISTORY_PATTERN_LINES=patterns; }
        } restore;
        using_history(); clear_history();
        bool mode=false; GlobalState::MAX_HISTORY_LINES=2;
        CHECK(isHistoryFileEmpty(path.string()));
        add_history("/first"); add_history("/second"); add_history("/third"); add_history("/second");
        saveHistory(mode); CHECK(readFile(path)=="/third\n/second\n");
        clear_history(); add_history("stale"); loadHistory(mode);
        CHECK(history_length==2 && std::string(history_list()[0]->line)=="/third" && std::string(history_list()[1]->line)=="/second");
        CHECK(!isHistoryFileEmpty(path.string()));
        mode=true; GlobalState::MAX_HISTORY_PATTERN_LINES=1;
        clear_history(); add_history("foo"); add_history("bar"); saveHistory(mode);
        CHECK(readFile(filter)=="bar\n" && readFile(path)=="/third\n/second\n");
        loadHistory(mode); CHECK(history_length==1 && std::string(history_list()[0]->line)=="bar");
        GlobalState::MAX_HISTORY_PATTERN_LINES=0; add_history("ignored"); saveHistory(mode);
        CHECK(history_length==0 && readFile(filter)=="bar\n");
        add_history("ignored"); loadHistory(mode); CHECK(history_length==0);
        mode=false; writeFile(path," \nrelative\n /leading-space\n"); CHECK(isHistoryFileEmpty(path.string()));
        clear_history(); saveHistory(mode); CHECK(readFile(path).empty());
        fs::remove(path); fs::remove(filter); add_history("stale"); loadHistory(mode); CHECK(history_length==0);
    }
}
