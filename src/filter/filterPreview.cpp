// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "isocmd/core/sharedRefreshState.h"
#include "isocmd/core/lists.h"
#include "isocmd/core/uiState.h"
#include "isocmd/filter/filter.h"
#include "isocmd/ui/display.h"
#include "isocmd/ui/printList.h"
#include "isocmd/ui/readlineResize.h"
#include "isocmd/ui/stringListView.h"
#include "isocmd/ui/themes.h"
#include "isocmd/util/stringManipulation.h"

// ─── Constants ───────────────────────────────────────────────────────────────

namespace {

    constexpr size_t PREVIEW_MIN_ROWS = 1;

    // Accounts for: leading newline before status, preview status line,
    // blank separator line in redisplay hook, prompt row, and header/safety margin.
    constexpr size_t PREVIEW_BASE_RESERVED_ROWS = 6;

    // ─── Data Structures ─────────────────────────────────────────────────────────

    /**
     * @brief Result of one live-preview filter pass: indices only, no strings copied.
     *
     * @c local      Indices into the searchable corpus (source list or derived
     *               labels) that match the current query.
     * @c global     Original source-list indices corresponding 1:1 to @c local,
     *               produced via resolveGlobalIndex().
     * @c identity   True when the query is empty / yields no groups, meaning "all
     *               entries match" and @c local/@c global are left empty (callers
     *               must treat the whole source as the match set).
     */
    struct LivePreviewResult {
        std::vector<size_t> local;
        std::vector<size_t> global;
        bool identity = false;
    };

    /**
     * @brief Bounded [start, end) window slice over a list of size @c total.
     */
    struct WindowSlice {
        size_t start = 0;
        size_t end = 0;
        size_t total = 0;
    };

    /**
     * @brief All state for one live filter-preview session.
     *
     * Stores the source list, searchable-label caches, printList() wiring,
     * previous query and matches for incremental narrowing, windowing state,
     * and repaint state.
     *
     * Session lifecycle: resetLivePreviewIteration() clears the incremental
     * narrowing/windowing fields and sets @c active; deactivateLivePreview()
     * clears @c active; ~LivePreviewReadlineGuard() zeroes the whole struct.
     * The caches (@c hasDerivedCache/@c derivedCache/@c lowerCache) are primed by
     * configureLivePreview() and are reused across keystrokes within one prompt.
     */
    struct LiveFilterPreview {
        const std::vector<std::string>* sourceList = nullptr;
        bool hasLastMatches = false;
        bool useNameOnly = false;
        bool useUnmountKey = false;

        // Wiring needed to call the real printList().
        std::string listType{};
        std::string listSubType{};
        std::vector<std::string>* pendingIndices = nullptr;
        bool* hasPendingProcess = nullptr;
        std::shared_ptr<RefreshState> state = nullptr;

        // Derived-label and lowercased search corpora for sourceList.
        bool hasDerivedCache = false;
        std::vector<std::string> derivedCache;
        std::vector<std::string> lowerCache;

        // Incremental-narrowing state, reset at the start of every readline().
        std::string lastQuery;
        std::vector<size_t> lastMatches;

        // Windowed preview state.
        size_t previewWindowRows = 12;
        size_t previewCursor = 0;
        size_t previewScrollOffset = 0;
        bool forceRepaint = false;

        bool primed = false;
        bool everRepainted = false;
        bool active = false;
    };

    LiveFilterPreview g_livePreview;

    // ─── Terminal Dimensions ─────────────────────────────────────────────────────

    /**
     * @brief Queries the current terminal height in character rows.
     *
     * @return Terminal row count, or a sensible default (24) if unavailable.
     */
    size_t queryTerminalRows()
    {
        struct winsize ws{};

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
            return static_cast<size_t>(ws.ws_row);

        return 24;
    }

    /**
     * @brief Queries the current terminal width in character columns.
     *
     * @return Terminal column count, or a sensible default (80) if unavailable.
     */
    size_t queryTerminalCols()
    {
        struct winsize ws{};

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return static_cast<size_t>(ws.ws_col);

        return 80;
    }

    /**
     * @brief Computes how many terminal rows a line occupies.
     *
     * @param visibleChars Number of visible characters.
     * @param termCols Terminal width in columns.
     *
     * @return Estimated number of occupied rows (always >= 1; 1 when either input
     *         is 0).
     */
    size_t wrappedLineRows(size_t visibleChars, size_t termCols)
    {
        if (termCols == 0 || visibleChars == 0)
            return 1;

        return ((visibleChars - 1) / termCols) + 1;
    }

    /**
     * @brief Computes the dynamic preview window height.
     *
     * Subtracts PREVIEW_BASE_RESERVED_ROWS plus @p extraReservedRows from the
     * terminal height and clamps the result to at least PREVIEW_MIN_ROWS.
     *
     * @param extraReservedRows Additional rows reserved for wrapped content.
     *
     * @return Available number of preview rows (>= PREVIEW_MIN_ROWS).
     */
    size_t computeDynamicPreviewWindowRows(size_t extraReservedRows = 0)
    {
        const size_t termRows = queryTerminalRows();
        const size_t reserved = PREVIEW_BASE_RESERVED_ROWS + extraReservedRows;

        if (termRows <= reserved)
            return PREVIEW_MIN_ROWS;

        return std::max(PREVIEW_MIN_ROWS, termRows - reserved);
    }

    /**
     * @brief Refreshes the preview window height from terminal dimensions.
     *
     * Writes the result of computeDynamicPreviewWindowRows() into
     * @c g_livePreview.previewWindowRows.
     *
     * @param extraReservedRows Additional rows reserved for wrapped content.
     */
    void refreshPreviewWindowHeight(size_t extraReservedRows = 0)
    {
        g_livePreview.previewWindowRows =
            computeDynamicPreviewWindowRows(extraReservedRows);
    }

    // ─── Preview Configuration ───────────────────────────────────────────────────

    /**
     * @brief Checks whether live preview has valid rendering configuration.
     *
     * @return True when all required rendering state and source-list references
     *         are configured, regardless of the source-list size (an empty source
     *         list still counts as configured).
     */
    bool livePreviewConfigured()
    {
        return g_livePreview.sourceList
            && !g_livePreview.listType.empty()
            && g_livePreview.pendingIndices
            && g_livePreview.hasPendingProcess
            && g_livePreview.state;
    }

    /**
     * @brief Checks whether live preview is currently active and usable.
     *
     * @return True when the preview can repaint the main list (the session is
     *         active AND livePreviewConfigured() holds).
     */
    bool liveMainListEnabled()
    {
        return g_livePreview.active && livePreviewConfigured();
    }

    // ─── Search Cache ────────────────────────────────────────────────────────────

    /**
     * @brief Derives the searchable and displayed text for one source entry.
     *
     * The transform is selected by session flags:
     *   - useUnmountKey -> extractUnmountKey(path)
     *   - useNameOnly   -> basename of @p path
     *   - otherwise     -> @p path unchanged
     *
     * @param path Original source path.
     *
     * @return Derived label or the original path.
     */
    std::string livePreviewLabel(const std::string& path)
    {
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
     * @brief Builds derived-label and lowercase search caches.
     *
     * The caches are constructed once per prompt rather than once per keystroke.
     *
     * @c derivedCache is only populated when useNameOnly or useUnmountKey is set;
     * otherwise @c hasDerivedCache is false and matching runs against the source
     * list directly. @c lowerCache always mirrors whichever corpus is searchable
     * (derived when available, otherwise the source) and holds lowercased copies
     * for case-insensitive filtering.
     *
     * @note Requires @c g_livePreview.sourceList to be non-null (set by
     *       configureLivePreview()).
     */
    void primeLivePreviewCaches()
    {
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

    // ─── Matching ────────────────────────────────────────────────────────────────

    /**
     * @brief Computes a bounded preview window while keeping the cursor visible.
     *
     * Clamps @p cursor into range, then slides @p scrollOffset so the cursor falls
     * inside [scrollOffset, scrollOffset + visibleCount). Also clamps the offset
     * to the largest possible start so the window never overhangs @p total.
     *
     * @param total Total number of matching entries.
     * @param maxRows Maximum number of entries to display.
     * @param cursor Current preview cursor.
     * @param scrollOffset Current scroll offset, updated when necessary.
     *
     * @return The visible [start, end) window.
     */
    WindowSlice computePreviewWindow(
        size_t total,
        size_t maxRows,
        size_t cursor,
        size_t& scrollOffset)
    {
        if (total == 0) {
            scrollOffset = 0;
            return {0, 0, 0};
        }

        if (maxRows == 0) {
            scrollOffset = 0;
            return {0, 0, total};
        }

        const size_t visibleCount = std::min(total, maxRows);
        cursor = std::min(cursor, total - 1);

        if (cursor < scrollOffset) {
            scrollOffset = cursor;
        } else if (cursor >= scrollOffset + visibleCount) {
            scrollOffset = cursor - visibleCount + 1;
        }

        const size_t maxStart = total - visibleCount;
        scrollOffset = std::min(scrollOffset, maxStart);

        return {scrollOffset, scrollOffset + visibleCount, total};
    }

    /**
     * @brief Computes matching indices without modifying filteringStack.
     *
     * Strict query extensions can reuse the previous result for incremental
     * narrowing. Backspacing, OR queries, negation, and incomplete escapes
     * require a full search.
     *
     * Empty / no-group queries (and a lone trailing empty group with no negation
     * anywhere) yield an identity result: @c local/@c global are empty and
     * @c identity is true; the incremental cache is cleared.
     *
     * @param query Current filter query.
     *
     * @return Local and global matching indices, or an identity result.
     */
    LivePreviewResult computeLivePreviewMatches(const std::string& query)
    {
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

    // ─── Rendering Guards ────────────────────────────────────────────────────────

    /**
     * @brief Temporarily forces printList() into its unpaginated rendering path.
     *
     * Restores ITEMS_PER_PAGE even when rendering exits through an exception.
     */
    struct UnpaginatedScope {
        const size_t saved = GlobalState::ITEMS_PER_PAGE;

        UnpaginatedScope()
        {
            GlobalState::ITEMS_PER_PAGE = 0;
        }

        ~UnpaginatedScope()
        {
            GlobalState::ITEMS_PER_PAGE = saved;
        }

        UnpaginatedScope(const UnpaginatedScope&) = delete;
        UnpaginatedScope& operator=(const UnpaginatedScope&) = delete;
    };

    /**
     * @brief Temporarily pushes preview indices for printList() index mapping.
     *
     * When @p push is true, installs a temporary FilteringState whose
     * originalIndices are the supplied globals, so printList()/appendRowIndexPrefix
     * map windowed view indices back to their original indices. When @p push is
     * false the guard is inert. The temporary state is popped in the destructor.
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

        ~PreviewStackGuard()
        {
            if (pushed)
                filteringStack.pop_back();
        }

        PreviewStackGuard(const PreviewStackGuard&) = delete;
        PreviewStackGuard& operator=(const PreviewStackGuard&) = delete;
    };

    // ─── Preview Window Calculation ──────────────────────────────────────────────

    /**
     * @brief Calculates a preview window that accounts for wrapped item labels.
     *
     * Starts with the available row budget and reduces the visible item count
     * when the estimated physical height exceeds that budget.
     *
     * @note When it shrinks the window, this function writes the reduced row
     *       budget back into @c g_livePreview.previewWindowRows, so the reduced
     *       height persists for subsequent frames and page-scroll sizing.
     *
     * @param preview Filtered preview result.
     * @param totalMatches Total number of matching entries.
     * @param termCols Terminal width.
     * @param displayLabels Labels used for wrapping estimates.
     *
     * @return Adjusted visible window.
     */
    WindowSlice calculateWrappedPreviewWindow(
        const LivePreviewResult& preview,
        size_t totalMatches,
        size_t termCols,
        const std::vector<std::string>& displayLabels)
    {
        size_t effectiveMaxRows = g_livePreview.previewWindowRows;

        WindowSlice win = computePreviewWindow(
            totalMatches,
            effectiveMaxRows,
            g_livePreview.previewCursor,
            g_livePreview.previewScrollOffset);

        if (totalMatches > 0 && win.end > win.start) {
            size_t physicalItemRows = 0;

            for (size_t i = win.start; i < win.end; ++i) {
                const size_t itemIdx =
                    preview.identity ? i : preview.local[i];

                const size_t approxCols =
                    (itemIdx < displayLabels.size()
                        ? displayLabels[itemIdx].size()
                        : 0) + 10;

                physicalItemRows += wrappedLineRows(approxCols, termCols);
            }

            if (physicalItemRows > effectiveMaxRows) {
                const size_t avgRowsPerItem =
                    std::max<size_t>(
                        1,
                        physicalItemRows / (win.end - win.start));

                effectiveMaxRows = std::max<size_t>(
                    PREVIEW_MIN_ROWS,
                    effectiveMaxRows / avgRowsPerItem);

                g_livePreview.previewWindowRows = effectiveMaxRows;

                win = computePreviewWindow(
                    totalMatches,
                    effectiveMaxRows,
                    g_livePreview.previewCursor,
                    g_livePreview.previewScrollOffset);
            }
        }

        return win;
    }

    /**
     * @brief Calculates the visible live-preview window.
     *
     * Determines available height from terminal dimensions, status-line wrapping,
     * and estimated item-label wrapping. Also maintains a valid cursor and offset.
     *
     * Uses a fixed 64-item sample (from the top of the match set) to estimate the
     * worst-case wrapped height per item, then reserves extra rows for the status
     * line plus a 2x-per-extra-wrapped-row heuristic before delegating to
     * calculateWrappedPreviewWindow().
     *
     * @param preview Filtered preview result.
     * @param totalMatches Total number of matching entries.
     * @param termCols Terminal width.
     *
     * @return Calculated visible window.
     */
    WindowSlice calculatePreviewWindow(
        const LivePreviewResult& preview,
        size_t totalMatches,
        size_t termCols)
    {
        if (totalMatches == 0) {
            g_livePreview.previewCursor = 0;
            g_livePreview.previewScrollOffset = 0;
        } else if (g_livePreview.previewCursor >= totalMatches) {
            g_livePreview.previewCursor = totalMatches - 1;
        }

        const size_t sampleEnd =
            std::min(totalMatches, static_cast<size_t>(9999));

        const size_t initialRows = computeDynamicPreviewWindowRows(0);
        const size_t sampleShown = std::min(totalMatches, initialRows);

        const std::string statusSample = (totalMatches > sampleShown)
            ? std::format(
                  "[preview {}-{}/{} · shown {} · PgUp/PgDn to scroll]",
                  totalMatches > 0 ? 1 : 0,
                  sampleEnd,
                  totalMatches,
                  sampleShown)
            : std::format(
                  "[preview {}-{}/{} · shown {}]",
                  totalMatches > 0 ? 1 : 0,
                  sampleEnd,
                  totalMatches,
                  sampleShown);

        const size_t statusWrappedRows =
            wrappedLineRows(statusSample.size(), termCols);

        const size_t statusWrapExtra =
            statusWrappedRows > 1 ? statusWrappedRows - 1 : 0;

        const std::vector<std::string>& source = *g_livePreview.sourceList;

        const std::vector<std::string>& displayLabels =
            g_livePreview.hasDerivedCache
                ? g_livePreview.derivedCache
                : source;

        size_t maxWrappedPerItem = 1;

        for (size_t i = 0; i < totalMatches && i < 64; ++i) {
            const size_t itemIdx =
                preview.identity ? i : preview.local[i];

            if (itemIdx < displayLabels.size()) {
                // Prefix "[idx]^ " (~10 characters) plus label length.
                const size_t approxCols =
                    displayLabels[itemIdx].size() + 10;

                maxWrappedPerItem = std::max(
                    maxWrappedPerItem,
                    wrappedLineRows(approxCols, termCols));
            }
        }

        const size_t wrapAdjustment =
            maxWrappedPerItem > 1
                ? (maxWrappedPerItem - 1) * 2
                : 0;

        refreshPreviewWindowHeight(statusWrapExtra + wrapAdjustment);

        return calculateWrappedPreviewWindow(
            preview,
            totalMatches,
            termCols,
            displayLabels);
    }

    // ─── Preview Rendering ───────────────────────────────────────────────────────

    /**
     * @brief Renders the visible portion of the live-filter preview.
     *
     * Builds a windowed StringListView when necessary, renders without pagination,
     * and prints the preview status line.
     *
     * Two windowing cases exist:
     *   - non-identity match set: a view over the matching local indices;
     *   - identity match set: a view over the identity range only when the source
     *     list is longer than the visible window (otherwise the full list is used).
     *
     * In both windowed cases a temporary FilteringState maps the windowed local
     * indices back to original globals; previewPage is always 0 because the
     * UnpaginatedScope disables pagination, and displayIndexOffset is win.start so
     * row numbering continues across pages.
     *
     * @param preview Filtered preview result.
     * @param win Calculated visible window.
     * @param totalMatches Total number of matching entries.
     */
    void renderPreviewWindow(
        const LivePreviewResult& preview,
        const WindowSlice& win,
        size_t totalMatches)
    {
        const std::vector<std::string>& source = *g_livePreview.sourceList;

        std::vector<size_t> windowedLocal;
        std::vector<size_t> windowedGlobal;
        bool useWindowedView = false;

        if (!preview.identity) {
            windowedLocal.reserve(win.end - win.start);
            windowedGlobal.reserve(win.end - win.start);

            for (size_t i = win.start; i < win.end; ++i) {
                windowedLocal.push_back(preview.local[i]);

                if (i < preview.global.size())
                    windowedGlobal.push_back(preview.global[i]);
            }

            useWindowedView = true;
        } else if (
            g_livePreview.previewWindowRows > 0 &&
            (win.end - win.start) < source.size())
        {
            windowedLocal.reserve(win.end - win.start);
            windowedGlobal.reserve(win.end - win.start);

            for (size_t i = win.start; i < win.end; ++i) {
                windowedLocal.push_back(i);
                windowedGlobal.push_back(resolveGlobalIndex(i));
            }

            useWindowedView = true;
        }

        size_t previewPage = 0;

        {
            const StringListView view = useWindowedView
                ? StringListView(source, windowedLocal)
                : StringListView(source);

            PreviewStackGuard stackGuard(
                std::move(windowedGlobal),
                useWindowedView);

            UnpaginatedScope unpaginated;

            printList(
                view,
                g_livePreview.listType,
                g_livePreview.listSubType,
                PrintListContext{
                    .pendingIndices = *g_livePreview.pendingIndices,
                    .hasPendingProcess = *g_livePreview.hasPendingProcess,
                    .currentPage = previewPage,
                    .state = g_livePreview.state,
                    .clearFirst = true,
                    .showPendingIndices = false,
                    .showSourceIndices = false,
                    .displayIndexOffset = win.start
                });
        }

        const PrintListTheme ft = getListColors();

        if (g_livePreview.previewWindowRows > 0 && totalMatches > 0) {
            const size_t shownStart = win.start + 1;
            const size_t shownEnd = win.end;

            const bool canScrollUp = win.start > 0;
            const bool canScrollDown = win.end < totalMatches;

            std::cout << "\r\033[K"
                      << ft.head
                      << std::format("\n[Matches {}({}", ft.head, ft.accent)
                      << shownStart
                      << ft.accent << "-"
                      << ft.accent << shownEnd
                      << ft.head << ")/" << totalMatches
                      << (canScrollUp ? " · PgUp ↑" : "")
                      << (canScrollDown ? (canScrollUp ? " | PgDn ↓" : " · PgDn ↓") : "")
                      << "]\n";
        }
    }

    /**
     * @brief Computes matches and paints one windowed live-preview frame.
     *
     * Does not interact with Readline or print the blank separator row.
     *
     * When the query changed since the last frame, resets the preview cursor and
     * scroll offset to the top before computing the window. Updates
     * @c g_livePreview.lastQuery and sets @c everRepainted.
     *
     * @param query Current filter query.
     */
    void paintLivePreviewFrame(const std::string& query)
    {
        const size_t termCols = queryTerminalCols();

        const bool queryChanged = query != g_livePreview.lastQuery;

        LivePreviewResult preview = computeLivePreviewMatches(query);
        g_livePreview.lastQuery = query;

        if (queryChanged) {
            g_livePreview.previewCursor = 0;
            g_livePreview.previewScrollOffset = 0;
        }

        const std::vector<std::string>& source = *g_livePreview.sourceList;

        const size_t totalMatches =
            preview.identity ? source.size() : preview.local.size();

        const WindowSlice win = calculatePreviewWindow(
            preview,
            totalMatches,
            termCols);

        renderPreviewWindow(preview, win, totalMatches);

        g_livePreview.everRepainted = true;
    }

    // ─── Readline Integration ────────────────────────────────────────────────────

    /**
     * @brief Readline redisplay hook that repaints the live preview when needed.
     *
     * Repaints the list, prints its separator row, and forces Readline to redraw
     * the prompt and current input after the screen has been repainted.
     *
     * The hook skips repainting (falling back to rl_redisplay()) when:
     *   - the preview is not active/configured;
     *   - a forced repaint is pending, a dangling backslash was followed by a
     *     forward keystroke, and this isn't the first frame of the call;
     *   - the query is unchanged and this isn't the first frame of the call.
     * A forced repaint (@c forceRepaint) always paints and clears the flag.
     */
    void liveFilterRedisplayHook()
    {
        if (!liveMainListEnabled()) {
            rl_redisplay();
            return;
        }

        const std::string query(rl_line_buffer ? rl_line_buffer : "");

        const bool isFirstFrameOfThisCall = !g_livePreview.primed;
        g_livePreview.primed = true;

        const bool forced = g_livePreview.forceRepaint;
        g_livePreview.forceRepaint = false;

        if (!forced && hasDanglingBackslash(query)) {
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

        if (!forced &&
            !isFirstFrameOfThisCall &&
            query == g_livePreview.lastQuery) {
            rl_redisplay();
            return;
        }

        paintLivePreviewFrame(query);

        // Blank separator row between the preview and the single-line prompt.
        std::cout << "\n";

        // The whole screen was repainted under Readline.
        rl_forced_update_display();
    }

    /**
     * @brief Scrolls the live-preview cursor and viewport by one visible page.
     *
     * The cursor and scroll offset move together so computePreviewWindow() does
     * not reduce a page-sized movement to a single-row viewport adjustment.
     * The page size uses the effective preview height established by the latest
     * rendering pass, which already accounts for wrapped labels.
     *
     * When the full match set is already visible (count <= pageSize), the
     * function returns without moving; otherwise both the cursor and offset are
     * clamped so they cannot exceed the last page. Sets @c forceRepaint and
     * invokes the redisplay hook so the next frame paints unconditionally.
     *
     * @param direction Negative for PageUp, positive for PageDown.
     */
    void movePreviewWindowCursor(ptrdiff_t direction)
    {
        if (!liveMainListEnabled() || direction == 0)
            return;

        const size_t count = g_livePreview.hasLastMatches
            ? g_livePreview.lastMatches.size()
            : (g_livePreview.sourceList
                ? g_livePreview.sourceList->size()
                : 0);

        if (count == 0) {
            g_livePreview.previewCursor = 0;
            g_livePreview.previewScrollOffset = 0;
            return;
        }

        const size_t pageSize = std::min(
            count,
            std::max<size_t>(1, g_livePreview.previewWindowRows));

        // There is no next or previous page when every match is already visible.
        if (count <= pageSize)
            return;

        const size_t maxScrollOffset = count - pageSize;
        const size_t currentCursor =
            std::min(g_livePreview.previewCursor, count - 1);
        const size_t currentOffset =
            std::min(g_livePreview.previewScrollOffset, maxScrollOffset);

        if (direction > 0) {
            g_livePreview.previewCursor =
                currentCursor + std::min(pageSize, count - 1 - currentCursor);
            g_livePreview.previewScrollOffset =
                currentOffset + std::min(pageSize, maxScrollOffset - currentOffset);
        } else {
            g_livePreview.previewCursor =
                currentCursor - std::min(pageSize, currentCursor);
            g_livePreview.previewScrollOffset =
                currentOffset - std::min(pageSize, currentOffset);
        }

        g_livePreview.forceRepaint = true;
        liveFilterRedisplayHook();
    }

    /**
     * @brief Readline PageUp handler for preview-window scrolling.
     *
     * Readline passes (count, key) but the preview ignores both; the action is
     * always a single page up. Returns 0 to signal success to Readline.
     */
    int rlPreviewPageUp(int /*count*/, int /*key*/)
    {
        movePreviewWindowCursor(-1);
        return 0;
    }

    /**
     * @brief Readline PageDown handler for preview-window scrolling.
     *
     * Readline passes (count, key) but the preview ignores both; the action is
     * always a single page down. Returns 0 to signal success to Readline.
     */
    int rlPreviewPageDown(int /*count*/, int /*key*/)
    {
        movePreviewWindowCursor(1);
        return 0;
    }

} // namespace

/**
 * @brief Reports whether the live preview has repainted at least once.
 *
 * @return True if a preview frame has been rendered during the current
 *         preview session; otherwise false. The flag is set by
 *         paintLivePreviewFrame() and is not cleared by
 *         resetLivePreviewIteration() — only by the LivePreviewReadlineGuard
 *         destructor (which zeroes the whole preview state).
 */
bool livePreviewWasRepainted()
{
    return g_livePreview.everRepainted;
}

// ─── Public Preview API ──────────────────────────────────────────────────────

/**
 * @brief Configures live-preview state and primes its search caches.
 *
 * Copies the rendering configuration from the filter context and prepares
 * searchable labels when live preview is enabled.
 *
 * The name-only and unmount-key modes are mutually exclusive and derived from
 * @p ctx: useNameOnly is set when names-only display is toggled and the filter
 * is not an unmount filter; useUnmountKey is set for unmount filters that are
 * not showing the full list. One of these (or neither) selects the search
 * corpus primed by primeLivePreviewCaches().
 *
 * @param ctx Filter context supplying list-rendering configuration.
 * @param previewSource Source list displayed by the preview.
 */
void configureLivePreview(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource)
{
    g_livePreview.sourceList = &previewSource;
    g_livePreview.useNameOnly =
        displayConfig::toggleNamesOnly && !ctx.isUnmount;
    g_livePreview.useUnmountKey =
        ctx.isUnmount && !ctx.toggleFullListUmount;

    g_livePreview.listType = ctx.listType;
    g_livePreview.listSubType = ctx.listSubType;
    g_livePreview.pendingIndices = ctx.pendingIndices;
    g_livePreview.hasPendingProcess = ctx.hasPendingProcess;
    g_livePreview.state = ctx.state;

    refreshPreviewWindowHeight();
    primeLivePreviewCaches();
}

/**
 * @brief Repaints the active live preview after a terminal resize.
 *
 * Refreshes the window height from the new terminal dimensions and paints a
 * frame using the current Readline line buffer. Does not print the separator
 * row or force Readline to redraw the prompt (the resize path handles that).
 *
 * @return True if the live preview handled the repaint; otherwise false (the
 *         caller should fall back to its own resize handling).
 */
bool repaintLivePreviewOnResizeIfConfigured()
{
    if (!liveMainListEnabled())
        return false;

    refreshPreviewWindowHeight();
    paintLivePreviewFrame(rl_line_buffer ? rl_line_buffer : "");

    return true;
}

/**
 * @brief Resets incremental matching and windowing state before readline().
 *
 * Clears @c lastQuery, @c lastMatches, cursor/offset, and @c forceRepaint;
 * refreshes the window height from the terminal; clears @c primed and sets
 * @c active. Does not clear the primed search caches or @c everRepainted.
 */
void resetLivePreviewIteration()
{
    g_livePreview.lastQuery.clear();
    g_livePreview.lastMatches.clear();
    g_livePreview.previewCursor = 0;
    g_livePreview.previewScrollOffset = 0;
    g_livePreview.forceRepaint = false;

    refreshPreviewWindowHeight();

    g_livePreview.primed = false;
    g_livePreview.active = true;
}

/**
 * @brief Marks the live-preview session inactive after readline() returns.
 */
void deactivateLivePreview()
{
    g_livePreview.active = false;
}

/**
 * @brief Installs and restores the Readline live-preview hooks and keybindings.
 *
 * Construction installs the preview redisplay callback and scrolling keys.
 * Destruction restores history navigation bindings, resets preview state,
 * and restores the default Readline redisplay callback.
 *
 * PageUp/PageDown are bound to previous/next history by default; the
 * constructor rebinds them to preview scrolling, and the destructor restores
 * those named functions and sets @c rl_redisplay_function back to
 * @c rl_redisplay.
 */
LivePreviewReadlineGuard::LivePreviewReadlineGuard()
{
    rl_redisplay_function = liveFilterRedisplayHook;

    rl_bind_keyseq("\x1b[5~", rlPreviewPageUp);   // PageUp
    rl_bind_keyseq("\x1b[6~", rlPreviewPageDown); // PageDown
}

LivePreviewReadlineGuard::~LivePreviewReadlineGuard()
{
    rl_bind_keyseq(
        "\x1b[5~",
        rl_named_function("previous-history"));

    rl_bind_keyseq(
        "\x1b[6~",
        rl_named_function("next-history"));

    g_livePreview = LiveFilterPreview{};

    rl_redisplay_function = rl_redisplay;
}
