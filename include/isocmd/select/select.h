// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SELECT_H
#define SELECT_H

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/core/state.h"

// Forward declarations
struct RefreshState;
struct FilterEntryContext;

// =================================================================
// Global State & Data Structures
// =================================================================

/// Pending state of the ISO select screen. Main-thread only.
struct IsoPending {
    std::vector<std::string>        paths;
    const std::vector<std::string>* source = nullptr;   // list the screen's source indices refer to
};

inline IsoPending g_isoPending;

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
    ReadlineKeybindingGuard() {
        setup_custom_keybindingsForSelect();
    }

    ~ReadlineKeybindingGuard() {
        reset_custom_keybindingsForSelect();
        RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
    }

    /// Re-establish custom keybindings after a sub-mode overrode them.
    void restore() {
        setup_custom_keybindingsForSelect();
    }

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
    void release() { released_ = true; }

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

    ~IsoListStateGuard() {
        if (!isUnmountCheck_()) {
            isAtISOList_.store(wasAtList_, std::memory_order_release);
        }

        GlobalState::g_filteringIndicator.store(false, std::memory_order_release);
    }

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
        inline void rewindToPromptRow() {
            std::cout << "\033[1A\r\033[K";
        }

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
        inline bool parsePositiveInt(std::string_view view, int& value) {
            if (view.empty() || view.front() == '0') {
                return false;
            }

            const bool allDigits = std::all_of(view.begin(), view.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            });
            if (!allDigits) {
                return false;
            }

            const auto [endPtr, error] = std::from_chars(view.data(), view.data() + view.size(), value);
            return error == std::errc{} && endPtr == view.data() + view.size();
        }

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
        inline bool parseIndexToken(std::string_view token, IndexToken& out) {
            out = {};

            const bool hasSourceSuffix = !token.empty() && token.back() == '^';
            if (hasSourceSuffix) {
                token.remove_suffix(1);
            }

            if (token.empty()) {
                return false;
            }

            const size_t firstDash = token.find('-');
            const bool hasMultipleDashes =
                firstDash != std::string_view::npos &&
                token.find('-', firstDash + 1) != std::string_view::npos;
            if (hasMultipleDashes) {
                return false;
            }

            int lo = 0;
            int hi = 0;

            if (firstDash == std::string_view::npos) {
                if (!parsePositiveInt(token, lo)) {
                    return false;
                }
                hi = lo;
            } else {
                const std::string_view left = token.substr(0, firstDash);
                const std::string_view right = token.substr(firstDash + 1);
                if (!parsePositiveInt(left, lo) || !parsePositiveInt(right, hi)) {
                    return false;
                }
            }

            if (lo > hi) {
                std::swap(lo, hi);
            }

            out.source = hasSourceSuffix;
            out.lo = static_cast<size_t>(lo);
            out.hi = static_cast<size_t>(hi);
            return true;
        }

        /**
         * @brief Maps staged paths to sorted, unique 1-based indices in @p source.
         *        Returns immediately when nothing is staged (no lookup map is built).
         * @param prune If true, paths that are not in @p source are erased from
         *              @p paths; otherwise @p paths is left untouched (display use,
         *              where @p source may be momentarily stale).
         */
        inline std::vector<size_t> resolveIndices(std::vector<std::string>& paths,
                                           const std::vector<std::string>& source,
                                           bool prune) {
            if (paths.empty()) return {};
            std::unordered_map<std::string_view, size_t> indexOf;
            indexOf.reserve(source.size());
            for (size_t i = 0; i < source.size(); ++i)
                indexOf.emplace(source[i], i + 1);            // first occurrence wins

            std::vector<size_t> idx;
            idx.reserve(paths.size());

            if (prune) {
                std::vector<std::string> kept;
                kept.reserve(paths.size());
                for (auto& p : paths) {
                    const auto it = indexOf.find(p);
                    if (it == indexOf.end()) continue;
                    idx.push_back(it->second);
                    kept.push_back(std::move(p));
                }
                paths = std::move(kept);
            } else {
                for (const auto& p : paths) {
                    const auto it = indexOf.find(p);
                    if (it != indexOf.end()) idx.push_back(it->second);
                }
            }

            std::sort(idx.begin(), idx.end());
            idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
            return idx;
        }

        /// {1,2,3,7,9,10} -> {"1-3","7","9-10"}
        inline std::vector<std::string> compressRuns(const std::vector<size_t>& idx) {
            std::vector<std::string> out;
            for (size_t i = 0; i < idx.size();) {
                size_t j = i;
                while (j + 1 < idx.size() && idx[j + 1] == idx[j] + 1) ++j;
                out.push_back(i == j
                    ? std::to_string(idx[i])
                    : std::to_string(idx[i]) + "-" + std::to_string(idx[j]));
                i = j + 1;
            }
            return out;
        }

        inline std::string joinTokens(const std::vector<std::string>& tokens) {
            std::string s;
            for (const auto& t : tokens) {
                if (!s.empty()) s += ' ';
                s += t;
            }
            return s;
        }

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
        inline bool stagePending(const std::string& inputString,
                          std::vector<std::string>& paths,
                          bool& hasPendingProcess,
                          bool& needsClrScrn,
                          const std::vector<std::string>& viewList,
                          const std::vector<std::string>& sourceList) {
            const size_t semi = inputString.find(';');
            if (semi == std::string::npos || inputString.find('/') != std::string::npos)
                return false;

            std::istringstream iss(inputString.substr(0, semi));
            std::unordered_set<std::string> seen(paths.begin(), paths.end());

            std::string tok;
            while (iss >> tok) {
                IndexToken t;
                if (!parseIndexToken(tok, t)) continue;

                const std::vector<std::string>& list = t.source ? sourceList : viewList;

                // Match tokenizeInput(): reject the entire token if either
                // endpoint exceeds the available list size.
                if (t.lo > list.size() || t.hi > list.size()) continue;

                for (size_t n = t.lo;; ++n) {
                    const std::string& path = list[n - 1];
                    if (seen.insert(path).second) paths.push_back(path);

                    if (n == t.hi) break;
                }
            }

            if (paths.empty()) return false;
            hasPendingProcess = true;
            needsClrScrn = true;
            return true;
        }


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
        inline bool isFilterCommand(const std::string& input) {
            return !input.empty() && input[0] == '/';
        }

    } // namespace select_common

} // namespace select_shared

// --- Data Loading & List Display ---

/// While an ISO select screen is active, projects its staged selection (held as
/// paths) onto pendingIndices as source indices, merged into ranges, against the
/// list as it is right now. Returns false when no select screen is active, so
/// the caller manages its own pending state. Called by the ISO / mounted-ISO
/// loaders right before they print.
bool syncIsoPendingDisplay(std::vector<std::string>& pendingIndices,
                           bool& hasPendingProcess,
                           bool importRunning);

/// Loads and displays the ISO list from the database / cache.
bool loadAndDisplayIso(
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    const std::string &listSubType,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/// Loads and displays currently mounted ISOs.
bool loadAndDisplayMountedISOs(
    std::vector<std::string> &isoDirs,
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/// Loads and displays non-ISO image files (MDF, NRG, CHD, DAA/GBI, BIN/IMG)
/// for conversion.
void loadAndDisplayImageFiles(
    std::vector<std::string> &files,
    const std::string &fileType,
    bool &need2Sort,
    bool &isFiltered,
    bool &list,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    std::shared_ptr<RefreshState> state
);


// --- Input Processing & Action Handlers ---

/// Processes a user selection for Mount or Unmount.
void processInputForMountOrUmount(
    const std::string &input,
    const std::vector<std::string> &files,
    bool &umountMvRmBreak,
    bool &verbose,
    bool isUnmount
);

/// Processes a user selection for Copy, Move, or Remove.
void processInputForCpMvRm(
    const std::string &input,
    const std::vector<std::string> &isoFiles,
    const std::string &process,
    bool &umountMvRmBreak,
    bool &filterHistory,
    bool &verbose
);

/// Processes a user selection for converting images to ISO.
void processInputForConversions(
    const std::string &input,
    std::vector<std::string> &fileList,
    const bool &modeMdf,
    const bool &modeNrg,
    const bool &modeChd,
    const bool &modeDaa,
    bool &verbose
);

/// Handles the write-to-USB flow for a user selection.
void writeToUsb(
    const std::string &input,
    const std::vector<std::string> &isoFiles
);


// --- Filtering & Pagination ---

/// Handles pagination keys, help ('?'), and display toggles ('*', '~').
bool processPaginationHelpAndDisplay(
    const std::string& inputString,
    size_t& totalPages,
    size_t& currentPage,
    bool& isFiltered,
    bool& needsClrScrn,
    const std::string& operation,
    bool& need2Sort,
    std::atomic<bool>* isAtISOList,
    std::vector<std::string>& filteredFiles
);

/**
 * @brief Filter entry point for ISO file operations (mount, unmount, etc.).
 * @details Resolves the correct source list based on the current filter and
 * unmount state, then forwards to @c runSharedFilterFlow. Source list
 * priority:
 * -# @p filteredFiles — if a filter is already active (@c ctx.isFiltered && @c *ctx.isFiltered)
 * -# @p isoDirs       — if in unmount mode with no active filter
 * -# @c GlobalState::globalIsoFileList — otherwise
 *
 * When @p ctx.state is provided it also enables the live main-list preview: the
 * list type is @c "MOUNTED_ISOS" in unmount mode and @c "ISO_FILES" otherwise,
 * and @c pendingIndices / @c hasPendingProcess / the list subtype are taken
 * from the shared @c RefreshState.
 *
 * @param inputString    Raw user input; must be exactly @c "/" to trigger filtering.
 * @param filteredFiles  The currently displayed (possibly already filtered) file list.
 * @param operation      Name of the ISO operation shown in the prompt (e.g. "Mount").
 * @param operationColor Raw ANSI escape code used to colorise @p operation in the prompt.
 * @param isoDirs        Mounted ISO paths used as the source list in unmount mode.
 * @param isUnmount      True when the caller is performing an unmount operation.
 * @param ctx            Shared filter/display state pointers, refresh state, and repaint callback.
 * @return @c true if @p inputString was handled as a filter command, @c false otherwise.
 */
bool handleFilteringForISO(const std::string& inputString,
                           std::vector<std::string>& filteredFiles,
                           const std::string& operation,
                           const std::string& operationColor,
                           const std::vector<std::string>& isoDirs,
                           bool isUnmount,
                           FilterEntryContext ctx);

/**
 * @brief Filter entry point for convert-to-ISO operations.
 * @details Forwards directly to @c runSharedFilterFlow with a fixed orange
 * operation color. Unlike @c handleFilteringForISO there is no unmount mode
 * or source list override — the files vector is always used as-is.
 *
 * When @p ctx.state is provided it enables the live main-list preview for the
 * @c "IMAGE_FILES" list (subtype @c "convert2iso"). Unlike the ISO path,
 * @c ctx.pendingIndices and @c ctx.hasPendingProcess here point to the caller's
 * own locals (selectForImageFiles), not fields of @p ctx.state.
 *
 * @param inputString Raw user input; must be exactly @c "/" to trigger filtering.
 * @param files       The list of convertible files to filter in place.
 * @param operation   Name of the conversion operation shown in the prompt.
 * @param ctx         Shared filter/display state pointers, sort/pending pointers,
 *                    refresh state, and repaint callback.
 */
void handleFilteringConvert2ISO(const std::string& inputString,
                              std::vector<std::string>& files,
                              const std::string& operation,
                              FilterEntryContext ctx);


// --- Utility & Cleanup ---

/// Displays the results of a selection session.
void handleSelectIsoFilesResults(
    const std::string &operation,
    bool &verbose,
    bool &isFiltered,
    bool &umountMvRmBreak
);

#endif // SELECT_H
