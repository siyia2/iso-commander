// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <signal.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "../common/testHarness.h"
#include "../common/terminalSync.h"

import isocmd.filter.filter;
import isocmd.core.sharedRefreshState;
import isocmd.core.uiState;
import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.ops.cpMvRm;
import isocmd.search.search;
import isocmd.select.selectShared;
import isocmd.settings.settings;
import isocmd.ui.display.readlineResize;
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
		using namespace RetainAndRestoreReadlineBuffer;
		using Binding = rl_command_func_t*;
		constexpr const char* up = "\033[A";
		constexpr const char* down = "\033[B";
		constexpr const char* pgUp = "\033[5~";
		constexpr const char* pgDown = "\033[6~";
		constexpr const char* esc = "\033";

		int sentinelKey(int, int) { return 0; }
		int sentinelHook() { return 0; }
		char** sentinelCompletion(const char*, int, int) { return nullptr; }
		void sentinelDisplay(char**, int, int) {}
		void sentinelSignal(int) {}
		struct Unwind {};

		Binding binding(const char* key) {

			int type = -1;
			auto fn = rl_function_of_keyseq(key, rl_get_keymap(), &type);
			// Esc is both a standalone binding and an escape-sequence prefix.
			if (type == ISKMAP) {
				const auto map = reinterpret_cast<Keymap>(fn);
				CHECK(map && map[ANYOTHERKEY].type == ISFUNC);
				return map[ANYOTHERKEY].function;
			}
			CHECK(type == ISFUNC);
			return fn;

		}

		void key(const char* sequence, Binding expected) {

			CHECK(binding(sequence) == expected);

		}

		void historyKeys() {

			key(pgUp, rl_named_function("previous-history"));
			key(pgDown, rl_named_function("next-history"));

		}

		void allExitPaths(auto body, auto after) {

			for (int path = 0; path < 3; ++path) {
				try { body(path); CHECK(path != 2); }
				catch (const Unwind&) { CHECK(path == 2); }
				after();
			}

		}

		// The actual guards are exercised on normal fallthrough, early return,
		// and C++ exception unwinding (never throw through Readline C callbacks).
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
				for (auto k : keys) key(k, rl_insert);
				historyKeys(); key(esc, exit_handler);
				ReadlineKeybindingGuard guard;
				checkSelectionBindings(commands);
				reset_custom_keybindingsForCpMvWrite2Usb();
				for (auto k : {"*", "/", "P", "C", "R", "~"}) key(k, rl_insert);
				key("?", help_handler);
				guard.restore(); checkSelectionBindings(commands);
				reset_custom_keybindingsForRm();
				for (auto k : keys) key(k, rl_insert);
				guard.restore(); checkSelectionBindings(commands);
				g_rl_pending_text = "selection input";
				if (exit == 1) return;
				leave(exit);
			}, [&] {
				for (auto k : keys) key(k, rl_insert);
				historyKeys(); key(esc, exit_handler);
				CHECK(g_rl_pending_text.empty());
			});
			setup_custom_keybindingsForSettingsEditor();
			key("r", reset_handler);
			key("?", help_handler); key(esc, exit_handler);
			reset_custom_keybindingsForSettingsEditor();
			key("r", rl_insert); key("?", rl_insert); key(esc, exit_handler);

		}

		void searchBindings() {

			for (int path = 0; path < 3; ++path) {
				rl_attempted_completion_function = sentinelCompletion;
				key("?", rl_insert);
				auto run = [&] {
					SearchReadlineGuard guard;
					setup_custom_keybindingsForSearches();
					rl_attempted_completion_function = my_special_completion_entry;
					RestoreReadlineBuffer();
					historyKeys(); key("?", help_handler); key(esc, exit_handler);
					key("\t", my_rl_complete); key("\f", clear_screen_and_buffer);
					CHECK(rl_attempted_completion_function == my_special_completion_entry);
					add_history("temporary search");
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(rl_attempted_completion_function == sentinelCompletion);
				key("\t", prevent_readline_keybindings); key("\f", rl_insert);
				key("?", rl_insert); key(esc, exit_handler); historyKeys();
				CHECK(history_length == 0);
			}

		}

		void operationBindings() {

			for (int path = 0; path < 3; ++path) {
				g_rl_complete_mode = 0;
				auto run = [&] {
					RlKeyBindGuard keys;
					RlCompleteModeGuard mode;
					RlStartupHookGuard startup;
					RestoreReadlineBuffer();
					rl_bind_keyseq("\\e", sentinelKey);
					rl_startup_hook = sentinelHook;
					g_rl_complete_mode = 1;
					key("\f", clear_screen_and_buffer); key("\t", my_rl_complete);
					key(esc, sentinelKey); CHECK(rl_startup_hook == sentinelHook);
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				key("\t", prevent_readline_keybindings); key("\f", prevent_readline_keybindings);
				key(esc, exit_handler);
				CHECK(!rl_startup_hook && g_rl_complete_mode == 0);
			}
			// Test form-feed independently: the other guard must not mask a failure.
			rl_bind_key('\t', sentinelKey);
			allExitPaths([](int exit) {
				RlFormFeedGuard guard;
				rl_bind_key('\f', sentinelKey);
				key("\f", sentinelKey);
				if (exit == 1) return;
				leave(exit);
			}, [] { key("\f", prevent_readline_keybindings); key("\t", sentinelKey); });
			g_rl_complete_mode = 1;
			{ RlCompleteModeGuard outer; g_rl_complete_mode = 0;
			  { RlCompleteModeGuard inner; g_rl_complete_mode = 1; }
			  CHECK(g_rl_complete_mode == 0); }
			CHECK(g_rl_complete_mode == 1); g_rl_complete_mode = 0;

		}

		void previewBindings() {

			Binding previewUp = nullptr, previewDown = nullptr;
			rl_voidfunc_t* previewDisplay = nullptr;
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
					LivePreviewReadlineGuard guard;
					configureLivePreview(ctx, files);
					if (!previewUp) {
						previewUp = binding(pgUp); previewDown = binding(pgDown);
						previewDisplay = rl_redisplay_function;
					}
					CHECK(previewUp != pgup_handler && previewUp != rl_get_previous_history);
					CHECK(previewDown != pgdn_handler && previewDown != rl_get_next_history);
					CHECK(previewDisplay != rl_redisplay);
					key(pgUp, previewUp); key(pgDown, previewDown);
					CHECK(rl_redisplay_function == previewDisplay);
					resetLivePreviewIteration();
					CHECK(repaintLivePreviewOnResizeIfConfigured() && livePreviewWasRepainted());
					CHECK(GlobalState::ITEMS_PER_PAGE == pageSize && filteringStack.empty());
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				historyKeys(); CHECK(rl_redisplay_function == rl_redisplay);
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

			key("\t", rl_complete); key("\f", clear_screen_and_buffer);
			key(up, rl_get_previous_history); key(down, rl_get_next_history); historyKeys();
			CHECK(rl_attempted_completion_function == completion_cb);

		}

		void mappingBindings() {

			for (int path = 0; path < 3; ++path) {
				const TerminalSnapshot before;
				auto run = [&] {
					TerminalStateGuard mapping;
					mappingKeys();
					const auto display = rl_completion_display_matches_hook;
					CHECK(display && display != customListingsFunction);
					allExitPaths([&](int exit) {
						ConfirmationModeGuard confirm;
						for (auto k : {"\t", "\f", up, down, pgUp, pgDown}) key(k, prevent_readline_keybindings);
						CHECK(rl_attempted_completion_function == completion_cb);
						CHECK(rl_completion_display_matches_hook == display);
						if (exit == 1) return;
						leave(exit);
					}, [&] { mappingKeys(); CHECK(rl_completion_display_matches_hook == display); });
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(!rl_attempted_completion_function);
				CHECK(rl_completion_display_matches_hook == customListingsFunction);
				key("\t", prevent_readline_keybindings); key("\f", prevent_readline_keybindings);
				key(up, rl_get_previous_history); key(down, rl_get_next_history); historyKeys();
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

			rl_event_hook = sentinelHook;
			const TerminalSnapshot before;
			for (int path = 0; path < 3; ++path) {
				auto run = [&] {
					ListResizeWatcher watcher([] {});
					CHECK(rl_event_hook != sentinelHook && rl_event_hook != nullptr);
					CHECK(TerminalSnapshot{}.resize.sa_flags & SA_SIGINFO);
					if (path == 1) return;
					leave(path);
				};
				try { run(); CHECK(path != 2); } catch (const Unwind&) { CHECK(path == 2); }
				CHECK(rl_event_hook == sentinelHook); sameTerminal(before, TerminalSnapshot{});
			}

		}

		bool cancellationStateObserved = false;

		int observeCancellation() {

			cancellationStateObserved = rl_attempted_completion_function == nullptr &&
			    rl_catch_sigwinch == 0 && rl_event_hook != sentinelHook &&
			    rl_redisplay_function != rl_redisplay &&
			    rl_function_of_keyseq("\t", rl_get_keymap(), nullptr) != sentinelKey;
			return 0;

		}

		void cancellationPrompt(bool eof) {

			rl_bind_key('\t', sentinelKey);
			rl_attempted_completion_function = sentinelCompletion;
			rl_completion_display_matches_hook = sentinelDisplay;
			rl_event_hook = sentinelHook;
			rl_startup_hook = observeCancellation;
			cancellationStateObserved = false;
			rl_redisplay_function = rl_redisplay;
			const auto catchWinch = rl_catch_sigwinch;
			const TerminalSnapshot before;
			auto line = readlineWithResize(eof ? "RAII-EOF> " : "RAII-ESC> ", [] {}, true);
			if (eof) { CHECK(!line); }
			else { CHECK(line && std::string_view(line.get()) == "\033"); }
			key("\t", sentinelKey);
			CHECK(rl_attempted_completion_function == sentinelCompletion);
			CHECK(cancellationStateObserved);
			CHECK(rl_completion_display_matches_hook == sentinelDisplay && rl_startup_hook == observeCancellation);
			CHECK(rl_event_hook == sentinelHook && rl_redisplay_function == rl_redisplay);
			CHECK(rl_catch_sigwinch == catchWinch);
			sameTerminal(before, TerminalSnapshot{});
			rl_startup_hook = nullptr;

		}
		int settingsStage = 0;
		Binding numericTab = nullptr;
		std::string settingsFailure;

		unsigned long lastSettingsSession = 0;

		void checkSettingsValue() {

			key("\f", clear_list_and_screen);
			key(pgUp, pgup_handler); key(pgDown, pgdn_handler);
			CHECK(g_rl_complete_mode == 0);
			if (settingsStage == 1) {
				CHECK(rl_attempted_completion_function == setting_value_completion_entry);
				key("\t", my_rl_complete);
			} else {
				CHECK(!rl_attempted_completion_function);
				numericTab = binding("\t");
				CHECK(numericTab && numericTab != my_rl_complete && numericTab != rl_complete);
			}

		}

		void checkSettingsMenu() {

			CHECK(!g_activeSettingValues && !rl_attempted_completion_function);
			CHECK(g_rl_complete_mode == 1);
			historyKeys(); key("\f", prevent_readline_keybindings);
			key("?", settingsStage == 4 ? rl_insert : help_handler);
			key("r", settingsStage == 4 ? rl_insert : reset_handler);
			if (numericTab) key("\t", numericTab);

		}

		int observeSettings() {

			const auto session = terminal_sync::readlineSession();
			if (session == lastSettingsSession) return 0;
			lastSettingsSession = session;
			try {
				CHECK(settingsStage < 6);
				CHECK(rl_completion_display_matches_hook == customListingsFunction);
				CHECK(!rl_startup_hook && SettingsEditorRepaint::get());
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
			restoreReadline();
			g_rl_complete_mode = 1;
			lastSettingsSession = terminal_sync::readlineSession();
			rl_event_hook = observeSettings;
			interactiveConfigEditor(config.string());
			if (!settingsFailure.empty()) throw std::runtime_error(settingsFailure);
			CHECK(settingsStage == 6);
			CHECK(!g_activeSettingValues && !rl_attempted_completion_function);
			CHECK(rl_completion_display_matches_hook == customListingsFunction);
			CHECK(!rl_startup_hook && g_rl_pending_text.empty() && g_rl_complete_mode == 1);
			CHECK(!SettingsEditorRepaint::get());
			key("r", rl_insert); key("?", rl_insert); key(esc, exit_handler);
			historyKeys(); key("\t", numericTab); key("\f", prevent_readline_keybindings);
			CHECK(rl_event_hook == observeSettings);
			g_rl_complete_mode = 0;
			rl_event_hook = nullptr;

		}

	}

	int readlineGuardsChild() {

		try {
			rl_initialize();
			rl_set_keymap(rl_get_keymap_by_name("emacs"));
			rl_variable_bind("keyseq-timeout", "10");
			using_history(); clear_history();
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
