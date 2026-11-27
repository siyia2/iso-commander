// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks native line editor guards, keybindings, hooks and terminal restoration.
 */

#include <stdio.h>
#include <signal.h>
#include <termios.h>
#include "../common/testHarness.h"
#include "../common/terminalSync.h"

import isocmd.input.lineEditor;
import isocmd.filter.filter;
import isocmd.core.sharedRefreshState;
import isocmd.core.uiState;
import isocmd.input.inputHandling;
import isocmd.input.terminalControl;
import isocmd.input.promptCompletion;
import isocmd.ops.cpMvRm;
import isocmd.search.search;
import isocmd.select.selectShared;
import isocmd.settings.settings;
import isocmd.ui.display.lineEditorResize;
import isocmd.ui.interface.pausePrompts;
import isocmd.write2usb.util.terminalGuards;

// These production handlers have external C++ linkage but are not part of
// the public module interface. Test their identity without exporting new APIs.
int toggleFlno_handler(int, int);
int filter_handler(int, int);
int proc_handler(int, int);
int clr_handler(int, int);
int refresh_handler(int, int);
int toggleList_handler(int, int);
int reset_handler(int, int);

namespace isocmd_tests {

	namespace {
		using namespace RetainAndRestoreLineEditorBuffer;
		using Binding = EditorCommand*;
		constexpr const char* up = "\033[A";
		constexpr const char* down = "\033[B";
		constexpr const char* pgUp = "\033[5~";
		constexpr const char* pgDown = "\033[6~";
		constexpr const char* esc = "\033";

		int sentinelKey(int, int) { return 0; }
		int sentinelHook() { return 0; }
		std::vector<std::string> sentinelCompletion(std::string_view, int, int) { return {}; }
		void sentinelDisplay(const std::vector<std::string>&, int, int) {}
		void sentinelSignal(int) {}
		struct Unwind {};

		Binding binding(const char* key) {

			return editorKeySequenceHandler(key);

		}

		void key(const char* sequence, Binding expected) {

			CHECK(binding(sequence) == expected);

		}

		void historyKeys() {

			key(pgUp, editorPreviousHistory);
			key(pgDown, editorNextHistory);

		}

		void allExitPaths(auto body, auto after) {

			for (int path = 0; path < 3; ++path) {
				try { body(path); CHECK(path != 2); }
				catch (const Unwind&) { CHECK(path == 2); }
				after();
			}

		}

		// The actual guards are exercised on normal fallthrough, early return,
		// and C++ exception unwinding (never throw through line editor C callbacks).
		void leave(int path) {

			if (path == 2) throw Unwind{};

		}

		void checkSelectionBindings(const std::array<Binding, 7>& commands) {

			key(pgUp, pgup_handler); key(pgDown, pgdn_handler);
			key(esc, exit_handler);
			const char* keys[] = {"*", "/", "P", "C", "R", "~", "?"};
			for (std::size_t i = 0; i < commands.size(); ++i) key(keys[i], commands[i]);

		}

		void selectionBindings() {

			const char* keys[] = {"*", "/", "P", "C", "R", "~", "?"};
			const std::array<Binding, 7> commands{toggleFlno_handler, filter_handler,
			    proc_handler, clr_handler, refresh_handler, toggleList_handler, help_handler};
			setup_custom_keybindingsForSelect();
			checkSelectionBindings(commands);
			reset_custom_keybindingsForSelect();
			allExitPaths([&](int exit) {
				for (auto k : keys) key(k, editorInsertKey);
				historyKeys(); key(esc, exit_handler);
				LineEditorKeybindingGuard guard;
				checkSelectionBindings(commands);
				reset_custom_keybindingsForCpMvWrite2Usb();
				for (auto k : {"*", "/", "P", "C", "R", "~"}) key(k, editorInsertKey);
				key("?", help_handler);
				guard.restore(); checkSelectionBindings(commands);
				reset_custom_keybindingsForRm();
				for (auto k : keys) key(k, editorInsertKey);
				guard.restore(); checkSelectionBindings(commands);
				g_editorPendingText = "selection input";
				if (exit == 1) return;
				leave(exit);
			}, [&] {
				for (auto k : keys) key(k, editorInsertKey);
				historyKeys(); key(esc, exit_handler);
				CHECK(g_editorPendingText.empty());
			});
			setup_custom_keybindingsForSettingsEditor();
			key("r", reset_handler);
			key("?", help_handler); key(esc, exit_handler);
			reset_custom_keybindingsForSettingsEditor();
			key("r", editorInsertKey); key("?", editorInsertKey); key(esc, exit_handler);

		}

		void searchBindings() {

			for (int path = 0; path < 3; ++path) {
				editorCompletionHook = sentinelCompletion;
				key("?", editorInsertKey);
				auto run = [&] {
					SearchLineEditorGuard guard;
					setup_custom_keybindingsForSearches();
					editorCompletionHook = my_special_completion_entry;
					RestoreLineEditorBuffer();
					historyKeys(); key("?", help_handler); key(esc, exit_handler);
					key("\t", completePrompt); key("\f", clear_screen_and_buffer);
					CHECK(editorCompletionHook == my_special_completion_entry);
					editorAddHistory("temporary search");
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(editorCompletionHook == sentinelCompletion);
				key("\t", ignoreEditorKeybinding); key("\f", editorInsertKey);
				key("?", editorInsertKey); key(esc, exit_handler); historyKeys();
				CHECK(editorHistoryList().size() == 0);
			}

		}

		void operationBindings() {

			for (int path = 0; path < 3; ++path) {
				g_editorCompletionMode = 0;
				auto run = [&] {
					EditorKeyBindGuard keys;
					EditorCompletionModeGuard mode;
					EditorStartupHookGuard startup;
					RestoreLineEditorBuffer();
					editorBindKeySequence("\033", sentinelKey);
					editorStartupHook = sentinelHook;
					g_editorCompletionMode = 1;
					key("\f", clear_screen_and_buffer); key("\t", completePrompt);
					key(esc, sentinelKey); CHECK(editorStartupHook == sentinelHook);
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				key("\t", ignoreEditorKeybinding); key("\f", ignoreEditorKeybinding);
				key(esc, exit_handler);
				CHECK(!editorStartupHook && g_editorCompletionMode == 0);
			}
			// Test form-feed independently: the other guard must not mask a failure.
			editorBindKey('\t', sentinelKey);
			allExitPaths([](int exit) {
				EditorFormFeedGuard guard;
				editorBindKey('\f', sentinelKey);
				key("\f", sentinelKey);
				if (exit == 1) return;
				leave(exit);
			}, [] { key("\f", ignoreEditorKeybinding); key("\t", sentinelKey); });
			g_editorCompletionMode = 1;
			{ EditorCompletionModeGuard outer; g_editorCompletionMode = 0;
			  { EditorCompletionModeGuard inner; g_editorCompletionMode = 1; }
			  CHECK(g_editorCompletionMode == 0); }
			CHECK(g_editorCompletionMode == 1); g_editorCompletionMode = 0;

		}

		void previewBindings() {

			Binding previewUp = nullptr, previewDown = nullptr;
			EditorRedisplay* previewDisplay = nullptr;
			for (int path = 0; path < 3; ++path) {
				std::weak_ptr<RefreshState> owner;
				const auto pageSize = GlobalState::ITEMS_PER_PAGE;
				auto run = [&] {
					std::vector<std::string> files{"/sample/alpha.iso"};
					auto state = std::make_shared<RefreshState>();
					owner = state;
					bool history = false, redraw = false;
					FilterContext ctx{files, state->isFiltered, redraw, history, state->currentPage};
					ctx.listType = "ISO_FILES"; ctx.listSubType = "cp";
					ctx.pendingIndices = &state->pendingIndices;
					ctx.hasPendingProcess = &state->hasPendingProcess;
					ctx.state = state;
					LivePreviewLineEditorGuard guard;
					configureLivePreview(ctx, files);
					if (!previewUp) {
						previewUp = binding(pgUp); previewDown = binding(pgDown);
						previewDisplay = editorRedisplayHook;
					}
					CHECK(previewUp != pgup_handler && previewUp != editorPreviousHistory);
					CHECK(previewDown != pgdn_handler && previewDown != editorNextHistory);
					CHECK(previewDisplay != editorRedisplay);
					key(pgUp, previewUp); key(pgDown, previewDown);
					CHECK(editorRedisplayHook == previewDisplay);
					resetLivePreviewIteration();
					CHECK(repaintLivePreviewOnResizeIfConfigured() && livePreviewWasRepainted());
					CHECK(GlobalState::ITEMS_PER_PAGE == pageSize && filteringStack.empty());
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				historyKeys(); CHECK(editorRedisplayHook == editorRedisplay);
				CHECK(owner.expired() && !livePreviewWasRepainted());
				CHECK(!repaintLivePreviewOnResizeIfConfigured());
			}

		}

		struct TerminalSnapshot {
			struct termios tty{};
			struct sigaction interrupt{}, resize{};
			sigset_t mask{};

			TerminalSnapshot() {
				CHECK(tcgetattr(STDIN_FILENO, &tty) == 0);
				CHECK(sigaction(SIGINT, nullptr, &interrupt) == 0);
				CHECK(sigaction(SIGWINCH, nullptr, &resize) == 0);
				CHECK(sigprocmask(SIG_SETMASK, nullptr, &mask) == 0);
			}
		};

		void sameMask(const sigset_t& a, const sigset_t& b) {

			for (int sig = 1; sig < NSIG; ++sig) CHECK(sigismember(&a, sig) == sigismember(&b, sig));

		}

		void sameAction(const struct sigaction& a, const struct sigaction& b) {

			CHECK(a.sa_flags == b.sa_flags);
			if (a.sa_flags & SA_SIGINFO) { CHECK(a.sa_sigaction == b.sa_sigaction); }
			else { CHECK(a.sa_handler == b.sa_handler); }
			sameMask(a.sa_mask, b.sa_mask);

		}

		void sameTerminal(const TerminalSnapshot& before, const TerminalSnapshot& after) {

			const auto& a = before.tty; const auto& b = after.tty;
			CHECK(a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag);
			CHECK(a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag);
			CHECK(std::equal(std::begin(a.c_cc), std::end(a.c_cc), std::begin(b.c_cc)));
			CHECK(cfgetispeed(&a) == cfgetispeed(&b) && cfgetospeed(&a) == cfgetospeed(&b));
			sameAction(before.interrupt, after.interrupt); sameAction(before.resize, after.resize);
			sameMask(before.mask, after.mask);

		}

		void mappingKeys() {

			key("\t", completeMappingPrompt); key("\f", clear_screen_and_buffer);
			key(up, editorPreviousHistory); key(down, editorNextHistory); historyKeys();
			CHECK(editorCompletionHook == completion_cb);

		}

		void mappingBindings() {

			for (int path = 0; path < 3; ++path) {
				const TerminalSnapshot before;
				auto run = [&] {
					TerminalStateGuard mapping;
					mappingKeys();
					const auto display = editorCompletionDisplayHook;
					CHECK(display && display != customListingsFunction);
					allExitPaths([&](int exit) {
						ConfirmationModeGuard confirm;
						for (auto k : {"\t", "\f", up, down, pgUp, pgDown}) key(k, ignoreEditorKeybinding);
						CHECK(editorCompletionHook == completion_cb);
						CHECK(editorCompletionDisplayHook == display);
						if (exit == 1) return;
						leave(exit);
					}, [&] { mappingKeys(); CHECK(editorCompletionDisplayHook == display); });
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(!editorCompletionHook);
				CHECK(editorCompletionDisplayHook == customListingsFunction);
				key("\t", ignoreEditorKeybinding); key("\f", ignoreEditorKeybinding);
				key(up, editorPreviousHistory); key(down, editorNextHistory); historyKeys();
				// The USB guard deliberately leaves SIGINT ignored and VEOF disabled.
				// Assert this documented policy, then restore the PTY test baseline.
				const TerminalSnapshot after;
				CHECK(after.interrupt.sa_handler == SIG_IGN && after.tty.c_cc[VEOF] == _POSIX_VDISABLE);
				CHECK(tcsetattr(STDIN_FILENO, TCSANOW, &before.tty) == 0);
				CHECK(sigaction(SIGINT, &before.interrupt, nullptr) == 0);
				sameTerminal(before, TerminalSnapshot{});
			}

		}

		void watcherUnwinding() {

			editorEventHook = sentinelHook;
			const auto oldResizeHook = editorResizeHook;
			const int oldWakeFd = editorResizeWakeFd;
			const auto sentinelResize = +[] {};
			editorResizeHook = sentinelResize;
			const TerminalSnapshot before;
			for (int path = 0; path < 3; ++path) {
				auto run = [&] {
					ListResizeWatcher watcher([] {});
					CHECK(editorEventHook == sentinelHook);
					CHECK(editorResizeHook != sentinelResize && editorResizeHook != nullptr);
					CHECK(editorResizeWakeFd >= 0);
					CHECK(TerminalSnapshot{}.resize.sa_flags & SA_SIGINFO);
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(editorEventHook == sentinelHook && editorResizeHook == sentinelResize);
				CHECK(editorResizeWakeFd == oldWakeFd);
				sameTerminal(before, TerminalSnapshot{});
			}
			editorResizeHook = oldResizeHook;

		}

		bool cancellationStateObserved = false;

		int observeCancellation() {

			cancellationStateObserved = editorCompletionHook == nullptr &&
			    editorCatchSigwinch == 0 && editorEventHook == sentinelHook && editorResizeHook != nullptr &&
			    editorRedisplayHook != editorRedisplay &&
			    editorKeySequenceHandler("\t") != sentinelKey;
			return 0;

		}

		void cancellationPrompt(bool eof) {

			editorBindKey('\t', sentinelKey);
			editorCompletionHook = sentinelCompletion;
			editorCompletionDisplayHook = sentinelDisplay;
			editorEventHook = sentinelHook;
			editorStartupHook = observeCancellation;
			cancellationStateObserved = false;
			editorRedisplayHook = editorRedisplay;
			const auto catchWinch = editorCatchSigwinch;
			const auto oldResizeHook = editorResizeHook;
			const int oldWakeFd = editorResizeWakeFd;
			const TerminalSnapshot before;
			auto line = editLineWithResize(eof ? "RAII-EOF> " : "RAII-ESC> ", [] {}, true);
			if (eof) { CHECK(!line); }
			else { CHECK(line && std::string_view(*line) == "\033"); }
			key("\t", sentinelKey);
			CHECK(editorCompletionHook == sentinelCompletion);
			CHECK(cancellationStateObserved);
			CHECK(editorCompletionDisplayHook == sentinelDisplay && editorStartupHook == observeCancellation);
			CHECK(editorEventHook == sentinelHook && editorRedisplayHook == editorRedisplay);
			CHECK(editorCatchSigwinch == catchWinch && editorResizeHook == oldResizeHook);
			CHECK(editorResizeWakeFd == oldWakeFd);
			sameTerminal(before, TerminalSnapshot{});
			editorStartupHook = nullptr;

		}
		int settingsStage = 0;
		Binding numericTab = nullptr;
		std::string settingsFailure;

		unsigned long lastSettingsSession = 0;

		void checkSettingsValue() {

			key("\f", clear_list_and_screen);
			key(pgUp, pgup_handler); key(pgDown, pgdn_handler);
			CHECK(g_editorCompletionMode == 0);
			if (settingsStage == 1) {
				CHECK(editorCompletionHook == setting_value_completion_entry);
				key("\t", completePrompt);
			} else {
				CHECK(!editorCompletionHook);
				numericTab = binding("\t");
				CHECK(numericTab && numericTab != completePrompt && numericTab != editorComplete);
			}

		}

		void checkSettingsMenu() {

			CHECK(!g_activeSettingValues && !editorCompletionHook);
			CHECK(g_editorCompletionMode == 1);
			historyKeys(); key("\f", ignoreEditorKeybinding);
			key("?", settingsStage == 4 ? editorInsertKey : help_handler);
			key("r", settingsStage == 4 ? editorInsertKey : reset_handler);
			if (numericTab) key("\t", numericTab);

		}

		int observeSettings() {

			const auto session = terminal_sync::lineEditorSession();
			if (session == lastSettingsSession) return 0;
			lastSettingsSession = session;
			try {
				CHECK(settingsStage < 6);
				CHECK(editorCompletionDisplayHook == customListingsFunction);
				CHECK(!editorStartupHook && SettingsEditorRepaint::get());
				key(esc, exit_handler);
				if (settingsStage == 1 || settingsStage == 2) {
					CHECK((g_activeSettingValues != nullptr) == (settingsStage == 1));
					checkSettingsValue();
				}
				else checkSettingsMenu();
			} catch (const std::exception& e) { if (settingsFailure.empty()) settingsFailure = e.what(); }
			std::cout << "RAII-SETTINGS-" << settingsStage++ << "-READY\n" << std::flush;
			return 0;

		}

		void settingsBindings() {

			TempDir temp;
			const auto config = temp.path / "config";
			writeFile(config, "pagination = 2\nfilenames_only = off\n");
			restoreLineEditor();
			g_editorCompletionMode = 1;
			lastSettingsSession = terminal_sync::lineEditorSession();
			editorEventHook = observeSettings;
			interactiveConfigEditor(config.string());
			if (!settingsFailure.empty()) throw std::runtime_error(settingsFailure);
			CHECK(settingsStage == 6);
			CHECK(!g_activeSettingValues && !editorCompletionHook);
			CHECK(editorCompletionDisplayHook == customListingsFunction);
			CHECK(!editorStartupHook && g_editorPendingText.empty() && g_editorCompletionMode == 1);
			CHECK(!SettingsEditorRepaint::get());
			key("r", editorInsertKey); key("?", editorInsertKey); key(esc, exit_handler);
			historyKeys(); key("\t", numericTab); key("\f", ignoreEditorKeybinding);
			CHECK(editorEventHook == observeSettings);
			g_editorCompletionMode = 0;
			editorEventHook = nullptr;

		}

	}

	/**
	 * @brief Exercises editor guards and terminal restoration in an isolated PTY child.
	 */
	int lineEditorGuardsChild() {

		try {
			editorClearHistory();
			enable_ctrl_d();
			struct sigaction action{};
			action.sa_handler = sentinelSignal;
			action.sa_flags = SA_RESTART;
			sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGUSR1);
			CHECK(sigaction(SIGWINCH, &action, nullptr) == 0);
			CHECK(sigaction(SIGINT, &action, nullptr) == 0);
			sigset_t blocked{};
			sigemptyset(&blocked); sigaddset(&blocked, SIGUSR2);
			CHECK(sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);
			selectionBindings(); searchBindings(); operationBindings(); previewBindings();
			mappingBindings(); watcherUnwinding();
			cancellationPrompt(false); cancellationPrompt(true);
			const TerminalSnapshot before;
			try {
				pressEnterToReturnWithRepaint([] { throw Unwind{}; });
				CHECK(false);
			} catch (const Unwind&) { sameTerminal(before, TerminalSnapshot{}); }
			pressEnterToReturnWithRepaint([] { std::cout << "RAII-PAUSE" << std::flush; });
			sameTerminal(before, TerminalSnapshot{});
			settingsBindings();
			std::cout << "RAII-GUARDS-PASS\n" << std::flush;
			return 0;
		} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

	}
}
