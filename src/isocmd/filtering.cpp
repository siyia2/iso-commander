// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cctype>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C / System Headers
#include <string.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../concurrency.h"
#include "../display.h"
#include "../filtering.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../sharedRefreshState.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../state.h"
#include "../threadpool.h"

// Defined in printList.cpp. Forward-declared here (same as displayCode.cpp
// does) so the live filter preview below can repaint the real list, not a
// stand-in, on every keystroke.
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state);

// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {
    constexpr const char* CLEAR_LINE_ABOVE = "\033[1A\033[K";
    constexpr const char* CLEAR_TWO_LINES  = "\033[2A\033[K";
}

// ─── Boyer-Moore implementation ──────────────────────────────────────────────

/**
 * @brief Precomputes Boyer-Moore bad character and good suffix tables for a pattern
 *
 * @param pattern The search pattern to precompute tables for
 * @param badCharTable Output table mapping characters to their last occurrence index
 * @param goodSuffixTable Output table with safe skip distances for suffix mismatches
 */
void precomputeBoyerMooreTables(const std::string& pattern, std::vector<int>& badCharTable, std::vector<int>& goodSuffixTable)
{
    const size_t m             = pattern.size();
    const int    ALPHABET_SIZE = 256;

    badCharTable.assign(ALPHABET_SIZE, -1);
    for (int i = 0; i < static_cast<int>(m); ++i)
        badCharTable[static_cast<unsigned char>(pattern[i])] = i;

    goodSuffixTable.resize(m, static_cast<int>(m));
    std::vector<int> suffix(m, 0);

    suffix[m - 1] = static_cast<int>(m);
    int g = static_cast<int>(m) - 1;
    int f = static_cast<int>(m) - 1;

    for (int i = static_cast<int>(m) - 2; i >= 0; --i) {
        if (i > g && suffix[i + m - 1 - f] < i - g) {
            suffix[i] = suffix[i + m - 1 - f];
        } else {
            g = std::min(g, i);
            f = i;
            while (g >= 0 && pattern[g] == pattern[g + m - 1 - f])
                --g;
            suffix[i] = f - g;
        }
    }

    for (int i = 0; i < static_cast<int>(m) - 1; ++i)
        goodSuffixTable[i] = static_cast<int>(m) - 1 - suffix[0];

    for (int i = 0; i <= static_cast<int>(m) - 2; ++i) {
        const int j = static_cast<int>(m) - 1 - suffix[i];
        if (goodSuffixTable[j] > static_cast<int>(m) - 1 - i)
            goodSuffixTable[j] = static_cast<int>(m) - 1 - i;
    }
}

/**
 * @brief Performs Boyer-Moore search to check if pattern exists in text
 *
 * @param text The text to search within
 * @param pattern The pattern to search for
 * @param badCharTable Precomputed bad character shift table
 * @param goodSuffixTable Precomputed good suffix shift table
 * @return true if pattern is found, false otherwise
 */
bool boyerMooreSearchExists(const std::string& text, const std::string& pattern, const std::vector<int>& badCharTable, const std::vector<int>& goodSuffixTable)
{
    const size_t n = text.size();
    const size_t m = pattern.size();
    if (m == 0 || m > n) return false;

    int s = 0;
    while (s <= static_cast<int>(n - m)) {
        int j = static_cast<int>(m) - 1;
        while (j >= 0 && text[s + j] == pattern[j])
            --j;

        if (j < 0)
            return true;

        const int bcShift = j - badCharTable[static_cast<unsigned char>(text[s + j])];
        const int gsShift = goodSuffixTable[j];
        s += std::max(1, std::max(bcShift, gsShift));
    }
    return false;
}

// ─── Query tokenization ──────────────────────────────────────────────────────

/**
 * @brief Builds query tokens from a semicolon-separated query string
 *
 * @param query The query string to tokenize
 * @return Vector of QueryToken objects ready for Boyer-Moore searching
 */
static std::vector<QueryToken> buildQueryTokens(const std::string& query) {
    std::vector<QueryToken> tokens;
    std::stringstream ss(query);
    std::string token;

    while (std::getline(ss, token, ';')) {
        if (token.empty()) continue;

        QueryToken qt;
        qt.original        = token;
        qt.isCaseSensitive = std::any_of(token.begin(), token.end(),
                                 [](unsigned char c) { return std::isupper(c); });

        precomputeBoyerMooreTables(qt.original, qt.originalBadChar, qt.originalGoodSuffix);

        if (!qt.isCaseSensitive) {
            qt.lower = token;
            toLowerInPlace(qt.lower);
            precomputeBoyerMooreTables(qt.lower, qt.lowerBadChar, qt.lowerGoodSuffix);
        }

        tokens.push_back(std::move(qt));
    }
    return tokens;
}

// ─── Core filter engine ──────────────────────────────────────────────────────

/**
 * @brief Filters file indices based on a search query using Boyer-Moore algorithm
 *
 * @param files Vector of file paths to filter
 * @param query Search query with semicolon-separated terms
 * @param precomputedLower Optional parallel array, same size as @p files, holding
 *        each entry of @p files already lowercased. When provided (and its size
 *        matches @p files), it is used in place of lowercasing each entry inline,
 *        letting a caller that runs this same @p files list through many queries
 *        in a row (e.g. the live filter preview, once per keystroke) lowercase
 *        each entry exactly once instead of on every call. Pass nullptr (the
 *        default) to preserve the original per-call lowercasing behavior.
 * @return Vector of indices matching the search criteria
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query,
                                        const std::vector<std::string>* precomputedLower)
{
    if (files.empty() || query.empty()) return {};

    const std::vector<QueryToken> queryTokens = buildQueryTokens(query);
    if (queryTokens.empty()) {
        std::vector<size_t> allIndices(files.size());
        std::iota(allIndices.begin(), allIndices.end(), 0);
        return allIndices;
    }

    const bool needLower = std::any_of(queryTokens.begin(), queryTokens.end(),
                               [](const QueryToken& qt) { return !qt.isCaseSensitive; });

    // Only trust a caller-supplied lowercase cache if it actually lines up
    // with `files` — otherwise silently fall back to lowercasing inline, so
    // a stale/mismatched cache can never produce wrong indices.
    const bool useCachedLower = precomputedLower && precomputedLower->size() == files.size();

    ThreadPool&  pool       = getStaticThreadPool();
    const size_t numThreads = std::min({
        pool.threadCount(),
        files.size(),
        static_cast<size_t>(GlobalConcurrency::FILTER_THREAD_CAP)
    });

    const size_t chunkSize  = (files.size() + numThreads - 1) / numThreads;

    std::vector<std::future<std::vector<size_t>>> futures;
        futures.reserve(numThreads);

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(files.size(), start + chunkSize);
        if (start >= end) break;

        futures.emplace_back(pool.enqueue(
            [&files, start, end, needLower, &queryTokens, precomputedLower, useCachedLower]() -> std::vector<size_t> {
                std::vector<size_t> localMatches;
                localMatches.reserve((end - start) / 4);

                std::string fileLowerScratch;
                if (needLower && !useCachedLower)
                    fileLowerScratch.reserve(256);

                for (size_t j = start; j < end; ++j) {
                    const std::string& file = files[j];

                    const std::string* fileLower = nullptr;
                    if (needLower) {
                        if (useCachedLower) {
                            fileLower = &(*precomputedLower)[j];
                        } else {
                            fileLowerScratch = file;
                            toLowerInPlace(fileLowerScratch);
                            fileLower = &fileLowerScratch;
                        }
                    }

                    for (const auto& qt : queryTokens) {
                        bool match;
                        if (qt.isCaseSensitive) {
                            match = boyerMooreSearchExists(file,       qt.original,
                                                            qt.originalBadChar, qt.originalGoodSuffix);
                        } else {
                            match = boyerMooreSearchExists(*fileLower, qt.lower,
                                                            qt.lowerBadChar,    qt.lowerGoodSuffix);
                        }
                        if (match) {
                            localMatches.push_back(j);
                            break;
                        }
                    }
                }
                return localMatches;
            }
        ));
    }

    std::vector<size_t> filteredIndices;
    filteredIndices.reserve(files.size());

    std::exception_ptr firstException;

    for (auto& fut : futures) {
        try {
            auto chunk = fut.get();
            filteredIndices.insert(filteredIndices.end(),
                                   std::make_move_iterator(chunk.begin()),
                                   std::make_move_iterator(chunk.end()));
        } catch (...) {
            if (!firstException)
                firstException = std::current_exception();
        }
    }

    if (firstException)
        std::rethrow_exception(firstException);

    return filteredIndices;
}

// ─── Shared filtering core ───────────────────────────────────────────────────

/**
 * @brief Derives the short "unmount key" label used to match/display an
 * already-mounted ISO (basename with any trailing "~hash" suffix stripped).
 * Shared by @c applyFilterCore and the live filter preview so both stay
 * in sync on exactly what text is being matched against.
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
 * @brief Executes core filtering logic with support for nested filter stacks.
 * * * Transforms source paths into searchable strings based on context (e.g.,
 * filename only or unmount-specific keys).
 * * Chains new results through the existing `filteringStack` to ensure that
 * local indices are correctly mapped back to the global database indices.
 * * Manages UI state by resetting pagination and marking the screen for refresh.
 *
 * @param searchString The substring pattern to filter by (saved for state recovery).
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return true if matches were found and the filter stack was updated;
 * false if the query is empty or no matches exist.
 */
static bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {
    if (searchString.empty()) return false;

    const std::vector<std::string>& sourceList =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

    std::vector<std::string> tempFiltered;
    std::vector<size_t>      tempIndices;

    const bool useNameOnly   = displayConfig::toggleNamesOnly && !ctx.isUnmount;
    const bool useUnmountKey = ctx.isUnmount && !ctx.toggleFullListUmount;

    if (useNameOnly || useUnmountKey) {
        std::vector<std::string> derived;
        derived.reserve(sourceList.size());
        for (const auto& path : sourceList) {
            if (useUnmountKey) {
                derived.push_back(extractUnmountKey(path));
            } else {
                size_t lastSlash = path.find_last_of('/');
                derived.push_back((lastSlash != std::string::npos)
                                  ? path.substr(lastSlash + 1)
                                  : path);
            }
        }

        auto matchedIndices = filterFilesIndices(derived, searchString);
        if (matchedIndices.empty()) return false;

        tempFiltered.reserve(matchedIndices.size());
        tempIndices.reserve(matchedIndices.size());
        for (size_t idx : matchedIndices) {
            tempFiltered.push_back(sourceList[idx]);
            tempIndices.push_back(idx);
        }
    } else {
        tempIndices = filterFilesIndices(sourceList, searchString);
        if (tempIndices.empty()) return false;

        tempFiltered.reserve(tempIndices.size());
        for (size_t idx : tempIndices) {
            tempFiltered.push_back(sourceList[idx]);
        }
    }

    if (tempFiltered.empty())                     return false;
    if (tempFiltered.size() == sourceList.size()) {
        std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
        return true;
    }

    ctx.currentPage  = 0;
    ctx.needsClrScrn = true;
    ctx.files        = std::move(tempFiltered);

    FilteringState newState;
    newState.originalIndices.reserve(tempIndices.size());
    newState.query      = searchString;  // save query
    newState.isFiltered = true;

	for (size_t idx : tempIndices) {
		size_t globalIdx = idx;
		// Walk all existing stack levels to translate idx (relative to the
		// current display list) all the way back to a globalIsoFileList index.
		// Each level's originalIndices maps its local positions to the level
		// below, until we reach level 0 whose indices ARE already global.
		if (!filteringStack.empty()) {
			// tempIndices are local to sourceList. sourceList was built from
			// filteringStack levels in order, so we need to chain through them.
			// Start from the innermost (back) and work outward.
			for (int lvl = static_cast<int>(filteringStack.size()) - 1; lvl >= 0; --lvl) {
				const auto& lvlIndices = filteringStack[lvl].originalIndices;
				if (globalIdx < lvlIndices.size())
					globalIdx = lvlIndices[globalIdx];
			}
		}
		newState.originalIndices.push_back(globalIdx);
	}

    filteringStack.push_back(std::move(newState));

    ctx.isFiltered = true;
    return true;
}

// ─── History helpers ─────────────────────────────────────────────────────────

/**
 * @brief Saves a search query to readline history
 *
 * @param query The query string to save
 * @param filterHistory Reference to filter history flag
 * @param alreadyLoaded If true, skips loadHistory (caller loaded before the prompt)
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
// Historically, matches were only computed (and shown) after the user
// pressed Enter on the FilterTerms prompt. This section adds a live,
// "type-to-narrow" repaint of the REAL on-screen list: GNU Readline calls
// rl_redisplay_function every time it redraws the input line — i.e. after
// essentially every keystroke, including backspaces — so we hook that call
// to recompute matches against the in-progress (uncommitted) query and
// repaint the same printList() the rest of the app uses, right where the
// list is already displayed.
//
// Mechanics: each repaint does clearScrollBuffer() + printList(), which
// wipes and redraws the whole screen, so readline's own idea of "what's
// currently on screen" (used for its normal incremental redraw) is now
// stale. We correct that with rl_forced_update_display(), which — unlike
// the rl_redisplay_function pointer we've hooked — is a real, directly
// callable Readline entry point that unconditionally repaints the prompt
// and in-progress line fresh, ignoring its stale cache. When nothing has
// changed since the last frame (e.g. pure cursor movement), we skip the
// repaint and just call the ordinary rl_redisplay() instead.
//
// Important: all of this is purely visual until Enter. filteringStack and
// ctx.files are never touched here — Enter still runs the exact same commit
// path as before (applyFilterCore + saveQueryToHistory), so history and
// nested-filter-stack semantics are unchanged.
//
// Search-corpus caching: the text actually searched per source entry (its
// basename, its unmount key, or the raw path) and that text's lowercased
// form depend only on `sourceList` + the useNameOnly/useUnmountKey toggles
// — never on the in-progress query. Both are therefore computed exactly
// once per runFilterLoop() invocation (primeLivePreviewCaches, called
// before the readline() loop starts) rather than being rebuilt from
// scratch on every keystroke, which used to mean a full pass of string
// allocation + case-folding over the whole source list per frame.
//
// Trade-offs worth knowing about:
//  - Because filteringStack isn't updated until commit, the live repaint
//    always renders with isFiltered=false while a query is in progress, so
//    the "[123]^" original-index tags (drawn from filteringStack.back())
//    don't show mid-type; they reappear normally once Enter commits.
//  - A full clearScrollBuffer()+printList() every keystroke is heavier than
//    a delta redraw and can flicker on slow/high-latency terminals. Above
//    kLivePreviewSourceCap source items we skip live repainting entirely
//    and fall back to the old Enter-only behavior, to keep typing responsive
//    on very large lists.
//  - The derived/lowercase caches are primed once per runFilterLoop() call
//    and intentionally never refreshed mid-loop: sourceList only changes on
//    a successful filter commit, which immediately ends the loop, so there
//    is no point in the loop's lifetime where the cache could go stale
//    while still being read from.

namespace {

/**
 * @brief Result of a single live-preview filter pass: the matching entries
 * from the preview's source list, alongside the corresponding indices
 * already translated back to @c globalIsoFileList (see
 * @c computeLivePreviewItems).
 */
struct LivePreviewResult {
    std::vector<std::string> items;
    std::vector<size_t> indices;
};

/**
 * @brief Holds all state for one live filter-preview session (i.e. one
 * @c runFilterLoop() invocation): the source list and how to derive its
 * searchable labels, the printList() wiring needed to repaint the real
 * on-screen list, the per-invocation derived/lowercase search caches
 * (see @c primeLivePreviewCaches), and the bookkeeping used to decide
 * whether a given readline() keystroke needs a fresh repaint.
 */
struct LiveFilterPreview {
    const std::vector<std::string>* sourceList     = nullptr;
    bool                             useNameOnly   = false;
    bool                             useUnmountKey = false;

    // Set up per readline() call (see runFilterLoop); mirrors the *actual*
    // current ctx state so the empty-query frame can render an exact match
    // of what's already on screen instead of resetting page/annotations.
    bool*   actualIsFiltered = nullptr;
    size_t* actualCurrentPage = nullptr;

    // Wiring needed to call the real printList().
    std::string                   listType{};
    std::string                   listSubType{};
    std::vector<std::string>*     pendingIndices    = nullptr;
    bool*                         hasPendingProcess = nullptr;
    std::shared_ptr<RefreshState> state = nullptr;

    // Derived-label ("name only" / unmount-key) and lowercased search
    // corpora for `*sourceList`. Both depend only on sourceList and the
    // useNameOnly/useUnmountKey toggles above, never on the in-progress
    // query, so they are computed exactly once per runFilterLoop()
    // invocation (see primeLivePreviewCaches) instead of being rebuilt on
    // every keystroke.
    bool                      hasDerivedCache = false; // true => search derivedCache, not *sourceList
    std::vector<std::string>  derivedCache;
    std::vector<std::string>  lowerCache;             // lowercased derivedCache (or *sourceList if !hasDerivedCache)

    std::string lastQuery;
    bool        primed        = false;  // a frame has been drawn for the *current* readline() call
    bool        everRepainted = false;  // a real repaint has happened at least once this runFilterLoop call
    bool        active        = false;
};

LiveFilterPreview g_livePreview;

// Above this source size, live-filtering on every keystroke (each of which
// re-runs the threaded Boyer-Moore search plus a full screen repaint) would
// add visible input lag, so we silently fall back to the old Enter-only
// behavior for huge lists.
constexpr size_t kLivePreviewSourceCap = 20000;

/**
 * @brief True once sourceList + the printList() wiring have been set up and
 * are within the size cap, regardless of whether a readline() call is
 * currently in flight. Used to decide whether it is worth priming the
 * derived/lowercase caches before the runFilterLoop while-loop starts.
 *
 * @return true if the live preview has everything it needs to run safely.
 */
bool livePreviewConfigured() {
    return g_livePreview.sourceList
        && !g_livePreview.listType.empty()
        && g_livePreview.pendingIndices
        && g_livePreview.hasPendingProcess
        && g_livePreview.state
        && g_livePreview.sourceList->size() <= kLivePreviewSourceCap;
}

/**
 * @brief True while a readline() call is actually in flight and the live
 * preview is fully configured, i.e. exactly when @c liveFilterRedisplayHook
 * should recompute and repaint rather than falling back to plain
 * @c rl_redisplay().
 *
 * @return true if the live main-list repaint should run for this frame.
 */
bool liveMainListEnabled() {
    return g_livePreview.active && livePreviewConfigured();
}

/**
 * @brief Derives the text actually searched/shown for one source entry,
 * according to the current @c useUnmountKey / @c useNameOnly toggles.
 *
 * @param path Full source-list entry (a path) to derive the label from.
 * @return The unmount key, the basename, or @p path unchanged, depending
 * on which toggle (if any) is active.
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
 * @brief Builds g_livePreview.derivedCache / lowerCache exactly once, from
 * the current sourceList + useNameOnly/useUnmountKey toggles.
 *
 * Must be called after those fields are set (see runFilterLoop) and before
 * the first keystroke of a runFilterLoop() invocation is processed. The
 * result stays valid for every readline() call and every keystroke within
 * that invocation: sourceList itself cannot change until a filter is
 * actually committed, and a successful commit immediately ends the loop
 * (see the "Search-corpus caching" note above), so there's no window in
 * which a stale cache could be read.
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
 * @brief Computes the "would-be" filtered list for the in-progress query,
 * without touching filteringStack — mirrors applyFilterCore's source
 * resolution and name/unmount-key handling, but is purely a preview. Reads
 * from the per-invocation derived/lowercase caches (primeLivePreviewCaches)
 * instead of rebuilding them from @c source on every keystroke.
 *
 * @param query The in-progress (uncommitted) query text from the readline
 * input buffer. An empty query returns every entry of the source list.
 * @return The matching entries (from the preview's source list) alongside
 * their indices, already translated back to @c globalIsoFileList indices.
 */
LivePreviewResult computeLivePreviewItems(const std::string& query)
{
    const std::vector<std::string>& source = *g_livePreview.sourceList;

    LivePreviewResult result;

    if (query.empty()) {
        result.items = source;

        result.indices.resize(source.size());
        std::iota(result.indices.begin(), result.indices.end(), 0);

        return result;
    }

    const std::vector<std::string>& searchable =
        g_livePreview.hasDerivedCache ? g_livePreview.derivedCache : source;

    const std::vector<size_t> matches =
        filterFilesIndices(searchable, query, &g_livePreview.lowerCache);

    result.items.reserve(matches.size());
    result.indices.reserve(matches.size());

    for (size_t idx : matches) {
        result.items.push_back(source[idx]);

        size_t globalIdx = idx;

        // Translate from current preview source -> globalIsoFileList
        // exactly like applyFilterCore().
        if (!filteringStack.empty()) {
            for (int lvl = static_cast<int>(filteringStack.size()) - 1;
                 lvl >= 0;
                 --lvl)
            {
                const auto& lvlIndices = filteringStack[lvl].originalIndices;

                if (globalIdx < lvlIndices.size())
                    globalIdx = lvlIndices[globalIdx];
            }
        }

        result.indices.push_back(globalIdx);
    }

    return result;
}

/**
 * @brief Installed as rl_redisplay_function for the lifetime of one
 * FilterTerms readline() call; invoked by readline on (almost) every
 * keystroke, including backspace, so the real list both narrows and widens
 * live as the query changes.
 */
void liveFilterRedisplayHook() {
    if (!liveMainListEnabled()) {
        rl_redisplay();
        return;
    }

    const std::string query(rl_line_buffer ? rl_line_buffer : "");
    const bool isFirstFrameOfThisCall = !g_livePreview.primed;
    g_livePreview.primed = true;

    if (query.empty() && isFirstFrameOfThisCall && !g_livePreview.everRepainted) {
        // Truly pristine: the screen already shows exactly this (unfiltered)
        // list, untouched since before this FilterTerms prompt began — just
        // let readline draw its own prompt line without repainting above it.
        g_livePreview.lastQuery = query;
        rl_redisplay();
        return;
    }

    if (!isFirstFrameOfThisCall && query == g_livePreview.lastQuery) {
        // Text unchanged (e.g. pure cursor movement) — the terminal still
        // matches what we last painted, so the ordinary incremental
        // redisplay is correct, and cheaper than a full repaint.
        rl_redisplay();
        return;
    }
    g_livePreview.lastQuery = query;

    LivePreviewResult preview = computeLivePreviewItems(query);

    clearScrollBuffer();

    size_t previewPage      = query.empty() ? *g_livePreview.actualCurrentPage : 0;

    // Live preview has its own temporary index mapping.
    // This makes printList() display indexes relative to the original ISO list
    // without modifying the real filtering stack.
    FilteringState previewState;
    previewState.originalIndices = preview.indices;

    const bool addPreviewStack = !query.empty();

    if (addPreviewStack)
        filteringStack.push_back(std::move(previewState));

    printList(preview.items,
              g_livePreview.listType,
              g_livePreview.listSubType,
              *g_livePreview.pendingIndices,
              *g_livePreview.hasPendingProcess,
              previewPage,
              g_livePreview.state);

    if (addPreviewStack)
        filteringStack.pop_back();

    // We just repainted the whole screen out from under readline; force it
    // to redraw its prompt + in-progress query fresh rather than attempting
    // an incremental diff against a screen state that no longer exists.
    rl_forced_update_display();
    g_livePreview.everRepainted = true;
}

/**
 * @brief RAII guard that installs/tears down the live preview hook for the
 * lifetime of one runFilterLoop() call. Guarantees rl_redisplay_function is
 * restored on every exit path — normal break, continue-driven loop exit, or
 * an exception propagating out of a keystroke (e.g. filterFilesIndices
 * rethrowing a worker exception).
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
 * @brief Runs an interactive filter session.
 *
 * @param promptText   The prompt text to display.
 * @param ctx          FilterContext containing state.
 * @param onSuccess    Callback invoked when a filter is successfully applied.
 * @param onEmptyInput Callback invoked when input is empty or cancelled.
 */
static void runFilterLoop(const std::string& promptText, FilterContext& ctx,
    const std::function<void()>& onSuccess,
    const std::function<void()>& onEmptyInput = nullptr)
{
    auto tryFilter = [&](const std::string& query) -> bool {
        return applyFilterCore(query, ctx);
    };

    auto defaultEmptyInput = [&]() {
        std::cout << AnsiEscape::CLEAR_TWO_LINES;
        ctx.needsClrScrn = false;
    };

    const auto& handleEmpty = onEmptyInput ? onEmptyInput : defaultEmptyInput;

    std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    // Installed for the whole loop (covers every readline() call below, and
    // guarantees cleanup on all exit paths, including thrown exceptions).
    LivePreviewGuard livePreviewGuard;

    // Point the live preview at whatever runFilterLoop itself would search
    // (same source-resolution rule as applyFilterCore). This — and the
    // derived/lowercase search caches primed just below — are resolved
    // ONCE for the whole invocation, not per readline() call or per
    // keystroke: ctx.files is only ever reassigned on a successful filter
    // commit (inside applyFilterCore), and a successful commit immediately
    // breaks out of the while-loop below, so nothing here can change out
    // from under us while the loop is still running.
    const std::vector<std::string>& previewSource =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;
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

    // Expensive (name/unmount-key derivation + full lowercasing over the
    // whole source list) and, prior to this change, redone from scratch on
    // every single keystroke. Now done exactly once here, up front. Only
    // worth doing if the live preview will actually run for this session;
    // livePreviewConfigured() mirrors liveMainListEnabled() minus the
    // "readline() call currently in flight" check, which can't be true yet
    // at this point in the function.
    if (livePreviewConfigured())
        primeLivePreviewCaches();

    while (true) {
        // Per-readline()-call reset only. sourceList / useNameOnly /
        // useUnmountKey / derivedCache / lowerCache are deliberately left
        // untouched here — see the priming block above for why they stay
        // valid across every iteration of this loop.
        g_livePreview.lastQuery.clear();
        g_livePreview.primed = false;
        g_livePreview.active = true;

        std::unique_ptr<char, decltype(&std::free)> raw(
            readline(promptText.c_str()), &std::free);

        g_livePreview.active = false;

        // If the query got live-repainted onto the real list (any branch
        // below that loops back for another attempt, or that cancels out),
        // the outer caller's own needsClrScrn-driven refresh — set on
        // success by applyFilterCore below, or already true from before
        // this prompt started otherwise — repaints the screen properly
        // afterward exactly as it did before this feature existed.

        //---- Robust handling of FilterTerms prompt ----
        if (!raw || raw.get()[0] == 27) {
            if (!raw) {
                std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            }
            // EOF (Ctrl+D) - exit
            clear_history();
            break;
        }

        if (raw.get()[0] == ';'
        || strstr(raw.get(), ";;") != nullptr) {
            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            handleEmpty();
            continue;
        }

        std::string query(raw.get());
        if (tryFilter(query)) {
            saveQueryToHistory(query, ctx.filterHistory, true);
            onSuccess();
            break;
        }

        std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
    }
}

// ─── Filter stack sync ───────────────────────────────────────────────────

/**
 * @brief Performs multi-stage filtering on the global ISO file list.
 * * This block processes a stack of filtering states to progressively narrow down
 * the files displayed to the user. Each level of the @ref filteringStack applies
 * a new search query to the results of the previous level.
 * * @section filtering_logic Logic Flow:
 * 1.  **Initialization**: Starts with a full range of indices representing @ref globalIsoFileList.
 * 2.  **Iterative Filtering**: For each @ref FilteringState in the stack:
 * - Extracts filenames or full paths based on @ref displayConfig::toggleNamesOnly.
 * - Executes the @ref filterFilesIndices function with the current query.
 * - Maps the resulting local indices back to the original global file indices.
 * - Updates the active index set for the next stack iteration.
 * 3.  **Break Condition**: If any filter level results in zero matches, the "broken" flag is set,
 * the stack is cleared, and filtering is disabled.
 * 4.  **Finalization**: If matches survive all levels, the @ref filteredFiles list is
 * repopulated using the final set of surviving global indices.
 * * @note This implementation uses `std::move` on the index vector to optimize performance
 * during transition between stack levels.
 * * @pre `isFiltered` must be true and `filteringStack` must not be empty.
 * @post `filteredFiles` will contain the subset of `globalIsoFileList` that satisfies all queries,
 * or will be cleared if no matches are found.
 */
void syncFilteringStackForIso(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered)
{
    if (!isFiltered || filteringStack.empty()) {
        return;
    }

    // Initialize currentIndices with all possible file indices [0, 1, ..., N-1]
    std::vector<size_t> currentIndices(globalIsoFileList.size());
    std::iota(currentIndices.begin(), currentIndices.end(), 0);

    bool broken = false;

    // Iterate through each filter in the stack
    for (auto& state : filteringStack) {
        std::vector<std::string> searchList;
        searchList.reserve(currentIndices.size());

        // Prepare the strings to search (Full Path vs File Name only)
        for (size_t idx : currentIndices) {
            const std::string& path = globalIsoFileList[idx];
            if (displayConfig::toggleNamesOnly) {
                size_t lastSlash = path.find_last_of('/');
                searchList.push_back(lastSlash != std::string::npos ? path.substr(lastSlash + 1) : path);
            } else {
                searchList.push_back(path);
            }
        }

        // Apply the filter query to the current subset
        auto localMatches = filterFilesIndices(searchList, state.query);

        if (localMatches.empty()) {
            broken = true;
            break;
        }

        // Map local relative indices back to the global indices
        std::vector<size_t> nextIndices;
        nextIndices.reserve(localMatches.size());
        for (size_t localIdx : localMatches) {
            nextIndices.push_back(currentIndices[localIdx]);
        }

        // Update the stack state and the "active" working set for the next iteration
        state.originalIndices = nextIndices;
        currentIndices = std::move(nextIndices);
    }

    // Finalize results
    if (broken) {
        filteringStack.clear();
        filteredFiles.clear();
        isFiltered = false;
    } else {
        filteredFiles.clear();
        filteredFiles.reserve(currentIndices.size());
        for (size_t idx : currentIndices) {
            filteredFiles.push_back(globalIsoFileList[idx]);
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 * @details Validates the input string, builds the readline prompt, constructs a
 * @c FilterContext from @p cfg, then delegates to @c runFilterLoop. Returns early
 * without side effects if @p inputString is not @c "/".
 *
 * While the nested FilterTerms prompt is active, pending asynchronous UI
 * refreshes (see @c GlobalState::g_pendingRefreshKind / @c checkPendingRefresh)
 * are suppressed via @c GlobalState::g_suppressPendingRefresh. This is
 * necessary because @c checkPendingRefresh runs from Readline's event hook and
 * would otherwise repaint the ISO list mid-call while this different, nested
 * @c readline() prompt owns the terminal — desyncing Readline's internal
 * cursor/line state and crashing. The suppression flag is set for the
 * duration of this function via an RAII guard (a @c shared_ptr<void> with a
 * custom deleter, to avoid a dedicated named type) and cleared on every exit
 * path, including early return. Suppressed refresh requests are not dropped;
 * @c checkPendingRefresh leaves the pending request in place and retries once
 * suppression is lifted.
 *
 * @param inputString  Raw input from the user; must be exactly @c "/" to trigger filtering.
 * @param cfg          Configuration struct with all state pointers and display options.
 *                     All non-optional pointer fields must be non-null.
 * @return @c true if @p inputString was recognised as a filter command and handled,
 *         @c false otherwise.
 */
bool runSharedFilterFlow(const std::string& inputString, const FilterCallConfig& cfg)
{
    if (inputString != "/")
        return false;

    GlobalState::g_suppressPendingRefresh.store(true);
        std::shared_ptr<void> suppressGuard(nullptr, [](void*) {
            GlobalState::g_suppressPendingRefresh.store(false);
        });

	rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
	rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
	std::cout << "\n";
	reset_custom_keybindingsForSelect();
	rl_bind_keyseq("\\e", exit_handler);
	std::cout << "\033[1A\033[K";

    const ReadlineAndPromptTheme ft = getFilterTheme("", false);
    const std::string prompt =
        "\n" +
        ft.filter  + "Filter: " +
        ft.reset;

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
 * @details Resolves the correct source list based on the current filter and unmount
 * state, then forwards to @c runSharedFilterFlow. The source list priority is:
 * -# @p filteredFiles — if a filter is already active
 * -# @p isoDirs       — if in unmount mode with no active filter
 * -# @c globalIsoFileList — otherwise
 *
 * @param inputString    Raw user input; must start with @c '/' to trigger filtering.
 * @param filteredFiles  The currently displayed (possibly already filtered) file list.
 * @param isFiltered     True if @p filteredFiles is a subset of the full source list.
 * @param needsClrScrn   Set to true when the display requires a full redraw.
 * @param filterHistory  Set to true when the filter term should be saved to history.
 * @param operation      Name of the ISO operation shown in the prompt (e.g. "Mount").
 * @param operationColor Raw ANSI escape code used to colorise @p operation in the prompt.
 * @param isoDirs        Mounted ISO paths used as the source list in unmount mode.
 * @param isUnmount      True when the caller is performing an unmount operation.
 * @param currentPage    Current page index; may be reset after filtering.
 * @return @c true if @p inputString was handled as a filter command, @c false otherwise.
 */
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles,
    bool& isFiltered, bool& needsClrScrn, bool& filterHistory,
    const std::string& operation, const std::string& operationColor,
    const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage,
    std::shared_ptr<RefreshState> state)
{
    const std::vector<std::string>& baseSource =
        isFiltered ? filteredFiles : (isUnmount ? isoDirs : GlobalState::globalIsoFileList);

    FilterCallConfig cfg {
        .files          = &filteredFiles,
        .sourceOverride = &baseSource,
        .operation      = operation,
        .operationColor = operationColor,
        .isFiltered     = &isFiltered,
        .needsClrScrn   = &needsClrScrn,
        .filterHistory  = &filterHistory,
        .currentPage    = &currentPage,
        .isUnmount      = isUnmount,
        .toggleFullList = displayConfig::toggleFullListUmount
    };

    // pendingIndices/hasPendingProcess/listSubtype all already live inside
    // the shared RefreshState for this call path, so no extra parameters
    // are needed beyond state itself to enable the live main-list repaint.
    if (state) {
        cfg.listType          = isUnmount ? "MOUNTED_ISOS" : "ISO_FILES";
        cfg.listSubType       = isUnmount ? "" : state->listSubtype;
        cfg.pendingIndices    = &state->pendingIndices;
        cfg.hasPendingProcess = &state->hasPendingProcess;
        cfg.state             = state;
    }

    return runSharedFilterFlow(inputString, cfg);
}

/**
 * @brief Filter entry point for convert-to-ISO operations.
 * @details Forwards directly to @c runSharedFilterFlow using a fixed orange
 * operation color. Unlike @c handleFilteringForISO there is no unmount mode
 * or source list override — the files vector is always used as-is.
 *
 * @param inputString  Raw user input; must start with @c '/' to trigger filtering.
 * @param files        The list of convertible files to filter in place.
 * @param operation    Name of the conversion operation shown in the prompt.
 * @param isFiltered   True if @p files is already a filtered subset.
 * @param needsClrScrn Set to true when the display requires a full redraw.
 * @param filterHistory Set to true when the filter term should be saved to history.
 * @param need2Sort    Set to true when the result list needs resorting after filtering.
 * @param currentPage  Current page index; may be reset after filtering.
 */
void handleFilteringConvert2ISO(const std::string& inputString, std::vector<std::string>& files,
    const std::string& operation, bool& isFiltered, bool& needsClrScrn,
    bool& filterHistory, bool& need2Sort, size_t& currentPage,
    std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
    std::shared_ptr<RefreshState> state)
{
    FilterCallConfig cfg {
        .files          = &files,
        .operation      = operation,
        .operationColor = UI::Palette::Orange,
        .isFiltered     = &isFiltered,
        .needsClrScrn   = &needsClrScrn,
        .filterHistory  = &filterHistory,
        .need2Sort      = &need2Sort,
        .currentPage    = &currentPage
    };

    // Unlike the ISO path, pendingIndices/hasPendingProcess here are the
    // caller's own locals (selectForImageFiles), not fields of state.
    if (state) {
        cfg.listType          = "IMAGE_FILES";
        cfg.listSubType       = "convert2iso";
        cfg.pendingIndices    = &pendingIndices;
        cfg.hasPendingProcess = &hasPendingProcess;
        cfg.state             = state;
    }

    runSharedFilterFlow(inputString, cfg);
}
