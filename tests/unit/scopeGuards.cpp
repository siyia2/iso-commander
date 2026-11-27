// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks scope cleanup, nested atomic guards and selection-state restoration.
 */

#include "../common/testHarness.h"

import isocmd.core.uiState;
import isocmd.select.selectShared;
import isocmd.input.promptCompletion;
import isocmd.ops.cpMvRm;

namespace isocmd_tests {

	namespace {
		struct Unwind {};

		void changeFlag(std::atomic<bool>& flag, bool early, bool fail) {

			AtomicFlagGuard guard(flag);
			CHECK(flag.load());
			if (fail) throw Unwind{};
			if (early) return;
			CHECK(flag.load());

		}

		void leaveSelection(bool unmount, bool early, bool fail) {

			using namespace GlobalState;
			using namespace RetainAndRestoreLineEditorBuffer;
			const std::vector<std::string> paths{"/first.iso"};
			bool nowUnmounting = false;
			IsoListStateGuard listGuard(isAtISOList, [&] { return nowUnmounting; });
			select_shared::pending::IsoPendingScope pending(&paths);
			PendingLineEditorTextClearGuard textGuard;
			EditorCompletionModeGuard completeGuard;
			CHECK(g_isoPending.source == &paths && g_isoPending.paths.empty());
			g_isoPending.paths = paths;
			g_filteringIndicator = true;
			isAtISOList = false;
			g_editorPendingText = "stashed input";
			g_editorCompletionMode = 1;
			nowUnmounting = unmount; // destructor must check the current state
			if (fail) throw Unwind{};
			if (early) return;
			CHECK(g_isoPending.paths == paths);

		}
	}

	/**
	 * @brief Checks restoration on normal exit, early return, exceptions, nesting and release.
	 */
	void atomicScopeGuards() {

		std::atomic<bool> flag{false};
		changeFlag(flag, false, false); CHECK(!flag.load());
		changeFlag(flag, true, false); CHECK(!flag.load());
		try { changeFlag(flag, false, true); CHECK(false); }
		catch (const Unwind&) { CHECK(!flag.load()); }
		{
			AtomicFlagGuard outer(flag);
			CHECK(flag.load());
			{
				AtomicFlagGuard inner(flag, false);
				CHECK(!flag.load());
			}
			CHECK(flag.load());
			{
				AtomicFlagGuard inner(flag, false);
				inner.release();
			}
			CHECK(!flag.load());
		}
		CHECK(!flag.load());
		{
			AtomicFlagGuard keep(flag);
			keep.release();
		}
		CHECK(flag.load());
		{
			AtomicFlagGuard clear(flag, false);
			CHECK(!flag.load());
		}
		CHECK(flag.load());

	}

	/**
	 * @brief Checks selection-state cleanup on each exit path and unmount-sensitive list restoration.
	 */
	void selectionScopeGuards() {

		using namespace GlobalState;
		using namespace RetainAndRestoreLineEditorBuffer;
		for (bool unmount : {false, true}) {
			for (int exit = 0; exit < 3; ++exit) {
				isAtISOList = true;
				g_filteringIndicator = false;
				g_editorCompletionMode = 0;
				g_isoPending.paths = {"stale.iso"};
				try { leaveSelection(unmount, exit == 1, exit == 2); CHECK(exit != 2); }
				catch (const Unwind&) { CHECK(exit == 2); }
				CHECK(isAtISOList.load() == !unmount);
				CHECK(!g_filteringIndicator.load());
				CHECK(g_isoPending.source == nullptr && g_isoPending.paths.empty());
				CHECK(g_editorPendingText.empty() && g_editorCompletionMode == 0);
			}
		}
		// A list which was already inactive must remain inactive.
		isAtISOList = false;
		{ IsoListStateGuard guard(isAtISOList, [] { return false; }); isAtISOList = true; }
		CHECK(!isAtISOList.load());

	}
}
