// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>
#include <readline/readline.h>
#include "../common/testHarness.h"

import isocmd.ui.display.readlineResize;

namespace isocmd_tests {
namespace { int previousHook() { return 0; } }
int terminalChild() {
    try {
        rl_initialize();
        rl_event_hook=previousHook;
        const auto oldTab=rl_function_of_keyseq("\t",rl_get_keymap(),nullptr);
        const auto oldCatch=rl_catch_sigwinch;
        struct sigaction oldWinch{}, after{};
        CHECK(sigaction(SIGWINCH,nullptr,&oldWinch)==0);
        int repaints=0;
        auto line=readlineWithResize("TEST> ",[&]{ ++repaints; std::cout << "\nTEST-REPAINT\n" << std::flush; },true);
        CHECK(line && std::string_view(line.get())=="abXcdef"); CHECK(repaints>=4);
        CHECK(rl_event_hook==previousHook && rl_catch_sigwinch==oldCatch);
        CHECK(rl_function_of_keyseq("\t",rl_get_keymap(),nullptr)==oldTab);
        CHECK(sigaction(SIGWINCH,nullptr,&after)==0 && after.sa_handler==oldWinch.sa_handler);
        std::cout << "TEST-TERMINAL-PASS\n" << std::flush; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
}
