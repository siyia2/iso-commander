// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "isocmd/core/concurrency.h"
#include "isocmd/ui/display.h"
#include "isocmd/filter/filter.h"
#include "isocmd/input/history.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/input/readline.h"
#include "isocmd/core/sharedRefreshState.h"
#include "isocmd/util/stringManipulation.h"
#include "isocmd/ui/themes.h"
#include "isocmd/core/state.h"
#include "isocmd/ui/stringListView.h"

// Defined in printList.cpp; forward-declared so the live filter preview can
// repaint the real list on every keystroke. The list is passed as a non-owning
// StringListView, so no strings are copied.
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst = true, bool showPendingIndices = true);

// ─── Constants ───────────────────────────────────────────────────────────────
namespace AnsiEscape {
    // From the row below a prompt back onto the prompt row, cleared.
    constexpr const char* REWIND_TO_PROMPT_ROW = "\033[1A\r\033[K";
}

// ─── Prompt-row convention ───────────────────────────────────────────────────
//
// Every prompt in this file is a SINGLE line (no leading "\n"), so readline's
// own cursor bookkeeping never has to cope with a multi-line prompt.
//
// The filter prompt REPLACES the select prompt in place: it is drawn on the
// select prompt's row, under the blank separator row that is already there.
//
//  - Entry: the caller's prompt was submitted (Enter or a hotkey, which now
//    both echo a newline), so the cursor is one row below the prompt row.
//    runFilterLoop rewinds onto that row once, then calls readline.
//  - Every exit (success, Esc, Ctrl+D) leaves the cursor one row below the
//    filter prompt row, so the caller needs no fix-up and rewinds once.
//  - A failed commit rewinds onto the prompt row and prompts again.
//  - applyFilterCore does no cursor movement; runFilterLoop owns all of it.
//  - liveFilterRedisplayHook prints the blank separator row itself after the
//    live list repaint, before forcing readline to redraw its prompt.
//  - The resize path leaves the blank row to ListResizeWatcher (see
//    paintLivePreviewFrame).

// ─── Shared filtering core ───────────────────────────────────────────────────

/**
 * @brief Translates a filter match's local index (relative to the current
 * display list) into its corresponding globalIsoFileList index.
 *
 * filteringStack[lvl].originalIndices always stores fully resolved global
 * indices at every level (applyFilterCore builds each level from the previous
 * one's already-global indices). On an ISO list refresh only the top level is
 * rewritten (see refreshFilterKeepingResults), and only the top level is ever
 * read, so resolving a local index requires one lookup against the top of the
 * stack; it must not walk back through earlier levels, whose stored indices
 * may be stale.
 *
 * @param localIdx Index into the list currently being searched/displayed.
 * @return The corresponding index into globalIsoFileList.
 */
static size_t resolveGlobalIndex(size_t localIdx) {
    if (!filteringStack.empty()) {
        const auto& top = filteringStack.back().originalIndices;
        if (localIdx < top.size())
            return top[localIdx];
    }
    return localIdx;
}

/**
 * @brief Derives the short "unmount key" label used to match/display an
 * already-mounted ISO (basename with any trailing "~hash" suffix stripped).
 * Shared by @c applyFilterCore and the live filter preview so both stay in
 * sync on exactly what text is being matched against.
 *
 * @param path Full or partial path to an ISO's mount entry.
 * @return The entry's basename with any trailing "~hash" suffix removed.
 */
static std::string extractUnmountKey(const std::string& path) {
    size_t lastSlash = path.find_last_of('/');
    std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    size_t lastTilde = name.find_last_of('~');
    return (lastTilde != std::string::npos) ? name.substr(0, lastTilde) : name;
}

/**
 * @brief Converts a single source path into its searchable representation.
 *
 * When neither unmount-key nor filename-only mode is active, returns the
 * original path unchanged.
 *
 * @param path          Source path or identifier from the active list.
 * @param useNameOnly   True to match only the basename after the last '/'.
 * @param useUnmountKey True to match the derived unmount key.
 * @return Searchable string used during query evaluation.
 */
static std::string formatSearchCandidate(const std::string& path,
                                        bool useNameOnly,
                                        bool useUnmountKey) {
    if (useUnmountKey) {
        return extractUnmountKey(path);
    }
    if (!useNameOnly) {
        return path;
    }

    const size_t lastSlash = path.find_last_of('/');
    return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}

/**
 * @brief Evaluates @p searchString against @p sourceList and collects matches.
 *
 * Projects entries into searchable strings when required, runs
 * @c filterFilesIndices, and populates @p outFiltered and @p outIndices with
 * the corresponding source entries and their local indices.
 *
 * @param sourceList    Source entries to filter against.
 * @param searchString  Filter query passed to @c filterFilesIndices.
 * @param useNameOnly   True to filter by filename component only.
 * @param useUnmountKey True to filter by unmount key.
 * @param[out] outFiltered Matched source strings in result order.
 * @param[out] outIndices  Local indices into @p sourceList for each match.
 * @return True if at least one match was found; false otherwise.
 */
static bool collectFilterMatches(const std::vector<std::string>& sourceList,
                                 const std::string& searchString,
                                 bool useNameOnly,
                                 bool useUnmountKey,
                                 std::vector<std::string>& outFiltered,
                                 std::vector<size_t>& outIndices) {
    std::vector<size_t> matchedIndices;

    if (useNameOnly || useUnmountKey) {
        std::vector<std::string> derived;
        derived.reserve(sourceList.size());
        for (const auto& path : sourceList) {
            derived.push_back(formatSearchCandidate(path, useNameOnly, useUnmountKey));
        }
        matchedIndices = filterFilesIndices(derived, searchString);
    } else {
        matchedIndices = filterFilesIndices(sourceList, searchString);
    }

    if (matchedIndices.empty()) {
        return false;
    }

    outFiltered.reserve(matchedIndices.size());
    outIndices.reserve(matchedIndices.size());
    for (size_t idx : matchedIndices) {
        outFiltered.push_back(sourceList[idx]);
        outIndices.push_back(idx);
    }

    return true;
}

/**
 * @brief Commits a non-trivial filtered result set to UI and stack state.
 *
 * Resets pagination, requests a screen clear, replaces @c ctx.files, maps
 * local indices back to global database indices, and pushes a new
 * @c FilteringState entry onto @c filteringStack.
 *
 * @param searchString  Original filter query stored for state recovery.
 * @param[in,out] ctx   Filter context updated in place.
 * @param useNameOnly   Recorded in the pushed @c FilteringState.
 * @param filteredFiles Matched entries moved into @c ctx.files.
 * @param matchedIndices Local indices to resolve into global indices.
 */
static void commitFilteredResults(const std::string& searchString,
                                  FilterContext& ctx,
                                  bool useNameOnly,
                                  std::vector<std::string>&& filteredFiles,
                                  const std::vector<size_t>& matchedIndices) {
    ctx.currentPage  = 0;
    ctx.needsClrScrn = true;
    ctx.files        = std::move(filteredFiles);

    FilteringState newState;
    newState.originalIndices.reserve(matchedIndices.size());
    newState.query      = searchString;  // save query
    newState.namesOnly  = useNameOnly;
    newState.isFiltered = true;

    for (size_t idx : matchedIndices) {
        newState.originalIndices.push_back(resolveGlobalIndex(idx));
    }

    filteringStack.push_back(std::move(newState));
    ctx.isFiltered = true;
}

/**
 * @brief Executes the core filter commit: transforms source paths into
 * searchable strings (filename only, unmount keys, or raw paths), runs the
 * query, and chains the result through @c filteringStack so local indices map
 * back to global database indices. Resets pagination and marks the screen for
 * refresh.
 *
 * This is the COMMIT path, run when the user presses Enter. It is separate
 * from the live preview (see the "Live incremental filter preview" section),
 * which never touches @c filteringStack or @c ctx.files. It does no terminal
 * output or cursor movement; the caller (runFilterLoop) owns that.
 *
 * @param searchString The query to filter by (saved for state recovery).
 *        Unescaped ';' separates OR-ed groups and unescaped '&' separates
 *        AND-ed terms; a leading unescaped '!' on a term negates it.
 *        '\;', '\&', '\!' and '\\' match a literal ';', '&', '!' and '\'.
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return true if matches were found and the filter stack was updated; false
 *         if the query is empty or no matches exist. Also returns true,
 *         without pushing a new level, when the query matches every entry
 *         (the filter would change nothing).
 */
static bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {
    if (searchString.empty()) {
        return false;
    }

    const std::vector<std::string>& sourceList =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

    const bool useNameOnly   = displayConfig::toggleNamesOnly && !ctx.isUnmount;
    const bool useUnmountKey = ctx.isUnmount && !ctx.toggleFullListUmount;

    std::vector<std::string> tempFiltered;
    std::vector<size_t>      tempIndices;

    if (!collectFilterMatches(sourceList, searchString, useNameOnly, useUnmountKey,
                              tempFiltered, tempIndices)) {
        return false;
    }

    if (tempFiltered.empty()) {
        return false;
    }
    if (tempFiltered.size() == sourceList.size()) {
        return true;   // matches everything: nothing to filter
    }

    commitFilteredResults(searchString, ctx, useNameOnly,
                          std::move(tempFiltered), tempIndices);
    return true;
}

// ─── History helpers ─────────────────────────────────────────────────────────

/**
 * @brief Saves a search query to readline history.
 *
 * @param query The query string to save.
 * @param filterHistory Reference to the filter-history flag.
 * @param alreadyLoaded If true, skips loadHistory (caller loaded before the prompt).
 */
static void saveQueryToHistory(const std::string& query, bool& filterHistory, bool alreadyLoaded = false) {
    filterHistory = true;
    if (!alreadyLoaded)
        loadHistory(filterHistory);
    add_history(query.c_str());
    saveHistory(filterHistory);
    clear_history();
}

// ─── Live incremental filter preview ─────────────────────────────────────────
//
// The preview is a temporary view of the list while the user edits the query;
// it does not commit results or modify ctx.files/filteringStack. Enter still
// uses applyFilterCore() as the single commit path.
//
// Key invariants:
//  - Matches are represented as indices and rendered through StringListView,
//    avoiding copies of the source strings.
//  - The preview is unpaginated. UnpaginatedScope restores ITEMS_PER_PAGE on
//    every exit path, and PreviewStackGuard removes its temporary index map.
//  - The searchable labels and lowercase corpus are cached once per prompt.
//    The source list and name/unmount display modes remain stable until commit.
//  - A strict query extension can reuse the previous matches only for a
//    single OR-group without negation. Backspacing, edits, OR queries and
//    negation require a full search; an incomplete trailing escape is not a
//    safe prefix either.
//  - The filter prompt is a single line. After printList() repaints the
//    screen, the hook prints the blank separator row itself and then forces a
//    prompt redraw, because readline's display cache is stale after the
//    repaint. Unchanged query text uses normal redisplay (for example, when
//    moving the cursor).
//
// Live repainting is disabled above LIVE_FILTER_LIMIT to avoid repeatedly
// redrawing large lists. The prompt then retains the normal Enter-to-commit
// behavior, while resize handling can still use the caller's repaint callback.

namespace {

    /**
     * @brief Result of one live-preview filter pass: indices only, no strings
     * copied.
     *
     * @c local indexes the preview's source list (or its derived label cache —
     * both share indexing) and is what the StringListView is built from.
     * @c global holds the same entries translated back to globalIsoFileList
     * indices (see @c resolveGlobalIndex) and feeds the temporary filteringStack
     * entry used for the "[N]^" tags. When @c identity is true the preview shows
     * the whole source list unchanged (empty query) and both vectors are empty.
     */
    struct LivePreviewResult {
        std::vector<size_t> local;
        std::vector<size_t> global;
        bool                identity = false;
    };

    /**
     * @brief All state for one live filter-preview session (one
     * @c runFilterLoop() invocation): the source list and how to derive its
     * searchable labels, the printList() wiring needed to repaint the real list,
     * the per-invocation derived/lowercase search caches (see
     * @c primeLivePreviewCaches), the previous query and its matches (for
     * incremental narrowing), and bookkeeping for the repaint decision.
     */
    struct LiveFilterPreview {
        const std::vector<std::string>* sourceList     = nullptr;
        bool                             hasLastMatches = false; // lastMatches is valid for lastQuery
        bool                             useNameOnly    = false;
        bool                             useUnmountKey  = false;

        // Set up per runFilterLoop() call; mirror the actual current ctx state.
        bool*   actualIsFiltered = nullptr;
        size_t* actualCurrentPage = nullptr;

        // Wiring needed to call the real printList().
        std::string                   listType{};
        std::string                   listSubType{};
        std::vector<std::string>*     pendingIndices    = nullptr;
        bool*                         hasPendingProcess = nullptr;
        std::shared_ptr<RefreshState> state = nullptr;

        // Derived-label ("name only" / unmount-key) and lowercased search corpora
        // for `*sourceList`. Both depend only on sourceList and the toggles above,
        // never on the in-progress query, so they are computed exactly once per
        // runFilterLoop() invocation (see primeLivePreviewCaches) instead of per
        // keystroke.
        bool                      hasDerivedCache = false; // true => search derivedCache, not *sourceList
        std::vector<std::string>  derivedCache;
        std::vector<std::string>  lowerCache;             // lowercased derivedCache (or *sourceList if !hasDerivedCache)

        // Incremental-narrowing state, reset at the start of every readline()
        // call. lastMatches holds the previous frame's local match indices (into
        // the searchable list) and is meaningful only while hasLastMatches is true.
        std::string lastQuery;
        std::vector<size_t> lastMatches;

        bool        primed        = false;  // a frame has been drawn for the current readline() call
        bool        everRepainted = false;  // a real repaint has happened at least once this runFilterLoop call
        bool        active        = false;  // a readline() call is currently in flight
    };

    LiveFilterPreview g_livePreview;

    /**
     * @brief True once sourceList + the printList() wiring are set up and are
     * within the size cap, regardless of whether a readline() call is in flight.
     * Used to decide whether to prime the derived/lowercase caches before the
     * runFilterLoop while-loop starts.
     *
     * @c GlobalState::LIVE_FILTER_LIMIT == 0 is a deliberate kill-switch: it
     * always disables live filtering, including for an empty source list (which
     * would otherwise pass the size check trivially).
     *
     * @return true if the live preview has everything it needs to run safely.
     */
    bool livePreviewConfigured() {
        if (GlobalState::LIVE_FILTER_LIMIT == 0) return false;

        return g_livePreview.sourceList
            && !g_livePreview.listType.empty()
            && g_livePreview.pendingIndices
            && g_livePreview.hasPendingProcess
            && g_livePreview.state
            && g_livePreview.sourceList->size() <= GlobalState::LIVE_FILTER_LIMIT;
    }

    /**
     * @brief True while a readline() call is in flight and the live preview is
     * fully configured, i.e. exactly when @c liveFilterRedisplayHook should
     * recompute and repaint rather than falling back to plain @c rl_redisplay().
     *
     * @return true if the live main-list repaint should run for this frame.
     */
    bool liveMainListEnabled() {
        return g_livePreview.active && livePreviewConfigured();
    }

    /**
     * @brief Derives the text searched/shown for one source entry, according to
     * the current @c useUnmountKey / @c useNameOnly toggles.
     *
     * @param path Full source-list entry (a path) to derive the label from.
     * @return The unmount key, the basename, or @p path unchanged, depending on
     *         which toggle (if any) is active.
     */
    std::string livePreviewLabel(const std::string& path) {
        if (g_livePreview.useUnmountKey) return extractUnmountKey(path);
        if (g_livePreview.useNameOnly) {
            size_t lastSlash = path.find_last_of('/');
            return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
        }
        return path;
    }

    /**
     * @brief Builds g_livePreview.derivedCache / lowerCache exactly once from
     * the current sourceList + useNameOnly/useUnmountKey toggles.
     *
     * Must be called after those fields are set (see runFilterLoop) and before
     * the first keystroke is processed. The result stays valid for every
     * readline() call and keystroke within that invocation: sourceList cannot
     * change until a filter is actually committed, and a successful commit
     * immediately ends the loop, so there is no window in which a stale cache
     * could be read.
     *
     * lowerCache is a full lowercased copy of the searchable list and stays
     * allocated for the whole prompt; it trades memory for not lowercasing every
     * entry on every keystroke.
     */
    void primeLivePreviewCaches() {
        const std::vector<std::string>& source = *g_livePreview.sourceList;

        g_livePreview.hasDerivedCache = g_livePreview.useNameOnly || g_livePreview.useUnmountKey;

        g_livePreview.derivedCache.clear();
        if (g_livePreview.hasDerivedCache) {
            g_livePreview.derivedCache.reserve(source.size());
            for (const auto& path : source)
                g_livePreview.derivedCache.push_back(livePreviewLabel(path));
        }

        const std::vector<std::string>& searchable =
            g_livePreview.hasDerivedCache ? g_livePreview.derivedCache : source;

        g_livePreview.lowerCache.clear();
        g_livePreview.lowerCache.reserve(searchable.size());
        for (const auto& s : searchable) {
            std::string lower = s;
            toLowerInPlace(lower);
            g_livePreview.lowerCache.push_back(std::move(lower));
        }
    }

    /**
     * @brief Computes the "would-be" match set for the in-progress query without
     * touching filteringStack — mirrors applyFilterCore's source resolution and
     * name/unmount-key handling, but is purely a preview. Returns indices only;
     * the caller wraps them in a zero-copy StringListView.
     *
     * Reads from the per-invocation caches (primeLivePreviewCaches) instead of
     * rebuilding them per keystroke, and narrows incrementally when possible: if
     * @p query strictly extends @c g_livePreview.lastQuery (neither contains ';',
     * and @p query has no negated term), only the previous frame's matches are
     * re-tested via @c filterFilesIndicesSubset; otherwise the whole list is
     * searched. '&' (AND) queries are eligible for the incremental path; negated
     * ('!') queries are not, because extending a negated term widens the match
     * set. Because it reads the previous query, the caller must update
     * @c lastQuery only AFTER this returns.
     *
     * Identity (whole source list) is returned for an empty query, a query that
     * parses to no terms, and a query whose last OR-branch is still empty
     * ("clon;"): the new branch has no term yet, so it matches everything until
     * the user types one. Enter is unaffected; the commit path never sees this.
     *
     * Side effects: updates @c lastMatches / @c hasLastMatches (cleared for an
     * identity result, set to this result otherwise).
     *
     * @param query The in-progress (uncommitted) query text from the readline
     *        input buffer.
     * @return The matching local indices plus their globalIsoFileList
     *         translation, or an identity result (whole source list).
     */
    LivePreviewResult computeLivePreviewMatches(const std::string& query)
    {
        LivePreviewResult result;

        // Parse once; reused below for the identity check and the negation gate.
        bool trailingEmptyGroup = false;
        const std::vector<QueryGroup> groups = buildQueryGroups(query, &trailingEmptyGroup);

        // An empty query, one that parses to no terms (e.g. "\", ";", "&", "!",
        // ";;"), or one whose last OR-branch is still empty ("clon;") matches
        // everything, so show the plain source list (identity), not a "filtered"
        // view with index tags. Not when a negated term exists: negation is
        // global, so its exclusion still applies ("!a;" shows the same list as
        // "!a").
        if (query.empty() || groups.empty() ||
            (trailingEmptyGroup && !groupsHaveNegation(groups))) {
            result.identity = true;
            g_livePreview.lastMatches.clear();
            g_livePreview.hasLastMatches = false;
            return result;
        }

        const std::vector<std::string>& source = *g_livePreview.sourceList;
        const std::vector<std::string>& searchable =
            g_livePreview.hasDerivedCache
                ? g_livePreview.derivedCache
                : source;

        // Strict prefix extension of the previous query, single OR-group only
        // (no ';'): the only case where "matches(new) is a subset of
        // matches(old)" is guaranteed under smart-case matching. '&' is safe
        // here because it only ever adds constraints to the one group. A
        // previous query ending in an unpaired backslash is excluded: its
        // trailing '\' was ignored by the parser, but the next character can
        // turn it into an escape and change the meaning of the term. Negation
        // is excluded too: "!a" -> "!ab" WIDENS the match set.
        const bool progressiveExtension =
            !g_livePreview.lastQuery.empty() &&
            !groupsHaveNegation(groups) &&
            !hasDanglingBackslash(g_livePreview.lastQuery) &&
            query.size() > g_livePreview.lastQuery.size() &&
            g_livePreview.lastQuery.find(';') == std::string::npos &&
            query.find(';') == std::string::npos &&
            query.compare(
                0,
                g_livePreview.lastQuery.size(),
                g_livePreview.lastQuery) == 0;

        if (progressiveExtension && g_livePreview.hasLastMatches) {
            result.local = filterFilesIndicesSubset(
                searchable,
                g_livePreview.lastMatches,
                query,
                &g_livePreview.lowerCache);
        } else {
            result.local = filterFilesIndices(
                searchable,
                query,
                &g_livePreview.lowerCache);
        }

        g_livePreview.lastMatches = result.local;
        g_livePreview.hasLastMatches = true;

        result.global.reserve(result.local.size());

        for (size_t idx : result.local)
            result.global.push_back(resolveGlobalIndex(idx));

        return result;
    }

    /**
     * @brief RAII guard that forces printList() into its unpaginated path
     * (GlobalState::ITEMS_PER_PAGE == 0) for one live-preview repaint and
     * restores the user's real setting on every exit path, including exceptions.
     *
     * Safe against concurrent repaints because runSharedFilterFlow suppresses
     * pending async refreshes (g_suppressPendingRefresh) for the whole prompt.
     */
    struct UnpaginatedScope {
        const std::size_t saved = GlobalState::ITEMS_PER_PAGE;
        UnpaginatedScope() { GlobalState::ITEMS_PER_PAGE = 0; }
        ~UnpaginatedScope() { GlobalState::ITEMS_PER_PAGE = saved; }
        UnpaginatedScope(const UnpaginatedScope&) = delete;
        UnpaginatedScope& operator=(const UnpaginatedScope&) = delete;
    };

    /**
     * @brief Temporary filteringStack entry so printList() draws "[N]^" tags
     * relative to the full list. Popped on every exit path. Pass push=false for
     * an identity (empty-query) frame.
     */
    struct PreviewStackGuard {
        bool pushed;
        PreviewStackGuard(std::vector<size_t>&& globals, bool push) : pushed(push) {
            if (!pushed) return;
            FilteringState s;
            s.originalIndices = std::move(globals);
            filteringStack.push_back(std::move(s));
        }
        ~PreviewStackGuard() { if (pushed) filteringStack.pop_back(); }
        PreviewStackGuard(const PreviewStackGuard&) = delete;
        PreviewStackGuard& operator=(const PreviewStackGuard&) = delete;
    };

    /**
     * @brief Computes the matches for @p query and paints the unpaginated live
     * preview frame (clear + list). Does NOT touch Readline, and does NOT print
     * the blank separator row below the list: the cursor is left on the row
     * directly after the list.
     *
     * Shared by two callers, both on the Readline thread:
     *  - @c liveFilterRedisplayHook, which prints the blank separator row and
     *    then calls @c rl_forced_update_display();
     *  - the resize repaint in @c runFilterLoop, after which the
     *    @c ListResizeWatcher prints the blank line and redraws the prompt and
     *    the half-typed input itself.
     *
     * Sets @c lastQuery after computing, so incremental narrowing keeps working
     * (see @c computeLivePreviewMatches, which reads the previous value). A resize
     * repaint passes the unchanged query, which is not longer than @c lastQuery,
     * so it takes the full-search path and leaves @c lastQuery correct for the
     * next keystroke. Also sets @c everRepainted, so a later cancel redraws the
     * normal paginated list.
     *
     * @param query The in-progress (uncommitted) query text, normally
     *        @c rl_line_buffer.
     */
    void paintLivePreviewFrame(const std::string& query) {
        LivePreviewResult preview = computeLivePreviewMatches(query);
        g_livePreview.lastQuery = query;

        size_t previewPage = 0;   // ignored by printList() when pagination is off
        {
            const std::vector<std::string>& source = *g_livePreview.sourceList;
            const StringListView view = preview.identity
                ? StringListView(source)
                : StringListView(source, preview.local);

            PreviewStackGuard stackGuard(std::move(preview.global), !preview.identity);
            UnpaginatedScope unpaginated;
            printList(view,
                      g_livePreview.listType,
                      g_livePreview.listSubType,
                      *g_livePreview.pendingIndices,
                      *g_livePreview.hasPendingProcess,
                      previewPage,
                      g_livePreview.state,
                      true,
                      false);
        }   // unpaginated restored, then stackGuard pops

        g_livePreview.everRepainted = true;
    }

    /**
     * @brief Installed as rl_redisplay_function for the lifetime of one
     * runFilterLoop() call; invoked by readline on (almost) every keystroke,
     * including backspace, so the real list narrows and widens live as the query
     * changes.
     *
     * Per invocation: (1) falls back to plain rl_redisplay() if the preview is
     * not enabled, if the user is typing forward onto an unpaired trailing
     * backslash (the next character may change what it escapes), or if the query
     * text is unchanged (e.g. cursor movement); (2) otherwise calls
     * paintLivePreviewFrame(), which computes the match set, records the query as
     * @c lastQuery, and repaints the real list unpaginated through printList()
     * with a zero-copy StringListView; (3) prints the blank separator row (the
     * single-line prompt no longer carries it as a leading "\n"); (4) asks
     * readline to redraw its prompt from scratch via rl_forced_update_display().
     *
     * See paintLivePreviewFrame() for the temporary filteringStack entry and the
     * unpaginated scope, which are released before the readline redraw.
     */
    void liveFilterRedisplayHook() {
        if (!liveMainListEnabled()) {
            rl_redisplay();
            return;
        }

        const std::string query(rl_line_buffer ? rl_line_buffer : "");
        const bool isFirstFrameOfThisCall = !g_livePreview.primed;
        g_livePreview.primed = true;

        // An unpaired trailing backslash may escape the next '&', ';', '!' or '\'.
        // Hold the list only while the user is typing forward onto it.
        if (hasDanglingBackslash(query)) {
            const bool typedForward =
                query.size() > g_livePreview.lastQuery.size() &&
                query.compare(0, g_livePreview.lastQuery.size(),
                              g_livePreview.lastQuery) == 0;
            if (typedForward) {
                rl_redisplay();
                return;
            }
        }

        if (!isFirstFrameOfThisCall && query == g_livePreview.lastQuery) {
            // Text unchanged (e.g. cursor movement): ordinary redisplay is correct.
            rl_redisplay();
            return;
        }

        paintLivePreviewFrame(query);

        // Blank separator row between the list and the (single-line) prompt.
        std::cout << "\n";

        // The whole screen was repainted under readline; redraw prompt + input fresh.
        rl_forced_update_display();
    }

    /**
     * @brief RAII guard that installs/tears down the live preview hook for the
     * lifetime of one runFilterLoop() call. Guarantees rl_redisplay_function is
     * restored on every exit path — normal break, loop exit, or an exception
     * propagating out of a keystroke (e.g. filterFilesIndices rethrowing a worker
     * exception).
     */
    struct LivePreviewGuard {
        LivePreviewGuard() { rl_redisplay_function = liveFilterRedisplayHook; }
        ~LivePreviewGuard() {
            g_livePreview = LiveFilterPreview{}; // drop all pointers/shared_ptr, reset flags + caches
            rl_redisplay_function = rl_redisplay;
        }
        LivePreviewGuard(const LivePreviewGuard&) = delete;
        LivePreviewGuard& operator=(const LivePreviewGuard&) = delete;
    };

} // namespace

// ─── Interactive / quick filter driver ───────────────────────────────────────

/**
 * @brief Configures global live-preview state and primes caches if enabled.
 *
 * Populates @c g_livePreview from @p ctx and @p previewSource, then primes
 * internal preview caches when live preview is configured.
 *
 * @param ctx           Filter context supplying display flags and list metadata.
 * @param previewSource Source item list used for live-preview filtering.
 */
static void configureLivePreview(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource)
{
    g_livePreview.sourceList        = &previewSource;
    g_livePreview.useNameOnly       = displayConfig::toggleNamesOnly && !ctx.isUnmount;
    g_livePreview.useUnmountKey     = ctx.isUnmount && !ctx.toggleFullListUmount;
    g_livePreview.actualIsFiltered  = &ctx.isFiltered;
    g_livePreview.actualCurrentPage = &ctx.currentPage;
    g_livePreview.listType          = ctx.listType;
    g_livePreview.listSubType       = ctx.listSubType;
    g_livePreview.pendingIndices    = ctx.pendingIndices;
    g_livePreview.hasPendingProcess = ctx.hasPendingProcess;
    g_livePreview.state             = ctx.state;

    if (livePreviewConfigured())
        primeLivePreviewCaches();
}

/**
 * @brief Repaints the display in response to a terminal resize during filtering.
 *
 * Selects the repaint strategy matching the active display mode:
 * - live preview running -> repaint the unpaginated preview of the current query;
 * - preview off (list above LIVE_FILTER_LIMIT or not configured) but the
 *   printList wiring is available -> repaint the normal paginated list
 *   straight from @p previewSource;
 * - otherwise -> caller's @c ctx.repaintList fallback (draw only, no
 *   readline interaction).
 *
 * @param ctx           Filter context and optional normal-list resize callback.
 * @param previewSource Active source list displayed prior to committing a filter.
 */
static void repaintOnResize(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource)
{
    if (liveMainListEnabled()) {
        paintLivePreviewFrame(rl_line_buffer ? rl_line_buffer : "");
    } else if (ctx.pendingIndices && ctx.hasPendingProcess && ctx.state &&
               !ctx.listType.empty()) {
        // Reproduce the normal (paginated) display the user is looking at.
        // Use previewSource, NOT ctx.files: before a commit ctx.files
        // (the caller's filteredFiles) may be empty/stale, while the
        // actually displayed list is *ctx.sourceOverride.
        size_t page = ctx.currentPage;
        printList(StringListView(previewSource),
                  ctx.listType,
                  ctx.listSubType,
                  *ctx.pendingIndices,
                  *ctx.hasPendingProcess,
                  page,
                  ctx.state,
                  /*clearFirst=*/true,
                  /*showPendingIndices=*/true);
    } else if (ctx.repaintList) {
        // Fallback: caller-supplied repaint. It must draw only (no readline
        // interaction). Restore the line with clear_undo=1.
        const std::string savedLine(rl_line_buffer ? rl_line_buffer : "");
        const int savedPoint = rl_point;
        ctx.repaintList();
        rl_replace_line(savedLine.c_str(), 1);
        rl_point = std::min(savedPoint, rl_end);
    }
}

/**
 * @brief Resets incremental live-preview state at the start of a prompt read.
 */
static void resetLivePreviewIteration()
{
    g_livePreview.lastQuery.clear();
    g_livePreview.lastMatches.clear();
    g_livePreview.primed = false;
    g_livePreview.active = true;
}

/**
 * @brief Handles prompt cancellation via EOF (Ctrl+D) or Escape.
 *
 * Normalizes cursor placement one row below the prompt row, flags a screen
 * clear if live preview repainted the list, and clears readline history.
 *
 * @param raw Line returned by readline (null on EOF or starting with ASCII 27 for Esc).
 * @param ctx Filter context updated with @c needsClrScrn if preview repainted.
 */
static void handleFilterCancel(const RlLine& raw, FilterContext& ctx)
{
    // Esc echoed its newline (exit_handler ends with rl_newline).
    // Ctrl+D (EOF) does not, so add it: every exit leaves the cursor
    // one row below the prompt row.
    if (!raw) {
        std::cout << "\n";
    }
    // The live preview painted an unpaginated list over the screen.
    // Make the caller redo its normal (paginated) refresh.
    if (g_livePreview.everRepainted)
        ctx.needsClrScrn = true;

    // EOF (Ctrl+D) or Esc - exit
    clear_history();
}

/**
 * @brief Runs the interactive filter prompt and commits a query on Enter.
 *
 * Live preview is enabled only when the caller supplies the required list
 * rendering state and the source list is within LIVE_FILTER_LIMIT. Otherwise
 * the prompt uses Enter-to-commit filtering. Each readline iteration resets
 * incremental-preview state and installs a resize repaint appropriate to the
 * current display mode.
 *
 * @p promptText must be a single line (no leading "\n").
 *
 * Cursor contract: on entry the cursor is one row below the caller's prompt
 * row (that prompt was just submitted). The loop rewinds onto that row, so the
 * filter prompt replaces it in place. On every exit (success, Esc, Ctrl+D) the
 * cursor is one row below the filter prompt row. A failed commit rewinds onto
 * the prompt row and prompts again.
 *
 * Esc or EOF cancels the prompt. A preview that has repainted the list marks
 * the screen for a normal paginated redraw before control returns. A failed
 * commit (empty query or no matches) leaves the prompt active; a successful
 * commit saves the query, invokes @p onSuccess and exits.
 *
 * @param promptText   Single-line prompt text passed to readlineWithResize().
 * @param ctx          Filter context and optional normal-list resize callback.
 * @param onSuccess    Callback invoked after a successful filter commit.
 * @param onEmptyInput Retained callback parameter; currently not invoked.
 */
static void runFilterLoop(const std::string& promptText, FilterContext& ctx,
    const std::function<void()>& onSuccess,
    const std::function<void()>& onEmptyInput = nullptr)
{
    auto tryFilter = [&](const std::string& query) -> bool {
        return applyFilterCore(query, ctx);
    };

    auto defaultEmptyInput = [&]() {
        ctx.needsClrScrn = false;
    };

    const auto& handleEmpty = onEmptyInput ? onEmptyInput : defaultEmptyInput;
    (void)handleEmpty;   // bound but not invoked by the loop body (see doc comment)

    // Draw the filter prompt on the row of the prompt that was just submitted.
    std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;
    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    // Installed for the whole loop; restores rl_redisplay_function on every exit path.
    LivePreviewGuard livePreviewGuard;

    const std::vector<std::string>& previewSource =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;
    configureLivePreview(ctx, previewSource);

    const auto resizeRepaint = [&] {
        repaintOnResize(ctx, previewSource);
    };

    while (true) {
        resetLivePreviewIteration();

        RlLine raw = readlineWithResize(promptText, resizeRepaint);

        g_livePreview.active = false;

        //---- Robust handling of FilterTerms prompt ----
        if (!raw || raw.get()[0] == 27) {
            handleFilterCancel(raw, ctx);
            break;
        }

        std::string query(raw.get());
        if (tryFilter(query)) {
            saveQueryToHistory(query, ctx.filterHistory, true);
            onSuccess();
            break;
        }

        // Failed commit (empty query or no matches): back onto the prompt row.
        std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 * @details Handles only the exact "/" command. Builds the filter context,
 * suppresses asynchronous list refreshes while the nested readline prompt
 * owns the terminal, then delegates to @c runFilterLoop.
 *
 * While the nested FilterTerms prompt is active, pending asynchronous UI
 * refreshes (see @c GlobalState::g_pendingRefreshKind /
 * @c checkPendingRefresh) are suppressed via
 * @c GlobalState::g_suppressPendingRefresh. This is necessary because
 * @c checkPendingRefresh runs from Readline's event hook and would otherwise
 * repaint the ISO list mid-call while this different, nested @c readline()
 * prompt owns the terminal — desyncing Readline's internal cursor/line state
 * and crashing. The flag is set for the duration of this function via an RAII
 * guard (a @c shared_ptr<void> with a custom deleter, to avoid a dedicated
 * named type) and cleared on every exit path, including early return.
 * Suppressed refresh requests are not dropped; @c checkPendingRefresh leaves
 * the pending request in place and retries once suppression is lifted. The
 * same suppression is what makes the live preview's temporary ITEMS_PER_PAGE
 * override (UnpaginatedScope) safe.
 *
 * The filter prompt is a single line that replaces the caller's prompt in
 * place; see @c runFilterLoop for the cursor contract. The caller must have
 * submitted its own prompt (cursor one row below it) before calling.
 *
 * Live preview is opt-in: @p cfg must provide the list type, pending-selection
 * state and shared refresh state required by printList(). Otherwise filtering
 * remains Enter-to-commit only.
 *
 * @param inputString  Raw input from the user; must be exactly @c "/" to trigger filtering.
 * @param cfg          Configuration struct with all state pointers and display options.
 *                     All non-optional pointer fields must be non-null.
 * @return @c true if @p inputString was recognised as a filter command and
 *         handled, @c false otherwise.
 */
static bool runSharedFilterFlow(const std::string& inputString, const FilterCallConfig& cfg)
{
    if (inputString != "/")
        return false;

    GlobalState::g_suppressPendingRefresh.store(true);
    std::shared_ptr<void> suppressGuard(nullptr, [](void*) {
        GlobalState::g_suppressPendingRefresh.store(false);
    });

    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
    reset_custom_keybindingsForSelect();
    rl_bind_keyseq("\\e", exit_handler);

    const ReadlineAndPromptTheme ft = getFilterTheme("", false);
    // Single-line prompt; runFilterLoop positions the cursor on the prompt row.
    const std::string prompt = std::format("{}Filter: {}", ft.filter, ft.reset);

    FilterContext ctx {
        *cfg.files,
        *cfg.isFiltered,
        *cfg.needsClrScrn,
        *cfg.filterHistory,
        *cfg.currentPage
    };
    if (cfg.sourceOverride) {
        ctx.sourceOverride       = cfg.sourceOverride;
        ctx.isUnmount            = cfg.isUnmount;
        ctx.toggleFullListUmount = cfg.toggleFullList;
    }

    // Live main-list rendering is opt-in: only wired up when the caller
    // supplied everything printList() needs (see FilterContext).
    ctx.listType          = cfg.listType;
    ctx.listSubType       = cfg.listSubType;
    ctx.pendingIndices    = cfg.pendingIndices;
    ctx.hasPendingProcess = cfg.hasPendingProcess;
    ctx.state             = cfg.state;

    // Normal-list repaint, used by the resize watcher when the live preview is off.
    ctx.repaintList       = cfg.repaintList;

    auto onEmptyInput = [&]() {
        *cfg.needsClrScrn = *cfg.isFiltered;
    };

    auto onSort = [&]() {
        if (cfg.need2Sort) *cfg.need2Sort = true;
    };

    runFilterLoop(prompt, ctx, onSort, onEmptyInput);
    return true;
}

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
                           FilterEntryContext ctx)
{
    const std::vector<std::string>& baseSource =
        (ctx.isFiltered && *ctx.isFiltered)
            ? filteredFiles
            : (isUnmount ? isoDirs : GlobalState::globalIsoFileList);

    FilterCallConfig cfg {
        .files          = &filteredFiles,
        .sourceOverride = &baseSource,
        .operation      = operation,
        .operationColor = operationColor,
        .isFiltered     = ctx.isFiltered,
        .needsClrScrn   = ctx.needsClrScrn,
        .filterHistory  = ctx.filterHistory,
        .currentPage    = ctx.currentPage,
        .isUnmount      = isUnmount,
        .toggleFullList = displayConfig::toggleFullListUmount
    };

    if (ctx.state) {
        cfg.listType          = isUnmount ? "MOUNTED_ISOS" : "ISO_FILES";
        cfg.listSubType       = isUnmount ? "" : ctx.state->listSubtype;
        cfg.pendingIndices    = &ctx.state->pendingIndices;
        cfg.hasPendingProcess = &ctx.state->hasPendingProcess;
        cfg.state             = ctx.state;
    }
    cfg.repaintList = std::move(ctx.repaintList);

    return runSharedFilterFlow(inputString, cfg);
}

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
                              FilterEntryContext ctx)
{
    FilterCallConfig cfg {
        .files          = &files,
        .operation      = operation,
        .operationColor = UI::Palette::Orange,
        .isFiltered     = ctx.isFiltered,
        .needsClrScrn   = ctx.needsClrScrn,
        .filterHistory  = ctx.filterHistory,
        .need2Sort      = ctx.need2Sort,
        .currentPage    = ctx.currentPage
    };

    if (ctx.state) {
        cfg.listType          = "IMAGE_FILES";
        cfg.listSubType       = "convert2iso";
        cfg.pendingIndices    = ctx.pendingIndices;
        cfg.hasPendingProcess = ctx.hasPendingProcess;
        cfg.state             = ctx.state;
    }
    cfg.repaintList = std::move(ctx.repaintList);

    runSharedFilterFlow(inputString, cfg);
}
