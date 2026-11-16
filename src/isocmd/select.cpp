// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../databaseOps.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../globalMutexes.h"
#include "../rendering.h"
#include "../readline.h"
#include "../select.h"
#include "../sharedRefreshState.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

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
namespace {

/**
 * @brief Moves from the row below the prompt back onto the prompt row and
 * clears it, so the next prompt is drawn in exactly the same place.
 */
inline void rewindToPromptRow() {
    std::cout << "\033[1A\r\033[K";
}

} // namespace

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
namespace {

/// One parsed pending token: "N" or "A-B", with an optional trailing '^'.
struct IndexToken {
    size_t lo     = 0;
    size_t hi     = 0;
    bool   source = false;   // '^' suffix: indices refer to the source list
};

/// Parses "N", "A-B", "N^" or "A-B^" (1-based). Returns false for anything else.
bool parseIndexToken(std::string_view tok, IndexToken& out) {
    out = {};
    if (!tok.empty() && tok.back() == '^') {
        out.source = true;
        tok.remove_suffix(1);
    }
    if (tok.empty()) return false;

    const char* p = tok.data();
    const char* e = p + tok.size();
    size_t a = 0, b = 0;

    auto r = std::from_chars(p, e, a);
    if (r.ec != std::errc{} || a == 0) return false;
    b = a;
    if (r.ptr != e) {
        if (*r.ptr != '-') return false;
        r = std::from_chars(r.ptr + 1, e, b);
        if (r.ec != std::errc{} || r.ptr != e || b == 0) return false;
    }
    if (a > b) std::swap(a, b);
    out.lo = a;
    out.hi = b;
    return true;
}

/**
 * @brief Maps staged paths to sorted, unique 1-based indices in @p source.
 *        Returns immediately when nothing is staged (no lookup map is built).
 * @param prune If true, paths that are not in @p source are erased from
 *              @p paths; otherwise @p paths is left untouched (display use,
 *              where @p source may be momentarily stale).
 */
std::vector<size_t> resolveIndices(std::vector<std::string>& paths,
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
std::vector<std::string> compressRuns(const std::vector<size_t>& idx) {
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

std::string joinTokens(const std::vector<std::string>& tokens) {
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
 * the first semicolon are expanded: bare numbers index @p viewList (what is on
 * screen), numbers with a trailing '^' index @p sourceList. Out-of-range and
 * non-numeric tokens are ignored. New paths are appended in the order given;
 * paths already staged are not duplicated.
 *
 * @return true when the input is accepted as staging syntax (and the queue is
 *         non-empty); false when it should go down the normal command path.
 */
bool stagePending(const std::string& inputString,
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
        const size_t hi = std::min(t.hi, list.size());   // also bounds huge ranges
        for (size_t n = t.lo; n <= hi; ++n) {
            const std::string& path = list[n - 1];
            if (seen.insert(path).second) paths.push_back(path);
        }
    }

    if (paths.empty()) return false;
    hasPendingProcess = true;
    needsClrScrn = true;
    return true;
}

/// Pending state of the ISO select screen. Main-thread only.
struct IsoPending {
    std::vector<std::string>        paths;
    const std::vector<std::string>* source = nullptr;   // list the screen's source indices refer to
};
IsoPending g_isoPending;

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

} // namespace

/**
 * @brief Projects the staged ISO selection onto the display state
 * (@p pendingIndices / @p hasPendingProcess) as 1-based SOURCE indices merged
 * into ranges, for the "Pending Indices" line. Does not prune staged paths.
 *
 * Called by @c loadAndDisplayIso and @c loadAndDisplayMountedISOs right
 * before they print (under @c updateListMutex), i.e. against the final,
 * reloaded and sorted list. Every redraw path therefore shows correct
 * numbers: loop redraws, resize repaints and asynchronous import refreshes.
 *
 * @param importRunning True while a background import is rebuilding the list;
 *        the last good display is kept then.
 * @return true if an ISO select screen owns the selection (the display state
 *         is managed here); false if none is active, in which case the caller
 *         handles its own pending state.
 */
bool syncIsoPendingDisplay(std::vector<std::string>& pendingIndices,
                           bool& hasPendingProcess,
                           bool importRunning) {
    if (!g_isoPending.source) return false;
    if (importRunning) return true;
    // A momentarily empty source list right after an import is still being
    // repopulated; keep the last good display. (Vanished paths are pruned by "P".)
    if (g_isoPending.source->empty() && !g_isoPending.paths.empty()) return true;
    pendingIndices =
        compressRuns(resolveIndices(g_isoPending.paths, *g_isoPending.source, false));
    hasPendingProcess = !pendingIndices.empty();
    return true;
}

namespace {

/// Implementation of processOperationForSelectedIsoFiles. When
/// @p useSourceList is true the indices refer to the full source list even if
/// a filter is active (used for "P", whose indices are source indices).
void processOperationImpl(const std::string& inputString,
                          bool& isFiltered,
                          const std::vector<std::string>& filteredFiles,
                          std::vector<std::string>& isoDirs,
                          const std::string& operation,
                          bool& umountMvRmBreak,
                          bool& filterHistory,
                          bool useSourceList) {

    clearScrollBuffer();
    bool verbose = false;

    const bool isUnmount = (operation == "umount");
    const std::vector<std::string>& sourceList =
        isUnmount ? isoDirs : GlobalState::globalIsoFileList;
    const std::vector<std::string>& activeList =
        (isFiltered && !useSourceList) ? filteredFiles : sourceList;

    if (operation == "mount" || isUnmount) {
        if (isUnmount) umountMvRmBreak = true;
        processInputForMountOrUmount(inputString, activeList, umountMvRmBreak, verbose, isUnmount);
    } else if (operation == "write2usb") {
        writeToUsb(inputString, activeList);
    } else {
        processInputForCpMvRm(inputString, activeList, operation, umountMvRmBreak, filterHistory, verbose);
    }

    handleSelectIsoFilesResults(operation, verbose, isFiltered, umountMvRmBreak);
}

/// Image caches the unfiltered list of a file type is restored from.
const std::vector<std::string>* imageCacheFor(const std::string& fileType) {
    if (fileType == "bin" || fileType == "img") return &GlobalState::binImgFilesCache;
    if (fileType == "mdf") return &GlobalState::mdfMdsFilesCache;
    if (fileType == "nrg") return &GlobalState::nrgFilesCache;
    if (fileType == "chd") return &GlobalState::chdFilesCache;
    if (fileType == "daa") return &GlobalState::daaGbiFilesCache;
    return nullptr;
}

} // namespace

/**
 * @brief Routes validated user input to the correct filesystem or mount
 * operation.
 *
 * Selects the active list (filtered files, global ISO files, or mounted
 * directories for unmount) and dispatches the input to the matching handler.
 * The caller manages list-view locking around the operation. Indices in
 * @p inputString refer to the filtered list when a filter is active.
 *
 * @param inputString      User input, typically indices or paths.
 * @param isFiltered       Updated if the operation invalidates the filter.
 * @param filteredFiles    Currently visible filtered subset.
 * @param isoDirs          Active mount points used by unmount.
 * @param operation        Operation to dispatch.
 * @param umountMvRmBreak  Requests a list refresh after destructive actions.
 * @param filterHistory    Readline search-history state updated by the handler.
 */
void processOperationForSelectedIsoFiles(const std::string& inputString,
                                         bool& isFiltered,
                                         const std::vector<std::string>& filteredFiles,
                                         std::vector<std::string>& isoDirs,
                                         const std::string& operation,
                                         bool& umountMvRmBreak,
                                         bool& filterHistory) {
    processOperationImpl(inputString, isFiltered, filteredFiles, isoDirs, operation,
                         umountMvRmBreak, filterHistory, /*useSourceList=*/false);
}

/**
 * @brief Background watcher that requests an ISO-list redraw after an import
 * completes.
 *
 * Waits on a condition variable until the import thread signals completion,
 * then publishes a pending refresh request instead of touching the terminal or
 * Readline. The actual redraw happens later, on the main thread, inside
 * Readline's event hook @c checkPendingRefresh — the only thread permitted to
 * call Readline functions or write to the terminal.
 *
 * - **Thread safety:** holds the shared @c RefreshState alive via
 *   @c std::shared_ptr; the handoff to @c GlobalState::g_pendingRefreshState is
 *   itself guarded by a mutex, since a bare @c shared_ptr is not safe to read
 *   and write concurrently.
 * - **Event-driven:** blocks on @c RefreshState::importCV rather than polling.
 * - **No direct Readline access:** only stores the target state and publishes
 *   @c PendingRefreshKind::IsoList; the redraw (including @c rl_on_new_line()
 *   and @c rl_redisplay()) happens later in @c checkPendingRefresh.
 * - **Pending selection:** the watcher never touches the staged selection.
 *   Staged paths survive the reload by construction; the index display is
 *   recomputed by the loader on the main thread when it redraws (see
 *   @c syncIsoPendingDisplay).
 * - **Indicator reset:** clears @c GlobalState::g_filteringIndicator so any
 *   active filter/sync status stops being displayed once the import/refresh
 *   flow finishes.
 * - **Auto-termination:** runs once and exits; not a loop.
 *
 * @param isAtISOList  Atomic flag; a refresh is only requested if the user is
 *                     in the list view.
 * @param state        Shared state providing @c isImportRunning (the CV
 *                     predicate), @c importMutex / @c importCV, the display
 *                     context (@c filteredFiles, @c isFiltered,
 *                     @c listSubtype), the list state (@c pendingIndices,
 *                     @c hasPendingProcess, @c umountMvRmBreak) and pagination
 *                     (@c currentPage, @c originalPage). Retained via
 *                     @c GlobalState::g_pendingRefreshState until consumed by
 *                     @c checkPendingRefresh on the main thread.
 */
void refreshListAfterAutoUpdate(std::atomic<bool>& isAtISOList,
                                std::shared_ptr<RefreshState> state) {
    {
        std::unique_lock<std::mutex> lock(state->importMutex);
        state->importCV.wait(lock, [&] {
            return !state->isImportRunning.load(std::memory_order_acquire);
        });
    }
    if (isAtISOList.load()) {
    {
        std::lock_guard<std::mutex> lk(GlobalMutexes::readLineMutex);
        GlobalState::g_pendingRefreshState = state;                        // set state first
    }
        GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::IsoList); // then publish
    }
    GlobalState::g_filteringIndicator.store(false);
}

/**
 * @brief Interactive TUI for batch ISO operations (mount, umount, cp, mv, rm,
 * write2usb).
 *
 * Paginated, multi-layer filterable interface for selecting ISO files and
 * executing system operations. Manages terminal state, background imports, and
 * operation dispatch inside a readline-driven event loop.
 *
 * - **Dynamic context:** toggles between @c GlobalState::globalIsoFileList and
 *   active mount points (@c isoDirs) based on @p operation; unmount operations
 *   skip database cleanup and disable the manual-refresh keybinding (R). The
 *   operation colour is chosen from @p operation at entry (red for rm, green
 *   for cp/mount, yellow for mv/write2usb/umount).
 * - **RAII keybindings:** @c ReadlineKeybindingGuard calls
 *   @c setup_custom_keybindingsForSelect() on construction and
 *   @c reset_custom_keybindingsForSelect() on destruction, so cleanup runs on
 *   every exit path (break, ESC return, exception) without manual resets. The
 *   guard is scoped to the whole function.
 * - **RAII atomic flags:** @c AtomicFlagGuard sets @p isAtISOList to @c false
 *   for the processing block and restores it on scope exit.
 *   @c GlobalState::g_filteringIndicator is likewise wrapped by its own
 *   @c AtomicFlagGuard during the same block. For non-unmount operations,
 *   @c IsoListStateGuard restores @p isAtISOList to its prior value on scope
 *   exit.
 * - **Event-driven refresh:** a detached watcher shares @c RefreshState via
 *   @c std::shared_ptr. It is started only for non-unmount operations while an
 *   import is running, at most once per session, and publishes a pending list
 *   refresh when @c importCV signals completion. The main Readline thread
 *   performs the actual redraw.
 * - **Stacked filtering:** supports successive narrowing via @c filteringStack.
 *   Reads/writes of @c filteredFiles and sizing evaluations are protected by
 *   @c GlobalMutexes::updateListMutex to avoid races with the watcher thread.
 *   On ESC (@c '\\x1b', typically bound to the @c '<' key via
 *   @c setup_custom_keybindingsForSelect): if a filter is active, clears the
 *   filter stack, restores @c currentPage to @c originalPage, and continues;
 *   if no filter is active, sets @c currentPage to 0 and returns to the
 *   previous menu (keybinding cleanup handled by the RAII guard).
 * - **Filtering lock enforcement:** attempting to filter while a background
 *   import is running blocks the operation and sets
 *   @c GlobalState::g_filteringIndicator to @c true ("Filtering locked during
 *   sync") to notify the user.
 * - **Two-phase execution (path-based):** a selection typed with a trailing
 *   semicolon ("3 5-9;") is staged as PATHS (@c g_isoPending). Bare numbers
 *   are what is on screen (the filtered view when filtering); "n^" numbers are
 *   source indices. The @c "P" command resolves the staged paths to source
 *   indices at that moment, drops files that no longer exist, and runs the
 *   operation against the full source list, so a selection staged under
 *   different filters (or none) is always executed correctly. The
 *   "Pending Indices" line shows the same source numbers as the "n:m^"
 *   column, merged into ranges. @c "clr" discards the staged set; @c "P" with
 *   nothing staged is a no-op.
 * - **Manual refresh:** pressing @c "R" (when not unmount, the ISO list is
 *   non-empty, and no import is running) spawns a background database import.
 *   Stale completed threads in @p backgroundThreads are joined and erased
 *   before each new import is launched.
 * - **Persistent directory state:** @c isoDirs is @c static, so its contents
 *   survive re-entry for the lifetime of the process.
 * - **Terminal integrity:** @c \\f and @c \\t are bound to no-ops via Readline
 *   to prevent terminal corruption. PgUp/PgDn are disabled when
 *   @c ITEMS_PER_PAGE is 0.
 * - **Prompt-row invariant:** the prompt is a single line and always lands on
 *   the same row (list, one blank separator row, prompt). When the list is
 *   redrawn the loop prints one @c "\\n" after it; when it is not, a single
 *   @c rewindToPromptRow() at the top of the loop returns to the prompt row.
 *   See the "Prompt-row cursor invariant" note near the top of this file.
 * - **Resize repaint:** each @c readline() is wrapped by
 *   @c readlineWithResize, whose lambda re-runs the normal display step so the
 *   list repaints on terminal-width changes while the prompt waits. The same
 *   lambda is passed to the filter handler for the case where the live preview
 *   is off. (Image select repaints with a throwaway sort flag, so a resize
 *   never re-sorts the list.)
 *
 * @param operation         Target action ("mount", "umount", "cp", "mv", "rm",
 *                          or "write2usb"). Determines list source, colour
 *                          scheme, and dispatch.
 * @param isAtISOList       Set to @c true while the ISO list is displayed
 *                          (non-unmount only); managed by the RAII guards
 *                          during operation execution. Also gates
 *                          watcher-thread refresh requests.
 * @param backgroundThreads Joinable worker threads (from manual R-press
 *                          imports), retained for lifetime management; stale
 *                          completed threads are joined and erased before each
 *                          new import.
 * @param refreshState      Shared UI state and condition variable used to
 *                          synchronize the watcher with the active import
 *                          session. If @c nullptr, a new @c RefreshState is
 *                          constructed internally.
 */
void selectForIsoFiles(const std::string& operation,
                       std::atomic<bool>& isAtISOList,
                       std::vector<std::thread>& backgroundThreads,
                       std::shared_ptr<RefreshState> refreshState) {

    // --- RAII: keybindings are automatically reset on any exit path ---
    ReadlineKeybindingGuard keybindingGuard;

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    static std::vector<std::string> isoDirs;
    isoDirs.reserve(1000);

    if (!refreshState) refreshState = std::make_shared<RefreshState>();

    std::vector<std::string>& filteredFiles = refreshState->filteredFiles;
    std::vector<std::string>& pendingIndices = refreshState->pendingIndices;
    bool& isFiltered                         = refreshState->isFiltered;
    bool& hasPendingProcess                  = refreshState->hasPendingProcess;
    bool& umountMvRmBreak                    = refreshState->umountMvRmBreak;
    std::string& listSubtype                 = refreshState->listSubtype;
    size_t& currentPage                      = refreshState->currentPage;
    size_t& originalPage                     = refreshState->originalPage;

    filteredFiles.reserve(1000);
    isFiltered        = false;
    hasPendingProcess = false;
    umountMvRmBreak   = false;
    currentPage       = 0;
    originalPage      = 0;

    const bool isUnmount = (operation == "umount");

    // Staged selection starts empty on every entry; it is held as paths and
    // projected into pendingIndices for display (see "Pending selection").
    pendingIndices.clear();
    IsoPendingScope pendingScope(isUnmount
        ? static_cast<const std::vector<std::string>*>(&isoDirs)
        : &GlobalState::globalIsoFileList);

    bool needsClrScrn  = true;
    bool filterHistory = false;

    std::string operationColor = std::string(
        operation == "rm"        ? UI::Palette::Red    :
        operation == "cp"        ? UI::Palette::Green  :
        operation == "mv"        ? UI::Palette::Yellow :
        operation == "mount"     ? UI::Palette::Green  :
        operation == "write2usb" ? UI::Palette::Yellow :
        operation == "umount"    ? UI::Palette::Yellow : UI::Palette::RL_BoldAlt
    );

    listSubtype = (operation == "mount") ? "mount" : (operation == "write2usb") ? "write2usb" : "cp_mv_rm";

    // --- RAII: isAtISOList state automatically managed ---
    IsoListStateGuard isoListGuard(isAtISOList, [&isUnmount]() { return isUnmount; });

    while (true) {
        clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        GlobalState::g_operationCancelled.store(false);
        filterHistory = false;
        clear_history();

        if (!isFiltered) originalPage = currentPage;

        if (!isUnmount) {
            removeNonExistentPathsFromDatabase(GlobalState::globalIsoFileList);
            isAtISOList.store(true);
        }

        // Position the cursor on the prompt row (see the invariant note at the
        // top of this file). Redraw: list, then one blank separator row.
        // No redraw: single rewind from the row below the previous prompt.
        if (needsClrScrn) {
            if (!isUnmount) {
                if (!loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, refreshState))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, refreshState))
                    break;
            }
            std::cout << "\n";
            umountMvRmBreak = false;
        } else {
            rewindToPromptRow();
        }

        if (refreshState->isImportRunning.load() && !isUnmount) {
            bool watcherExpected = false;
            if (refreshState->isWatcherRunning.compare_exchange_strong(watcherExpected, true)) {
                std::thread([&isAtISOList, refreshState]() {
                    refreshListAfterAutoUpdate(isAtISOList, refreshState);
                    refreshState->isWatcherRunning.store(false);
                }).detach();
            }
        }

        const ReadlineAndPromptTheme pt = getPromptTheme();

        setup_custom_keybindingsForSelect();

        // Unbind unused hotkeys for the umount prompt.
        if (isUnmount) {
            rl_bind_keyseq("R", rl_insert);
            rl_bind_keyseq("*", rl_insert);
        }

        std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

        // Single-line prompt: the blank separator row above it is printed by
        // the display step, not by a leading "\n" in the prompt.
        std::string prompt =
            prefix +
            std::string(UI::Palette::RL_idxGreen) + "IDX" +
            pt.primary + " ↵ for " + "\001" +
            operationColor + "\002" + operation +
            pt.primary + ", ? for help: " +
            pt.reset;

        // Repaints the list on terminal resize while the prompt waits. Runs on
        // the Readline thread (rl_event_hook); uses the same display step as
        // above.
        auto repaintList = [&] {
            const bool savedBreak = umountMvRmBreak;
            if (!isUnmount)
                loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak,
                                  pendingIndices, hasPendingProcess, currentPage, originalPage,
                                  refreshState);
            else
                loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak,
                                          pendingIndices, hasPendingProcess, currentPage, originalPage,
                                          refreshState);
            umountMvRmBreak = savedBreak;
        };

        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, repaintList);
        rl_startup_hook = nullptr;

        if (!rawInput) {
            if (isFiltered) {
            isFiltered = false;
            filteringStack.clear();
            currentPage = originalPage;
            needsClrScrn = true;
            break;
            } else {
                break;
            }
        }

        std::string inputString(rawInput.get());

        if (inputString[0] == ';' || inputString.find(";;") != std::string::npos) {
            needsClrScrn = false;
            continue;
        }

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        if (inputString[0] == 'R' && refreshState->isImportRunning.load()) {
            needsClrScrn = false;
            continue;
        }

        if (inputString == "R" && !isUnmount && !GlobalState::globalIsoFileList.empty() && !refreshState->isImportRunning.load()) {
            backgroundThreads.erase(
                std::remove_if(backgroundThreads.begin(), backgroundThreads.end(),
                    [](std::thread& t) {
                        if (t.joinable()) { t.join(); return true; }
                        return false;
                    }),
                backgroundThreads.end()
            );
            needsClrScrn = true;
            refreshState->isImportRunning.store(true);
            backgroundThreads.emplace_back([refreshState] {
                backgroundDatabaseImport(refreshState);
                refreshState->isWatcherRunning.store(false);
            });
            continue;
        }

        if (inputString == "\x1b") {
            if (isFiltered) {
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                needsClrScrn = true;
                continue;
            } else {
                currentPage = 0;
                return;
            }
        }

        if (inputString == "P" && g_isoPending.paths.empty()) {
            needsClrScrn = false;
            hasPendingProcess = false;
            continue;
        }

        if (inputString == "clr") {
            g_isoPending.paths.clear();
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        size_t totalPages = 0;
        {
            std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
            const std::vector<std::string>& currentList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : GlobalState::globalIsoFileList);
            totalPages = (GlobalState::ITEMS_PER_PAGE != 0) ? ((currentList.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE) : 0;
        }
        bool need2Sort = false;

        bool validCommand = processPaginationHelpAndDisplay(
            inputString,
            totalPages,
            currentPage,
            isFiltered,
            needsClrScrn,
            operation,
            need2Sort,
            &isAtISOList,
            filteredFiles
        );

        if (validCommand) continue;

        // --- RAII: lock guard automatically releases on scope exit ---
        bool filteringHandled = false;
        {
            if ((inputString == "/" || (!inputString.empty() && inputString[0] == '/')) && !refreshState->isImportRunning.load()) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                if (handleFilteringForISO(inputString, filteredFiles, isFiltered, needsClrScrn,
                                          filterHistory, operation, operationColor, isoDirs, isUnmount, currentPage,
                                          refreshState, repaintList)) {
                    GlobalState::g_filteringIndicator.store(false);
                    filteringHandled = true;
                }
            } else if ((inputString == "/" || (!inputString.empty() && inputString[0] == '/')) && refreshState->isImportRunning.load()) {
                GlobalState::g_filteringIndicator.store(true);
                needsClrScrn = true;
                continue;
            }
        }

        if (filteringHandled) {
            keybindingGuard.restore();
            continue;
        }

        // Stage "3 5-9;" as paths. Bare numbers = what is on screen, "n^" = source index.
        bool pendingHandled = false;
        {
            std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
            const std::vector<std::string>& sourceList = *g_isoPending.source;
            const std::vector<std::string>& viewList   = isFiltered ? filteredFiles : sourceList;
            pendingHandled = stagePending(inputString, g_isoPending.paths,
                                          hasPendingProcess, needsClrScrn,
                                          viewList, sourceList);
        }
        if (pendingHandled) continue;

        // --- RAII: atomic flag guard for the processing block ---
        bool pendingExecuted = false;
        {
            AtomicFlagGuard processingGuard(isAtISOList, false);
            AtomicFlagGuard filteringGuard(GlobalState::g_filteringIndicator, true);

            needsClrScrn = true;

            if (inputString == "P") {
                // Resolve staged paths to source indices NOW; drop vanished files.
                std::string batch;
                {
                    std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                    batch = joinTokens(compressRuns(
                        resolveIndices(g_isoPending.paths, *g_isoPending.source, true)));
                }
                pendingExecuted = true;
                if (batch.empty()) {
                    pendingIndices.clear();
                    hasPendingProcess = false;
                } else {
                    processOperationImpl(batch, isFiltered, filteredFiles, isoDirs,
                                         operation, umountMvRmBreak, filterHistory,
                                         /*useSourceList=*/true);
                }
            } else {
                processOperationImpl(inputString, isFiltered, filteredFiles, isoDirs,
                                     operation, umountMvRmBreak, filterHistory,
                                     /*useSourceList=*/false);
            }
        }
        // isAtISOList automatically restored here.
        keybindingGuard.restore();
        if (pendingExecuted) continue;
    }
    // Keybinding guard destructor automatically calls reset_custom_keybindingsForSelect().
}

/**
 * @brief Interactive TUI for selecting disc images to convert to ISO.
 *
 * Paginated, filterable interface for choosing BIN/IMG, MDF, NRG, CHD, or
 * DAA/GBI files and dispatching them to the convert2iso pipeline. Drives the
 * same Readline event loop as @c selectForIsoFiles, but over the image cache
 * for the requested @p fileType, with the matching operation name.
 *
 * - **File type mapping:** @p fileType selects the extension(s), the display
 *   name, and the conversion operation (@c "ccd2iso", @c "mdf2iso",
 *   @c "nrg2iso", @c "chd2iso", @c "daa2iso"); "bin" and "img" both map to
 *   the BIN/IMG conversion.
 * - **RAII keybindings:** @c ReadlineKeybindingGuard installs select-mode
 *   bindings on construction and resets them on destruction, so cleanup runs
 *   on every exit path.
 * - **Prompt-row invariant:** same as @c selectForIsoFiles: a single-line
 *   prompt on a fixed row (list, one blank separator row, prompt), reached
 *   either by printing one @c "\\n" after a list redraw or by a single
 *   @c rewindToPromptRow() when the list is not redrawn.
 * - **Resize repaint:** each @c readline() is wrapped in
 *   @c readlineWithResize with a lambda that re-runs the display step. A
 *   throwaway sort flag is used so a resize never re-sorts the list.
 * - **Cancellation:** ESC clears the filter (restoring the unfiltered cache
 *   for this file type and the original page) if a filter is active,
 *   otherwise resets the page and returns to the caller. The @c R key is
 *   unbound because this prompt has no manual refresh.
 * - **Two-phase execution (path-based):** "3 5-9;" is staged as paths. Bare
 *   numbers are what is on screen, "n^" numbers are indices into the
 *   unfiltered list. "P" resolves the paths to indices of the unfiltered list
 *   and converts against it; an active filter stays in place. The pending line
 *   shows the unfiltered-list numbers. Verbose output, when enabled, is printed after
 *   each dispatch.
 *
 * @param fileType  "bin", "img", "mdf", "nrg", "chd", or "daa".
 * @param files     The image file list being filtered/selected; updated in
 *                  place by filtering and by ESC-restore.
 * @param list      Toggled by the "ls" command (from the caller) to view the
 *                  RAM cache.
 * @param state     Shared @c RefreshState forwarded to
 *                  @c handleFilteringConvert2ISO for display coordination.
 */
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files,
                         bool& list, std::shared_ptr<RefreshState> state) {

    // --- RAII: keybindings are automatically reset on any exit path ---
    ReadlineKeybindingGuard keybindingGuard;

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    std::vector<std::string> pendingIndices;      // display projection only
    std::vector<std::string> pendingPaths;        // the real staged selection
    bool hasPendingProcess = false;

    size_t currentPage = 0;
    size_t originalPage = currentPage;

    bool isFiltered = false;
    bool needsClrScrn = true;
    bool filterHistory = false;
    bool need2Sort = true;

    std::string fileExtension;
    std::string fileExtensionWithOutDots;
    std::string operation;
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        operation = "ccd2iso";
        fileExtensionWithOutDots = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileExtensionWithOutDots = "MDF";
        operation = "mdf2iso";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileExtensionWithOutDots = "NRG";
        operation = "nrg2iso";
    } else if (fileType == "chd") {
        fileExtension = ".chd";
        fileExtensionWithOutDots = "CHD";
        operation = "chd2iso";
    } else if (fileType == "daa") {
        fileExtension = ".daa/.gbi";
        fileExtensionWithOutDots = "DAA/GBI";
        operation = "daa2iso";
    } else {
        fileExtension = "";
        fileExtensionWithOutDots = "FILES";
    }

    // The unfiltered list for this file type (what ESC restores). While a
    // filter is active, source indices ("n^") refer to this list.
    const std::vector<std::string>* const cache = imageCacheFor(fileType);

    // Source list: the cache while filtered, otherwise `files` itself.
    auto sourceList = [&]() -> const std::vector<std::string>& {
        return (isFiltered && cache) ? *cache : files;
    };

    // Recomputes the "Pending Indices" display from the staged paths.
    auto syncPendingDisplay = [&] {
        pendingIndices = compressRuns(resolveIndices(pendingPaths, sourceList(), false));
        hasPendingProcess = !pendingIndices.empty();
    };

    // Same restore as ESC: back to the full list for this file type.
    auto restoreUnfilteredList = [&] {
        if (cache) files = *cache;
        isFiltered = false;
        filteringStack.clear();
        currentPage = originalPage;
        need2Sort = false;
    };

    while (true) {
        clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();

        GlobalState::g_operationCancelled.store(false);
        bool verbose = false;

        if (!isFiltered) originalPage = currentPage;

        clear_history();

        // Position the cursor on the prompt row (see the invariant note at the
        // top of this file).
        if (needsClrScrn) {
            syncPendingDisplay();
            loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list,
                                     pendingIndices, hasPendingProcess, currentPage,
                                     state);
            std::cout << "\n";
        } else {
            rewindToPromptRow();
        }

        const ReadlineAndPromptTheme pt = getPromptTheme();

        setup_custom_keybindingsForSelect();

        // Unused in convert2iso prompts.
        rl_bind_keyseq("R", rl_insert);

        std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

        // Single-line prompt; the blank separator row is printed by the
        // display step above.
        std::string prompt =
            prefix +
            std::string(UI::Palette::RL_idxGreen) + "IDX" +
            pt.primary   + " ↵ for " +
            pt.highlight + operation +
            pt.primary   + ", ? for help: " +
            pt.reset;

        // Repaints the list on terminal resize. A throwaway sort flag keeps a
        // resize from re-sorting the list.
        auto repaintList = [&] {
            bool sortNow = false;
            syncPendingDisplay();
            loadAndDisplayImageFiles(files, fileType, sortNow, isFiltered, list,
                                     pendingIndices, hasPendingProcess, currentPage,
                                     state);
        };

        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, repaintList);
        rl_startup_hook = nullptr;

        if (!rawInput) {
            if (isFiltered) {
                needsClrScrn = true;
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                need2Sort = false;
                break;
            } else {
                break;
            }
        }

        std::string inputString(rawInput.get());

        if (inputString == "\x1b") {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore original unfiltered file list.
                restoreUnfilteredList();
                needsClrScrn = true;
                continue;
            } else {
                currentPage = 0;
                need2Sort = false;
                return;
            }
        }

        if (inputString == "P" && pendingPaths.empty()) {
            hasPendingProcess = false;
            needsClrScrn = false;
            continue;
        }

        if (inputString == "clr") {
            pendingPaths.clear();
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        if (inputString[0] == ';' || inputString.find(";;") != std::string::npos) {
            needsClrScrn = false;
            continue;
        }

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        size_t totalPages = (GlobalState::ITEMS_PER_PAGE != 0)
            ? ((files.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE)
            : 0;

        bool validCommand = processPaginationHelpAndDisplay(
            inputString,
            totalPages,
            currentPage,
            isFiltered,
            needsClrScrn,
            operation,
            need2Sort,
            nullptr,
            files
        );

        if (validCommand) continue;

        if (inputString == "P") {
            // The staged selection is source indices of the FULL list, so P
            // runs against it. processInputForConversions only reads the list,
            // so while filtered we hand it a copy of the cache and leave the
            // active filter (and `files`) untouched.
            std::vector<std::string> fullList;
            if (isFiltered && cache) fullList = *cache;
            std::vector<std::string>& target = (isFiltered && cache) ? fullList : files;

            const std::string batch = joinTokens(compressRuns(
                resolveIndices(pendingPaths, target, true)));   // drops vanished files
            needsClrScrn = true;

            if (batch.empty()) {
                pendingIndices.clear();
                hasPendingProcess = false;
                continue;
            }

            processInputForConversions(
                batch,
                target,
                fileType == "mdf",
                fileType == "nrg",
                fileType == "chd",
                fileType == "daa",
                verbose);

            keybindingGuard.restore();

            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages,
                             verboseSets.operationCompleted,
                             verboseSets.operationSkipped,
                             verboseSets.operationFailed, 3);
            }

            continue;
        }

        if (inputString == "/" || (!inputString.empty() && inputString[0] == '/')) {
            handleFilteringConvert2ISO(inputString, files, operation, isFiltered,
                                       needsClrScrn, filterHistory, need2Sort, currentPage,
                                       pendingIndices, hasPendingProcess, state,
                                       repaintList);
            keybindingGuard.restore();
            continue;
        }
        else if (inputString.find(';') != std::string::npos) {
            // Bare numbers = what is on screen (`files`), "n^" = unfiltered list.
            if (stagePending(inputString, pendingPaths, hasPendingProcess, needsClrScrn,
                             files, sourceList())) {
                continue;
            }
        }
        else {
            processInputForConversions(inputString, files,
                                       (fileType == "mdf"),
                                       (fileType == "nrg"),
                                       (fileType == "chd"),
                                       (fileType == "daa"),
                                       verbose);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages,
                            verboseSets.operationCompleted,
                            verboseSets.operationSkipped,
                            verboseSets.operationFailed, 3);
                needsClrScrn = true;
            }
            keybindingGuard.restore();
        }
    }
    // Keybinding guard destructor automatically calls reset_custom_keybindingsForSelect().
}
