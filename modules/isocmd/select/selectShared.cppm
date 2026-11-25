// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>

export module isocmd.select.selectShared;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- select/selectShared ----
// Internal helpers shared by selectIso.cpp and selectImage.cpp: RAII guards
// and the path-based pending-selection logic. Not for use outside src/select/.

/// Pending state of the ISO select screen. Main-thread only.
struct IsoPending {
	std::vector<std::string>        paths;
	const std::vector<std::string>* source = nullptr;   // list the screen's source indices refer to
};

extern IsoPending g_isoPending;

// ======================================================================
// RAII Guards for Select Functions
// ======================================================================
//
// These guards manage terminal state, readline keybindings, atomic flags,
// and cursor position across the interactive TUI selection functions
// (selectForIsoFiles, selectForImageFiles). They eliminate manual cleanup
// calls and guarantee proper teardown on all exit paths, including early
// returns, loop breaks, and stack unwinding from exceptions.
// ======================================================================

/**
 * @brief Installs the select-mode readline keybindings on construction and
 *        resets them on destruction.
 *
 * The destructor also clears @c RetainAndRestoreReadlineBuffer::g_rl_pending_text,
 * so a pending prompt-text restore from a previous session cannot leak into
 * the caller. Call @ref restore after any sub-mode that replaces the selection
 * keybindings (e.g. the filter flow via @c runSharedFilterFlow) so the select
 * prompt keeps its own bindings.
 */
class ReadlineKeybindingGuard {
public:
	ReadlineKeybindingGuard();
	~ReadlineKeybindingGuard();

	/// Re-establish custom keybindings after a sub-mode overrode them.
	void restore();

	// Non-copyable, non-movable
	ReadlineKeybindingGuard(const ReadlineKeybindingGuard&) = delete;
	ReadlineKeybindingGuard& operator=(const ReadlineKeybindingGuard&) = delete;
};

/**
 * @brief Sets an @c std::atomic<bool> to @p activeValue on construction and
 *        restores the previous value on destruction (unless released).
 *
 * The previous value is captured at construction time, so nested guards and
 * pre-existing values round-trip correctly; the flag is not simply flipped.
 * Call @ref release to leave the flag at @p activeValue.
 */
class AtomicFlagGuard {
public:
	explicit AtomicFlagGuard(std::atomic<bool>& flag, bool activeValue = true)
	    : flag_(flag),
	      restoreValue_(flag.load(std::memory_order_acquire)),
	      released_(false) {

		flag_.store(activeValue, std::memory_order_release);

	}

	~AtomicFlagGuard() {

		if (!released_) {
			flag_.store(restoreValue_, std::memory_order_release);
		}

	}

	/// Leave the flag at @p activeValue on destruction instead of restoring.
	void release() {

		released_ = true;

	}

	AtomicFlagGuard(const AtomicFlagGuard&) = delete;
	AtomicFlagGuard& operator=(const AtomicFlagGuard&) = delete;

private:
	std::atomic<bool>& flag_;
	bool restoreValue_;
	bool released_;
};

/**
 * @brief Restores @c isAtISOList to its pre-scope value on destruction
 *        (unless the operation is an unmount) and always clears the global
 *        filtering indicator.
 *
 * The unmount exemption is per-instance and re-evaluated at destruction
 * time via @p isUnmountCheck, so a long-lived guard can observe a change in
 * whether the current operation is an unmount.
 *
 * The filtering indicator (@c GlobalState::g_filteringIndicator) is cleared
 * unconditionally, regardless of whether the flag above was restored, so a
 * failed filter cannot leave the "locked during sync" banner stuck on
 * screen after the guard goes out of scope.
 */
class IsoListStateGuard {
public:
	IsoListStateGuard(std::atomic<bool>& isAtISOList,
	                   const std::function<bool()>& isUnmountCheck)
	    : isAtISOList_(isAtISOList),
	      wasAtList_(isAtISOList.load(std::memory_order_acquire)),
	      isUnmountCheck_(isUnmountCheck) {}

	~IsoListStateGuard();

	IsoListStateGuard(const IsoListStateGuard&) = delete;
	IsoListStateGuard& operator=(const IsoListStateGuard&) = delete;

private:
	std::atomic<bool>& isAtISOList_;
	bool wasAtList_;
	std::function<bool()> isUnmountCheck_;
};

// ======================================================================
// Helpers shared by selectIso.cpp and selectImage.cpp
// ======================================================================
// Each .cpp brings these in with `using namespace select_shared;`.

namespace select_shared {

	// ─── Prompt-row cursor invariant ─────────────────────────────────────────────
	//
	// Layout of every select screen (rows counted from the last list row L):
	//
	//     L      last list row
	//     L+1    blank separator row
	//     L+2    the prompt row            <- readline() is called with the cursor
	//                                         here, at column 0
	//
	// The prompt string itself is a SINGLE line (no leading "\n"), so readline's
	// own cursor bookkeeping is never off by a row. The blank separator row is
	// printed by the caller ("\n" after the list is drawn).
	//
	// After the user submits, the cursor is one row below the prompt row (L+3).
	// That holds for Enter AND for every hotkey: the hotkey handlers finish the
	// line with rl_newline() (see submitHotkey), so readline echoes the newline
	// exactly as it does for Enter. The filter prompt keeps the same rule on every
	// exit (Enter, Esc, Ctrl+D). Every iteration that does not redraw the list
	// therefore brings the cursor back with exactly one call, rewindToPromptRow(),
	// at the top of the loop. There are no other up/down escapes in the loop.
	namespace term {

		/**
		 * @brief Moves from the row below the prompt back onto the prompt row and
		 * clears it, so the next prompt is drawn in exactly the same place.
		 */
		void rewindToPromptRow();

	} // namespace term

	// ─── Pending selection (path-based) ──────────────────────────────────────────
	//
	// Staged ("pending") selections are stored as PATHS, not as index strings.
	// A path is stable across filter changes, filter clears, list refreshes and
	// re-sorts; an index is not. The index strings the UI shows
	// (RefreshState::pendingIndices, the "Pending Indices [P]" line) are only a
	// DISPLAY projection: loadAndDisplayIso / loadAndDisplayMountedISOs recompute
	// them from the paths (syncIsoPendingDisplay) against the final source list
	// right before every print, always as 1-based SOURCE indices, i.e. the same
	// numbers as the "n:m^" column, merged into ranges ("1-3 7 9-10").
	//
	//  - Staging ("3 5-9;"): bare numbers refer to what is on screen (the filtered
	//    view when a filter is active); a trailing '^' ("12^") refers to the
	//    source list directly. Each number is turned into a path immediately.
	//  - "P": the paths are resolved to source indices NOW, against the current
	//    source list, and the operation runs on the full source list. Paths that
	//    no longer exist are dropped.
	namespace pending {

		/// One parsed pending token: "N" or "A-B", with an optional trailing '^'.
		struct IndexToken {
			size_t lo     = 0;
			size_t hi     = 0;
			bool   source = false;   // '^' suffix: indices refer to the source list
		};

		/**
		 * @brief Parses a positive integer without leading zeros from a string view.
		 *
		 * Rejects empty views, leading zeros (including `"0"`), non-ASCII-digit
		 * characters, and values outside the range of `int`.
		 *
		 * @param[in]  view  Candidate token slice containing only digits.
		 * @param[out] value Receives the parsed positive integer on success.
		 * @return `true` if `view` represents a valid positive `int`; otherwise `false`.
		 */
		bool parsePositiveInt(std::string_view view, int& value);

		/**
		 * @brief Parses an index or index-range token with an optional `^` suffix.
		 *
		 * Supports `"N"`, `"A-B"`, `"N^"`, and `"A-B^"`, where `N`, `A`, and `B` are
		 * positive decimal integers without leading zeros. If `A > B`, the bounds are
		 * swapped so `out.lo <= out.hi`. `out` is reset to a default-constructed state
		 * before parsing.
		 *
		 * @param[in]  token Token string view to parse.
		 * @param[out] out   Populated with `source`, `lo`, and `hi` on success;
		 *                   default-initialized on failure.
		 * @return `true` if `token` is valid and parsed into `out`; otherwise `false`.
		 */
		bool parseIndexToken(std::string_view token, IndexToken& out);

		/**
		 * @brief Maps staged paths to sorted, unique 1-based indices in @p source.
		 *        Returns immediately when nothing is staged (no lookup map is built).
		 * @param prune If true, paths that are not in @p source are erased from
		 *              @p paths; otherwise @p paths is left untouched (display use,
		 *              where @p source may be momentarily stale).
		 */
		std::vector<size_t> resolveIndices(std::vector<std::string>& paths,
		                                   const std::vector<std::string>& source,
		                                   bool prune);

		/// {1,2,3,7,9,10} -> {"1-3","7","9-10"}
		std::vector<std::string> compressRuns(const std::vector<size_t>& idx);

		std::string joinTokens(const std::vector<std::string>& tokens);

		/**
		 * @brief Stages semicolon-terminated selections ("3 5-9 12^;") as paths.
		 *
		 * Recognizes input containing a semicolon and no forward slash. Tokens before
		 * the first semicolon are parsed using the same strict numeric validation rules
		 * as @c tokenizeInput(): leading-zero indices, malformed tokens, and integer
		 * overflow are rejected. Bare numbers index @p viewList (what is on screen),
		 * while numbers with a trailing '^' index @p sourceList. Descending ranges are
		 * accepted, but an entire range is rejected if either endpoint exceeds the
		 * selected list's size. Valid indices are expanded into paths; new paths are
		 * appended in input order, and paths already staged are not duplicated.
		 *
		 * @return true when the input is accepted as staging syntax and the pending
		 *         queue is non-empty; false when it should go down the normal command
		 *         path.
		 */
		bool stagePending(const std::string& inputString,
		                  std::vector<std::string>& paths,
		                  bool& hasPendingProcess,
		                  bool& needsClrScrn,
		                  const std::vector<std::string>& viewList,
		                  const std::vector<std::string>& sourceList);

		/// Owns g_isoPending for the lifetime of one selectForIsoFiles() call, so the
		/// loaders only treat pending as path-based while an ISO select screen is
		/// active (see syncIsoPendingDisplay).
		struct IsoPendingScope {
			explicit IsoPendingScope(const std::vector<std::string>* source) {

				g_isoPending.paths.clear();
				g_isoPending.source = source;

			}

			~IsoPendingScope() {

				g_isoPending.paths.clear();
				g_isoPending.source = nullptr;

			}

			IsoPendingScope(const IsoPendingScope&) = delete;
			IsoPendingScope& operator=(const IsoPendingScope&) = delete;
		};

	} // namespace pending

	// ─── Shared by both select screens ───────────────────────────────────────────
	//
	// Each screen's event loop is split into small steps that live in a session
	// class (iso_select::SelectSession, image_select::ImageSession). Each input
	// handler either declines the input (LoopAction::Proceed) or consumes it and
	// tells the loop what to do next (Continue / Break / Return). The handlers are
	// tried in a fixed order that is the same order as the original if-chain.
	namespace select_common {

		enum class LoopAction { Proceed, Continue, Break, Return };

		/// True for filter commands ("/...").
		bool isFilterCommand(const std::string& input);

	} // namespace select_common

} // namespace select_shared

} // extern "C++"
