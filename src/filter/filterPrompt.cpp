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
#include "isocmd/core/sharedRefreshState.h"
#include "isocmd/core/lists.h"
#include "isocmd/core/uiState.h"
#include "isocmd/filter/filter.h"
#include "isocmd/input/history.h"
#include "isocmd/input/readline.h"
#include "isocmd/ui/display.h"
#include "isocmd/ui/readlineResize.h"
#include "isocmd/ui/stringListView.h"
#include "isocmd/ui/themes.h"
#include "isocmd/util/stringManipulation.h"

// Defined in printList.cpp; forward-declared so the live filter preview can
// repaint the real list on every keystroke. The list is passed as a non-owning
// StringListView, so no strings are copied.
void printList(
    const StringListView& items,
    const std::string& listType,
    const std::string& listSubType,
    std::vector<std::string>& pendingIndices,
    bool& hasPendingProcess,
    size_t& currentPage,
    std::shared_ptr<RefreshState> state,
    bool clearFirst = true,
    bool showPendingIndices = true);

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

// ─── History helpers ─────────────────────────────────────────────────────────

/**
 * @brief Saves a search query to readline history.
 *
 * @param query The query string to save.
 * @param filterHistory Reference to the filter-history flag.
 * @param alreadyLoaded If true, skips loadHistory (caller loaded before prompt).
 */
static void saveQueryToHistory(
    const std::string& query,
    bool& filterHistory,
    bool alreadyLoaded = false)
{
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
//    screen, the hook prints the blank separator row itself and then forces
//    a prompt redraw, because readline's display cache is stale after repaint.
//
// Live repainting is disabled above LIVE_FILTER_LIMIT to avoid repeatedly
// redrawing large lists. The prompt then retains normal Enter-to-commit
// behavior, while resize handling can still use the caller's repaint callback.

namespace {

    /**
     * @brief Result of one live-preview filter pass: indices only, no strings
     * copied.
     *
     * @c local indexes the preview's source list (or its derived label cache —
     * both share indexing) and is what the StringListView is built from.
     * @c global holds the same entries translated back to globalIsoFileList
     * indices and feeds the temporary filteringStack entry used for the "[N]^"
     * tags. When @c identity is true the preview shows the whole source list
     * unchanged (empty query) and both vectors are empty.
     */
    struct LivePreviewResult {
        std::vector<size_t> local;
        std::vector<size_t> global;
        bool identity = false;
    };

    /**
     * @brief All state for one live filter-preview session.
     *
     * Stores the source list, searchable-label caches, printList() wiring,
     * previous query and matches for incremental narrowing, and repaint state.
     */
    struct LiveFilterPreview {
        const std::vector<std::string>* sourceList = nullptr;
        bool hasLastMatches = false;
        bool useNameOnly = false;
        bool useUnmountKey = false;

        // Set up per runFilterLoop() call; mirror the actual current ctx state.
        bool* actualIsFiltered = nullptr;
        size_t* actualCurrentPage = nullptr;

        // Wiring needed to call the real printList().
        std::string listType{};
        std::string listSubType{};
        std::vector<std::string>* pendingIndices = nullptr;
        bool* hasPendingProcess = nullptr;
        std::shared_ptr<RefreshState> state = nullptr;

        // Derived-label and lowercased search corpora for *sourceList.
        bool hasDerivedCache = false;
        std::vector<std::string> derivedCache;
        std::vector<std::string> lowerCache;

        // Incremental-narrowing state, reset at the start of every readline().
        std::string lastQuery;
        std::vector<size_t> lastMatches;

        bool primed = false;
        bool everRepainted = false;
        bool active = false;
    };

    LiveFilterPreview g_livePreview;

    /**
     * @brief True once sourceList and printList() wiring are configured and
     * within the live-filter size cap.
     *
     * LIVE_FILTER_LIMIT == 0 deliberately disables live filtering, including
     * for an empty source list.
     */
    bool livePreviewConfigured() {
        if (GlobalState::LIVE_FILTER_LIMIT == 0)
            return false;

        return g_livePreview.sourceList
            && !g_livePreview.listType.empty()
            && g_livePreview.pendingIndices
            && g_livePreview.hasPendingProcess
            && g_livePreview.state
            && g_livePreview.sourceList->size() <= GlobalState::LIVE_FILTER_LIMIT;
    }

    /**
     * @brief True while readline is active and live preview is fully configured.
     */
    bool liveMainListEnabled() {
        return g_livePreview.active && livePreviewConfigured();
    }

    /**
     * @brief Derives the text searched/shown for one source entry.
     *
     * @param path Full source-list entry to derive the label from.
     * @return The unmount key, basename, or original path.
     */
    std::string livePreviewLabel(const std::string& path) {
        if (g_livePreview.useUnmountKey)
            return extractUnmountKey(path);

        if (g_livePreview.useNameOnly) {
            const size_t lastSlash = path.find_last_of('/');
            return (lastSlash != std::string::npos)
                ? path.substr(lastSlash + 1)
                : path;
        }

        return path;
    }

    /**
     * @brief Builds the derived and lowercase search caches once per prompt.
     *
     * Must be called after sourceList and the display-mode flags are set.
     * The caches remain valid for the duration of the prompt.
     */
    void primeLivePreviewCaches() {
        const std::vector<std::string>& source = *g_livePreview.sourceList;

        g_livePreview.hasDerivedCache =
            g_livePreview.useNameOnly || g_livePreview.useUnmountKey;

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
     * @brief Computes the would-be match set without touching filteringStack.
     *
     * Uses the per-invocation caches and incrementally narrows the previous
     * result when the query is a strict extension of the previous query,
     * contains no OR separator, and has no negated term.
     *
     * Identity is returned for an empty query, a query that parses to no
     * terms, or a query whose last OR-branch is still empty, unless a negated
     * term exists.
     *
     * Updates lastMatches and hasLastMatches. The caller updates lastQuery
     * only after this function returns.
     */
    LivePreviewResult computeLivePreviewMatches(const std::string& query) {
        LivePreviewResult result;

        bool trailingEmptyGroup = false;
        const std::vector<QueryGroup> groups =
            buildQueryGroups(query, &trailingEmptyGroup);

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

        // A strict prefix extension of a single OR-group is safe to narrow.
        // '&' only adds constraints. A dangling previous escape is excluded
        // because the next character can change its parsing.
        // Negation is excluded because extending a negated term can widen
        // the match set.
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
     * @brief Forces printList() into its unpaginated path for one repaint.
     *
     * Restores ITEMS_PER_PAGE on every exit path, including exceptions.
     */
    struct UnpaginatedScope {
        const std::size_t saved = GlobalState::ITEMS_PER_PAGE;

        UnpaginatedScope() {
            GlobalState::ITEMS_PER_PAGE = 0;
        }

        ~UnpaginatedScope() {
            GlobalState::ITEMS_PER_PAGE = saved;
        }

        UnpaginatedScope(const UnpaginatedScope&) = delete;
        UnpaginatedScope& operator=(const UnpaginatedScope&) = delete;
    };

    /**
     * @brief Temporarily pushes preview indices so printList() draws "[N]^"
     * tags relative to the full list.
     *
     * Pass push=false for an identity frame.
     */
    struct PreviewStackGuard {
        bool pushed;

        PreviewStackGuard(std::vector<size_t>&& globals, bool push)
            : pushed(push)
        {
            if (!pushed)
                return;

            FilteringState s;
            s.originalIndices = std::move(globals);
            filteringStack.push_back(std::move(s));
        }

        ~PreviewStackGuard() {
            if (pushed)
                filteringStack.pop_back();
        }

        PreviewStackGuard(const PreviewStackGuard&) = delete;
        PreviewStackGuard& operator=(const PreviewStackGuard&) = delete;
    };

    /**
     * @brief Computes the matches and paints one unpaginated live-preview frame.
     *
     * Does not touch Readline or print the blank separator row. The cursor is
     * left directly after the list.
     */
    void paintLivePreviewFrame(const std::string& query) {
        LivePreviewResult preview = computeLivePreviewMatches(query);
        g_livePreview.lastQuery = query;

        size_t previewPage = 0;

        {
            const std::vector<std::string>& source = *g_livePreview.sourceList;

            const StringListView view = preview.identity
                ? StringListView(source)
                : StringListView(source, preview.local);

            PreviewStackGuard stackGuard(
                std::move(preview.global),
                !preview.identity);

            UnpaginatedScope unpaginated;

            printList(
                view,
                g_livePreview.listType,
                g_livePreview.listSubType,
                *g_livePreview.pendingIndices,
                *g_livePreview.hasPendingProcess,
                previewPage,
                g_livePreview.state,
                true,
                false);
        }

        g_livePreview.everRepainted = true;
    }

    /**
     * @brief Readline redisplay hook that repaints the real list as the query
     * changes.
     *
     * Falls back to normal redisplay when live preview is disabled, when the
     * query is unchanged, or while typing forward onto a dangling escape.
     */
    void liveFilterRedisplayHook() {
        if (!liveMainListEnabled()) {
            rl_redisplay();
            return;
        }

        const std::string query(rl_line_buffer ? rl_line_buffer : "");
        const bool isFirstFrameOfThisCall = !g_livePreview.primed;
        g_livePreview.primed = true;

        // An unpaired trailing backslash may escape the next '&', ';', '!' or
        // '\'. Hold the list only while the user is typing forward onto it.
        if (hasDanglingBackslash(query)) {
            const bool typedForward =
                query.size() > g_livePreview.lastQuery.size() &&
                query.compare(
                    0,
                    g_livePreview.lastQuery.size(),
                    g_livePreview.lastQuery) == 0;

            if (typedForward) {
                rl_redisplay();
                return;
            }
        }

        if (!isFirstFrameOfThisCall && query == g_livePreview.lastQuery) {
            rl_redisplay();
            return;
        }

        paintLivePreviewFrame(query);

        // Blank separator row between the list and the single-line prompt.
        std::cout << "\n";

        // The whole screen was repainted under readline; redraw prompt + input.
        rl_forced_update_display();
    }

    /**
     * @brief Installs and tears down the live-preview Readline hook.
     *
     * Restores rl_redisplay_function on every exit path.
     */
    struct LivePreviewGuard {
        LivePreviewGuard() {
            rl_redisplay_function = liveFilterRedisplayHook;
        }

        ~LivePreviewGuard() {
            g_livePreview = LiveFilterPreview{};
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
 * @param ctx Filter context supplying display flags and list metadata.
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
 * Uses the live preview when active, otherwise the normal paginated list
 * renderer when its required state is available, then the caller's fallback.
 */
static void repaintOnResize(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource)
{
    if (liveMainListEnabled()) {
        paintLivePreviewFrame(rl_line_buffer ? rl_line_buffer : "");
    } else if (ctx.pendingIndices && ctx.hasPendingProcess && ctx.state &&
               !ctx.listType.empty()) {
        // Reproduce the normal paginated display. Use previewSource, not
        // ctx.files: before a commit ctx.files may be empty or stale, while
        // the displayed list is *ctx.sourceOverride.
        size_t page = ctx.currentPage;

        printList(
            StringListView(previewSource),
            ctx.listType,
            ctx.listSubType,
            *ctx.pendingIndices,
            *ctx.hasPendingProcess,
            page,
            ctx.state,
            true,
            true);
    } else if (ctx.repaintList) {
        // Fallback callback must draw only (no Readline interaction).
        // Restore the current line and cursor after repainting.
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
static void resetLivePreviewIteration() {
    g_livePreview.lastQuery.clear();
    g_livePreview.lastMatches.clear();
    g_livePreview.primed = false;
    g_livePreview.active = true;
}

/**
 * @brief Handles prompt cancellation via EOF (Ctrl+D) or Escape.
 *
 * Normalizes cursor placement one row below the prompt row, flags a screen
 * clear if live preview repainted the list, and clears Readline history.
 *
 * @param raw Line returned by readline (null on EOF or starting with ASCII 27).
 * @param ctx Filter context updated with needsClrScrn if preview repainted.
 */
static void handleFilterCancel(const RlLine& raw, FilterContext& ctx) {
    // Esc echoed its newline. Ctrl+D (EOF) does not, so add it.
    if (!raw)
        std::cout << "\n";

    if (g_livePreview.everRepainted)
        ctx.needsClrScrn = true;

    clear_history();
}

/**
 * @brief Runs the interactive filter prompt and commits a query on Enter.
 *
 * Live preview is enabled only when the caller supplies the required list
 * rendering state and the source list is within LIVE_FILTER_LIMIT.
 *
 * @param promptText Single-line prompt text passed to readlineWithResize().
 * @param ctx Filter context and optional normal-list resize callback.
 * @param onSuccess Callback invoked after a successful filter commit.
 * @param onEmptyInput Retained callback parameter; currently not invoked.
 */
static void runFilterLoop(
    const std::string& promptText,
    FilterContext& ctx,
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
    (void)handleEmpty;

    // Draw the filter prompt on the row of the prompt that was just submitted.
    std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;

    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    // Restores rl_redisplay_function on every exit path.
    LivePreviewGuard livePreviewGuard;

    const std::vector<std::string>& previewSource =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

    configureLivePreview(ctx, previewSource);

    const auto resizeRepaint = [&]() {
        repaintOnResize(ctx, previewSource);
    };

    while (true) {
        resetLivePreviewIteration();

        RlLine raw = readlineWithResize(promptText, resizeRepaint);

        g_livePreview.active = false;

        // Robust handling of the filter prompt.
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

        // Failed commit (empty query or no matches): back onto prompt row.
        std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 *
 * Handles only the exact "/" command. Builds the filter context, suppresses
 * asynchronous list refreshes while the nested Readline prompt owns the
 * terminal, then delegates to runFilterLoop().
 *
 * The suppression flag is restored on every exit path using an RAII guard.
 * Suppressed refresh requests are not dropped; they remain pending and are
 * retried once suppression is lifted.
 *
 * @param inputString Raw input; must be exactly "/" to trigger filtering.
 * @param cfg Configuration struct with state pointers and display options.
 * @return True if inputString was recognized and handled, false otherwise.
 */
static bool runSharedFilterFlow(
    const std::string& inputString,
    const FilterCallConfig& cfg)
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

    // Live main-list rendering is opt-in.
    ctx.listType          = cfg.listType;
    ctx.listSubType       = cfg.listSubType;
    ctx.pendingIndices    = cfg.pendingIndices;
    ctx.hasPendingProcess = cfg.hasPendingProcess;
    ctx.state             = cfg.state;

    // Normal-list repaint, used when live preview is disabled.
    ctx.repaintList = cfg.repaintList;

    auto onEmptyInput = [&]() {
        *cfg.needsClrScrn = *cfg.isFiltered;
    };

    auto onSort = [&]() {
        if (cfg.need2Sort)
            *cfg.need2Sort = true;
    };

    runFilterLoop(prompt, ctx, onSort, onEmptyInput);
    return true;
}

/**
 * @brief Filter entry point for ISO file operations (mount, unmount, etc.).
 *
 * Resolves the correct source list based on the current filter and unmount
 * state, then forwards to runSharedFilterFlow().
 *
 * Source-list priority:
 * - filteredFiles if a filter is already active;
 * - isoDirs if in unmount mode with no active filter;
 * - globalIsoFileList otherwise.
 *
 * @param inputString Raw user input; must be exactly "/" to trigger filtering.
 * @param filteredFiles Currently displayed (possibly filtered) file list.
 * @param operation Name of the ISO operation shown in the prompt.
 * @param operationColor ANSI escape code used to colorise operation.
 * @param isoDirs Mounted ISO paths used in unmount mode.
 * @param isUnmount True when performing an unmount operation.
 * @param ctx Shared filter/display state, refresh state, and repaint callback.
 * @return True if inputString was handled as a filter command.
 */
bool handleFilteringForISO(
    const std::string& inputString,
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
 *
 * Forwards directly to runSharedFilterFlow() with a fixed orange operation
 * color. Unlike handleFilteringForISO(), there is no unmount mode or source
 * list override.
 *
 * @param inputString Raw user input; must be exactly "/" to trigger filtering.
 * @param files List of convertible files to filter in place.
 * @param operation Name of the conversion operation shown in the prompt.
 * @param ctx Shared filter/display state, pending-selection pointers, refresh
 *            state, and repaint callback.
 */
void handleFilteringConvert2ISO(
    const std::string& inputString,
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
