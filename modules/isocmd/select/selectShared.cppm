// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.select.selectShared;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// Internal helpers shared by selectIso.cpp and selectImage.cpp: scope guards
// and the path-based pending-selection logic. Not for use outside src/select/.

/// Pending state of the ISO select screen. Main-thread only.
struct IsoPending {
	std::vector<std::string>        paths;
	const std::vector<std::string>* source = nullptr;   // list the screen's source indices refer to
};

extern IsoPending g_isoPending;

// Scope guards restore terminal state, keybindings and UI flags on every exit path.

/**
 * @brief Installs selection keybindings and restores them automatically on exit.
 * Also clears pending prompt text so it cannot leak into another screen.
 * Call restore() after a sub-mode replaces the keybindings.
 */
class LineEditorKeybindingGuard {
public:
	LineEditorKeybindingGuard();
	~LineEditorKeybindingGuard();

	/// Re-establish custom keybindings after a sub-mode overrode them.
	void restore();


	LineEditorKeybindingGuard(const LineEditorKeybindingGuard&) = delete;
	LineEditorKeybindingGuard& operator=(const LineEditorKeybindingGuard&) = delete;
};

/**
 * @brief Temporarily sets a shared atomic flag.
 * Restores its original value on scope exit, including when guards are nested.
 * Call release() to keep the new value instead.
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
 * @brief Restores the ISO-list flag on exit except during unmount.
 * The unmount check runs at destruction time. Always clear the filtering
 * indicator so the "Filtering locked during sync" message cannot remain stuck.
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

	// Keep the prompt on a fixed row: list, blank separator, then one input line.
	// Long input scrolls horizontally instead of wrapping. Enter and hotkeys
	// finish one row below the prompt; if the list is not redrawn, rewind once.
	// The filter prompt follows the same rule on every exit path.
	namespace term {

		/**
		 * @brief Moves from the row below the prompt back onto the prompt row and
		 * clears it, so the next prompt is drawn in exactly the same place.
		 */
		void rewindToPromptRow();

	} // namespace term

	// Store staged selections as paths, not row numbers, so sorting, filtering
	// and refreshes cannot change which files are selected. Bare numbers refer
	// to visible rows; a '^' suffix refers to the source list. P resolves paths
	// against the current source list and drops missing paths. The displayed
	// pending indices are rebuilt from those paths before each list redraw.
	namespace pending {

		/// One parsed pending token: "N" or "A-B", with an optional trailing '^'.
		struct IndexToken {
			std::size_t lo     = 0;
			std::size_t hi     = 0;
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
		std::vector<std::size_t> resolveIndices(std::vector<std::string>& paths,
		                                   const std::vector<std::string>& source,
		                                   bool prune);

		/// {1,2,3,7,9,10} -> {"1-3","7","9-10"}
		std::vector<std::string> compressRuns(const std::vector<std::size_t>& idx);

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
