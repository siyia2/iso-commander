// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Provides deterministic shared progress-bar cancellation fixtures.
 */

#include <signal.h>
#include "../common/testHarness.h"

import isocmd.core.cancellation;
import isocmd.input.lineEditor;
import isocmd.input.terminalControl;
import isocmd.proc.process;
import isocmd.ui.display.themes;

namespace isocmd_tests {

	namespace {

		/// Changes worker counters after SIGINT, while the bar is still running.
		void interruptedProgress(bool trackBytes) {

			GlobalState::g_operationCancelled = false;
			setupSignalHandlerCancellations();
			std::atomic<std::size_t> completed{3}, failed{2}, skipped{1}, bytes{200};
			std::atomic<bool> done{false};
			bool verbose = true;
			std::jthread worker([&](std::stop_token stop) {
				while (!stop.stop_requested() && !GlobalState::g_operationCancelled.load()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (stop.stop_requested()) return;
				completed = 4;
				failed = 4;
				skipped = 2;
				bytes = 900;
				// Allow several redraws before cleanup finishes.
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				done.store(true, std::memory_order_release);
			});
			const std::string operation = trackBytes ? "c2iso" : "mount";
			const std::string header = "\nProcessing " + operation + "\n";
			std::cout << header << std::flush;
			displayProgressBarWithSize(trackBytes ? &bytes : nullptr, trackBytes ? 1000 : 0,
			                           &completed, &failed, 10, &done, &verbose, operation, header, &skipped);
			worker.join();
			CHECK(!verbose && GlobalState::g_operationCancelled);
			CHECK(completed == 4 && failed == 4 && skipped == 2 && bytes == 900);

		}

		/// Exercises completion before the first live frame, with and without cancellation.
		void immediateProgress(bool cancelled) {

			GlobalState::g_operationCancelled = cancelled;
			setupSignalHandlerCancellations();
			std::atomic<std::size_t> completed{cancelled ? 0u : 10u}, failed{cancelled ? 10u : 0u};
			std::atomic<bool> done{true};
			bool verbose = true;
			const std::string header = "\nProcessing umount\n";
			std::cout << header << std::flush;
			displayProgressBarWithSize(nullptr, 0, &completed, &failed, 10,
			                           &done, &verbose, "umount", header);
			CHECK(!verbose);

		}

		/// A no-op conversion must reach 100% without claiming successful work or bytes.
		void skippedProgress() {

			GlobalState::g_operationCancelled = false;
			setupSignalHandlerCancellations();
			std::atomic<std::size_t> completed{0}, failed{0}, skipped{10}, bytes{0};
			std::atomic<bool> done{true};
			bool verbose = true;
			const std::string header = "\nProcessing c2iso\n";
			std::cout << header << std::flush;
			displayProgressBarWithSize(&bytes, 1000, &completed, &failed, 10,
			                           &done, &verbose, "c2iso", header, &skipped);
			CHECK(!verbose);

		}

	}

	int sharedProgressChild() {

		try {
			getskin(); UI::Palette::updateDefaultColors();
			editorClearHistory();
			editorAddHistory("older history entry");
			editorAddHistory("recent history entry");
			interruptedProgress(false);
			interruptedProgress(true);
			immediateProgress(true);
			immediateProgress(false);
			skippedProgress();
			CHECK(editorHistoryList() == std::vector<std::string>{"older history entry", "recent history entry"});
			CHECK(editorKeySequenceHandler("\033[A") == editorPreviousHistory);
			CHECK(editorKeySequenceHandler("\033[B") == editorNextHistory);
			CHECK(editorKeySequenceHandler("\033[5~") == editorPreviousHistory);
			CHECK(editorKeySequenceHandler("\033[6~") == editorNextHistory);
			editorClearHistory();
			GlobalState::g_operationCancelled = false;
			std::cout << "TEST-SHARED-PROGRESS-PASS\n" << std::flush;
			return 0;
		} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

	}

} // namespace isocmd_tests
