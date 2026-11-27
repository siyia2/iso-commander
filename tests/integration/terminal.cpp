// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Provides child prompts for PTY input, resize and restoration tests.
 */

#include <stdio.h>
#include <signal.h>
#include "../common/testHarness.h"

import isocmd.input.lineEditor;
import isocmd.ui.display.lineEditorResize;

namespace isocmd_tests {

	namespace {
		int previousHook() {

			return 0;

		}

		void longInputPrompt() {

			// Exercise the fixed-row renderer with bracketed paste.
			editorSetBracketedPaste(true);
			const std::string prompt = "HEADER\n\001\033[32m\002LONG> \001\033[0m\002";
			auto line = editLineWithResize(prompt, [] {
				std::cout << "\033[H\033[2JLONG-REPAINT\nHEADER" << std::flush;
			});
			const std::string expected = "XSTART-OF-INPUT" + std::string(160, 'a') +
			    "TYPED-END!" + std::string(2400, 'b') + "PASTE-END@" +
			    "\nmultiline\nPASTE-FINAL#";
			CHECK(line && std::string_view(*line) == expected);

			std::cout << "TEST-LONG-INPUT-PASS\n" << std::flush;

		}

		void overflowSpacePrompt() {

			const std::string prompt = "\001\033[32m\002" + std::string(29, 'p') + "> \001\033[0m\002";
			auto line = editLineWithResize(prompt);
			const std::string expected = "X" + std::string(160, 'a') + " " + std::string(84, 'b');
			CHECK(line && std::string_view(*line) == expected);
			std::cout << "TEST-OVERFLOW-SPACE-PASS\n" << std::flush;

		}

	}

	int terminalChild() {

		try {
			editorEventHook = previousHook;
			const auto oldTab = editorKeySequenceHandler("\t");
			const auto oldCatch = editorCatchSigwinch;
			const auto oldRedisplay = editorRedisplayHook;
			struct sigaction oldWinch{}, after{};
			CHECK(sigaction(SIGWINCH, nullptr, &oldWinch) == 0);
			int repaints = 0;
			auto line = editLineWithResize("TEST> ", [&]{ ++repaints; std::cout << "\nTEST-REPAINT\n" << std::flush; }, true);
			CHECK(line && std::string_view(*line) == "abXcdef"); CHECK(repaints >= 4);
			CHECK(editorEventHook == previousHook && editorCatchSigwinch == oldCatch);
			CHECK(editorRedisplayHook == oldRedisplay);
			CHECK(editorKeySequenceHandler("\t") == oldTab);
			CHECK(sigaction(SIGWINCH, nullptr, &after) == 0 && after.sa_handler == oldWinch.sa_handler);
			std::cout << "TEST-TERMINAL-PASS\n" << std::flush;
			longInputPrompt();
			overflowSpacePrompt();
			auto unicode = editLineWithResize("UNICODE ↵ Ω> ");
			CHECK(unicode && std::string_view(*unicode) == "é界");
			std::cout << "TEST-UNICODE-PASS\n" << std::flush;
			CHECK(editorRedisplayHook == oldRedisplay);
			return 0;
		} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

	}
}
