// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>
#include <readline/readline.h>
#include "../common/testHarness.h"

import isocmd.ui.display.readlineResize;

namespace isocmd_tests {
	namespace {
		int previousHook() {

			return 0;

		}

		void longInputPrompt() {

			// Simulate an inputrc that prefers wrapping. The application must
			// enforce its own single-row invariant, even in this case.
			rl_variable_bind("horizontal-scroll-mode", "off");
			rl_variable_bind("enable-bracketed-paste", "on");
			const std::string prompt = "HEADER\n\001\033[32m\002LONG> \001\033[0m\002";
			auto line = readlineWithResize(prompt, [] {
				std::cout << "\033[H\033[2JLONG-REPAINT\nHEADER" << std::flush;
			});
			const std::string expected = "XSTART-OF-INPUT" + std::string(160, 'a') +
			    "TYPED-END!" + std::string(2400, 'b') + "PASTE-END@" +
			    "\nmultiline\nPASTE-FINAL#";
			CHECK(line && std::string_view(line.get()) == expected);
			CHECK(std::string_view(rl_variable_value("horizontal-scroll-mode")) == "on");
			std::cout << "TEST-LONG-INPUT-PASS\n" << std::flush;

		}

		void overflowSpacePrompt() {

			const std::string prompt = "\001\033[32m\002Display verbose output? (y/n): \001\033[0m\002";
			auto line = readlineWithResize(prompt);
			const std::string expected = "X" + std::string(160, 'a') + " " + std::string(84, 'b');
			CHECK(line && std::string_view(line.get()) == expected);
			std::cout << "TEST-OVERFLOW-SPACE-PASS\n" << std::flush;

		}

	}

	int terminalChild() {

		try {
			rl_initialize();
			rl_event_hook=previousHook;
			const auto oldTab=rl_function_of_keyseq("\t",rl_get_keymap(),nullptr);
			const auto oldCatch=rl_catch_sigwinch;
			const auto oldRedisplay=rl_redisplay_function;
			struct sigaction oldWinch{}, after{};
			CHECK(sigaction(SIGWINCH,nullptr,&oldWinch)==0);
			int repaints=0;
			auto line=readlineWithResize("TEST> ",[&]{ ++repaints; std::cout << "\nTEST-REPAINT\n" << std::flush; },true);
			CHECK(line && std::string_view(line.get())=="abXcdef"); CHECK(repaints>=4);
			CHECK(rl_event_hook==previousHook && rl_catch_sigwinch==oldCatch);
			CHECK(rl_redisplay_function==oldRedisplay);
			CHECK(rl_function_of_keyseq("\t",rl_get_keymap(),nullptr)==oldTab);
			CHECK(sigaction(SIGWINCH,nullptr,&after)==0 && after.sa_handler==oldWinch.sa_handler);
			std::cout << "TEST-TERMINAL-PASS\n" << std::flush;
			longInputPrompt();
			overflowSpacePrompt();
			CHECK(rl_redisplay_function==oldRedisplay);
			return 0;
		} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

	}
}
