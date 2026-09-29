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
#include <immintrin.h>
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
#include "../stringListView.h"
#include "../threadpool.h"

// Defined in printList.cpp. Forward-declared here (same as displayCode.cpp
// does) so the live filter preview below can repaint the real list, not a
// stand-in, on every keystroke.
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state);

// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {
    constexpr const char* CLEAR_LINE_ABOVE = "\033[1A\033[K";
    constexpr const char* CLEAR_TWO_LINES_ABOVE  = "\033[2A\033[K";
    constexpr const char* CLEAR_LINE_BELOW  = "\033[1B\033[K";
}

// ─── SIMD Search Implementation (SSE2) ───────────────────────────────────────

/**
 * @brief Blazing-fast SSE2-accelerated pattern search using 128-bit vector registers.
 *
 * Scans the provided text using 128-bit SSE2 vector instructions to find the first character
 * of the pattern simultaneously, followed by verification on candidate matches.
 *
 * @param text The text string view to search within.
 * @param pattern The pattern string view to search for.
 * @return true if pattern is found, false otherwise.
 */
inline bool simdSearchExists(std::string_view text, std::string_view pattern)
{
    const size_t n = text.size();
    const size_t m = pattern.size();
    if (m == 0 || m > n) return false;

    char firstChar = pattern[0];
    size_t i = 0;

    // SSE2 processes 16 bytes simultaneously using 128-bit vector registers
    const __m128i target = _mm_set1_epi8(firstChar);

    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data() + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, target);
        int mask = _mm_movemask_epi8(cmp);

        while (mask != 0) {
            int bitPos = __builtin_ctz(mask);
            size_t matchIndex = i + bitPos;

            if (matchIndex + m <= n && std::string_view(text.data() + matchIndex, m) == pattern) {
                return true;
            }
            mask &= mask - 1;
        }
        i += 16;
    }

    // Scalar fallback loop for remaining bytes
    for (; i <= n - m; ++i) {
        if (text[i] == firstChar && std::string_view(text.data() + i, m) == pattern) {
            return true;
        }
    }

    return false;
}

// ─── Query tokenization ──────────────────────────────────────────────────────

/**
 * @brief Builds query tokens from a semicolon-separated query string.
 *
 * Parses the query string, splits it by semicolons, determines case sensitivity
 * based on the presence of uppercase characters, and prepares lowercased versions
 * for case-insensitive matching where applicable.
 *
 * @param query The query string to tokenize.
 * @return Vector of QueryToken objects ready for searching.
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

        if (!qt.isCaseSensitive) {
            qt.lower = token;
            toLowerInPlace(qt.lower);
        }

        tokens.push_back(std::move(qt));
    }
    return tokens;
}

// ─── Core filter engine ──────────────────────────────────────────────────────

/**
 * @brief Filters file indices based on a search query using SIMD search.
 *
 * Distributes the search workload across multiple worker threads in the thread pool,
 * matching file paths or precomputed lowercased paths against individual query tokens.
 *
 * @param files Vector of file paths to filter.
 * @param query Search query with semicolon-separated terms.
 * @param precomputedLower Optional parallel array, same size as @p files, holding
 *        each entry of @p files already lowercased. When provided (and its size
 *        matches @p files), it is used in place of lowercasing each entry inline,
 *        letting a caller that runs this same @p files list through many queries
 *        in a row (e.g. the live filter preview, once per keystroke) lowercase
 *        each entry exactly once instead of on every call. Pass nullptr (the
 *        default) to preserve the original per-call lowercasing behavior.
 * @return Vector of indices matching the search criteria.
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
                            match = simdSearchExists(file, qt.original);
                        } else {
                            match = simdSearchExists(*fileLower, qt.lower);
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

/**
 * @brief Filters only a previously established subset of file indices.
 *
 * The returned indices remain indices into @p files, not indices into
 * @p candidateIndices. This allows a progressively extended query such as
 * "is" -> "iso" -> "isob" to search only the previous match set without
 * requiring any string copies.
 *
 * @param files Vector of all file paths.
 * @param candidateIndices Subset of indices from previous filtering step to evaluate.
 * @param query Search query to apply.
 * @param precomputedLower Optional parallel array of lowercased file paths.
 * @return Vector of indices matching the refined criteria.
 */
static std::vector<size_t> filterFilesIndicesSubset(
    const std::vector<std::string>& files,
    const std::vector<size_t>& candidateIndices,
    const std::string& query,
    const std::vector<std::string>* precomputedLower)
{
    if (files.empty() || candidateIndices.empty() || query.empty())
        return {};

    const std::vector<QueryToken> queryTokens = buildQueryTokens(query);
    if (queryTokens.empty())
        return candidateIndices;

    const bool needLower = std::any_of(
        queryTokens.begin(), queryTokens.end(),
        [](const QueryToken& qt) { return !qt.isCaseSensitive; });

    const bool useCachedLower =
        precomputedLower && precomputedLower->size() == files.size();

    ThreadPool& pool = getStaticThreadPool();

    const size_t numThreads = std::min({
        pool.threadCount(),
        candidateIndices.size(),
        static_cast<size_t>(GlobalConcurrency::FILTER_THREAD_CAP)
    });

    const size_t chunkSize =
        (candidateIndices.size() + numThreads - 1) / numThreads;

    std::vector<std::future<std::vector<size_t>>> futures;
    futures.reserve(numThreads);

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(candidateIndices.size(),
                                      start + chunkSize);

        if (start >= end)
            break;

        futures.emplace_back(pool.enqueue(
            [&files, &candidateIndices, start, end,
             needLower, &queryTokens, precomputedLower,
             useCachedLower]() -> std::vector<size_t> {

                std::vector<size_t> localMatches;
                localMatches.reserve((end - start) / 4);

                std::string fileLowerScratch;
                if (needLower && !useCachedLower)
                    fileLowerScratch.reserve(256);

                for (size_t candidatePos = start;
                     candidatePos < end;
                     ++candidatePos) {

                    const size_t fileIndex = candidateIndices[candidatePos];
                    const std::string& file = files[fileIndex];

                    const std::string* fileLower = nullptr;

                    if (needLower) {
                        if (useCachedLower) {
                            fileLower = &(*precomputedLower)[fileIndex];
                        } else {
                            fileLowerScratch = file;
                            toLowerInPlace(fileLowerScratch);
                            fileLower = &fileLowerScratch;
                        }
                    }

                    for (const auto& qt : queryTokens) {
                        bool match;

                        if (qt.isCaseSensitive) {
                            match = simdSearchExists(file, qt.original);
                        } else {
                            match = simdSearchExists(*fileLower, qt.lower);
                        }

                        if (match) {
                            // Preserve the original index into `files`.
                            localMatches.push_back(fileIndex);
                            break;
                        }
                    }
                }

                return localMatches;
            }
        ));
    }

    std::vector<size_t> filteredIndices;
    filteredIndices.reserve(candidateIndices.size());

    std::exception_ptr firstException;

    for (auto& fut : futures) {
        try {
            auto chunk = fut.get();

            filteredIndices.insert(
                filteredIndices.end(),
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
 * @brief Translates a filter match's local index (relative to the current
 * display list) into its corresponding globalIsoFileList index.
 *
 * filteringStack[lvl].originalIndices always stores fully-resolved global
 * indices at every level (see syncFilteringStackForIso, which builds each
 * level from currentIndices — itself already global from the prior
 * iteration). So resolving a local index only ever requires one lookup,
 * against the top of the stack; it must not walk back through earlier
 * levels, whose entries are unrelated positions once idx no longer refers
 * to them.
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
 *
 * Transforms source paths into searchable strings based on context (e.g.,
 * filename only or unmount-specific keys). Chains new results through the existing
 * `filteringStack` to ensure that local indices are correctly mapped back to the
 * global database indices. Manages UI state by resetting pagination and marking the screen for refresh.
 *
 * @param searchString The substring pattern to filter by (saved for state recovery).
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return true if matches were found and the filter stack was updated;
 * false if the query is empty or no matches exist.
 */
static bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {
    if (searchString.empty()) {
        std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
        return false;
    }

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
        if (matchedIndices.empty()) {
            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            return false;
        }

        tempFiltered.reserve(matchedIndices.size());
        tempIndices.reserve(matchedIndices.size());
        for (size_t idx : matchedIndices) {
            tempFiltered.push_back(sourceList[idx]);
            tempIndices.push_back(idx);
        }
    } else {
        tempIndices = filterFilesIndices(sourceList, searchString);
        if (tempIndices.empty()) {
            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            return false;
        }

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

    for (size_t idx : tempIndices)
        newState.originalIndices.push_back(resolveGlobalIndex(idx));

    filteringStack.push_back(std::move(newState));

    ctx.isFiltered = true;
    return true;
}

// ─── History helpers ─────────────────────────────────────────────────────────

/**
 * @brief Saves a search query to readline history.
 *
 * @param query The query string to save.
 * @param filterHistory Reference to filter history flag.
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

namespace {

struct LivePreviewResult {
    std::vector<size_t> local;
    std::vector<size_t> global;
    bool                identity = false;
};

struct LiveFilterPreview {
    const std::vector<std::string>* sourceList     = nullptr;
    bool                             hasLastMatches = false;
    bool                             useNameOnly    = false;
    bool                             useUnmountKey  = false;

    bool*   actualIsFiltered = nullptr;
    size_t* actualCurrentPage = nullptr;

    std::string                   listType{};
    std::string                   listSubType{};
    std::vector<std::string>*     pendingIndices    = nullptr;
    bool*                         hasPendingProcess = nullptr;
    std::shared_ptr<RefreshState> state = nullptr;

    bool                      hasDerivedCache = false;
    std::vector<std::string>  derivedCache;
    std::vector<std::string>  lowerCache;

    std::string lastQuery;
    std::vector<size_t> lastMatches;

    bool        primed        = false;
    bool        everRepainted = false;
    bool        active        = false;
};

LiveFilterPreview g_livePreview;

/**
 * @brief Checks if live filter previewing is correctly configured and enabled.
 * @return true if configured and active, false otherwise.
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
 * @brief Determines whether the main list live filter preview is enabled.
 * @return true if live preview should be executed during redisplay.
 */
bool liveMainListEnabled() {
    return g_livePreview.active && livePreviewConfigured();
}

/**
 * @brief Generates the preview label for an entry based on active display options.
 * @param path The target file or mount path.
 * @return The formatted label string.
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
 * @brief Primes caches required for efficient live filter evaluation.
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
 * @brief Computes search matches for the live filter preview framework.
 * @param query The current filter query typed in the input line.
 * @return LivePreviewResult containing local and global matching indices.
 */
LivePreviewResult computeLivePreviewMatches(const std::string& query)
{
    LivePreviewResult result;

    if (query.empty()) {
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

struct UnpaginatedScope {
    const std::size_t saved = GlobalState::ITEMS_PER_PAGE;
    UnpaginatedScope() { GlobalState::ITEMS_PER_PAGE = 0; }
    ~UnpaginatedScope() { GlobalState::ITEMS_PER_PAGE = saved; }
    UnpaginatedScope(const UnpaginatedScope&) = delete;
    UnpaginatedScope& operator=(const UnpaginatedScope&) = delete;
};

/**
 * @brief Readline redisplay hook used to update the live filter preview on each keystroke.
 */
void liveFilterRedisplayHook() {
    if (!liveMainListEnabled()) {
        rl_redisplay();
        return;
    }

    const std::string query(rl_line_buffer ? rl_line_buffer : "");
    const bool isFirstFrameOfThisCall = !g_livePreview.primed;
    g_livePreview.primed = true;

    if (!isFirstFrameOfThisCall && query == g_livePreview.lastQuery) {
        rl_redisplay();
        return;
    }

    LivePreviewResult preview = computeLivePreviewMatches(query);

    g_livePreview.lastQuery = query;

    clearScrollBuffer();

    size_t previewPage = 0;

    {
        const std::vector<std::string>& source = *g_livePreview.sourceList;
        const StringListView view = preview.identity
            ? StringListView(source)
            : StringListView(source, preview.local);

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
        } stackGuard(std::move(preview.global), !preview.identity);

        UnpaginatedScope unpaginated;
        printList(view,
                  g_livePreview.listType,
                  g_livePreview.listSubType,
                  *g_livePreview.pendingIndices,
                  *g_livePreview.hasPendingProcess,
                  previewPage,
                  g_livePreview.state);
    }

    rl_forced_update_display();
    g_livePreview.everRepainted = true;
}

struct LivePreviewGuard {
    LivePreviewGuard() { rl_redisplay_function = liveFilterRedisplayHook; }
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
 * @brief Runs the interactive filtering loop with readline prompt and hooks.
 *
 * @param promptText The text displayed at the input prompt.
 * @param ctx Context structures managing files and UI states.
 * @param onSuccess Callback executed when a filter query succeeds.
 * @param onEmptyInput Optional callback executed when input is empty.
 */
static void runFilterLoop(const std::string& promptText, FilterContext& ctx,
    const std::function<void()>& onSuccess,
    const std::function<void()>& onEmptyInput = nullptr)
{
    auto tryFilter = [&](const std::string& query) -> bool {
        return applyFilterCore(query, ctx);
    };

    auto defaultEmptyInput = [&]() {
        std::cout << AnsiEscape::CLEAR_TWO_LINES_ABOVE;
        ctx.needsClrScrn = false;
    };

    const auto& handleEmpty = onEmptyInput ? onEmptyInput : defaultEmptyInput;

    std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    LivePreviewGuard livePreviewGuard;

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

    if (livePreviewConfigured())
        primeLivePreviewCaches();

    while (true) {
        g_livePreview.lastQuery.clear();
        g_livePreview.lastMatches.clear();
        g_livePreview.primed = false;
        g_livePreview.active = true;

        std::unique_ptr<char, decltype(&std::free)> raw(
            readline(promptText.c_str()), &std::free);

        g_livePreview.active = false;

        if (!raw || raw.get()[0] == 27) {
            if (!raw) {
                std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            }
            if (g_livePreview.everRepainted)
                ctx.needsClrScrn = true;

            clear_history();
            break;
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
 * @brief Synchronizes the filtering stack against updated global file lists for ISOs.
 *
 * Re-evaluates existing query filters in the stack against the current file database
 * to keep indices consistent when external changes or refreshes occur.
 *
 * @param globalIsoFileList Reference global file list.
 * @param filteringStack Active filtering stack to synchronize.
 * @param filteredFiles Target filtered files collection to update.
 * @param isFiltered Reference to active filter state boolean.
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

    std::vector<size_t> currentIndices(globalIsoFileList.size());
    std::iota(currentIndices.begin(), currentIndices.end(), 0);

    bool broken = false;

    for (auto& state : filteringStack) {
        std::vector<std::string> searchList;
        searchList.reserve(currentIndices.size());

        for (size_t idx : currentIndices) {
            const std::string& path = globalIsoFileList[idx];
            if (displayConfig::toggleNamesOnly) {
                size_t lastSlash = path.find_last_of('/');
                searchList.push_back(lastSlash != std::string::npos ? path.substr(lastSlash + 1) : path);
            } else {
                searchList.push_back(path);
            }
        }

        auto localMatches = filterFilesIndices(searchList, state.query);

        if (localMatches.empty()) {
            broken = true;
            break;
        }

        std::vector<size_t> nextIndices;
        nextIndices.reserve(localMatches.size());
        for (size_t localIdx : localMatches) {
            nextIndices.push_back(currentIndices[localIdx]);
        }

        state.originalIndices = nextIndices;
        currentIndices = std::move(nextIndices);
    }

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

// ─── Public API ────────────────────────────────______________________________

/**
 * @brief Runs the shared filter flow when triggered by the shortcut input string.
 *
 * @param inputString Character input triggering the filter flow.
 * @param cfg Configuration options for the filter call.
 * @return true if the filter flow was handled, false otherwise.
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
	std::cout << AnsiEscape::CLEAR_LINE_ABOVE;

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
 * @brief Entry point for filtering operations specific to ISO files and mounts.
 *
 * @param inputString Trigger input character.
 * @param filteredFiles Output vector for filtered files.
 * @param isFiltered Reference to filter state flag.
 * @param needsClrScrn Reference to clear screen trigger flag.
 * @param filterHistory Reference to history tracking flag.
 * @param operation Current operation descriptor.
 * @param operationColor Color code styling string.
 * @param isoDirs Vector of source ISO directories.
 * @param isUnmount Flag indicating unmount context.
 * @param currentPage Current pagination index.
 * @param state Shared refresh state reference.
 * @return true if filtering was handled successfully.
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
 * @brief Specialized filter handler wrapper for convert-to-ISO flows.
 *
 * @param inputString Trigger input string.
 * @param files File list vector.
 * @param operation Operation descriptor string.
 * @param isFiltered Reference to filter state flag.
 * @param needsClrScrn Reference to clear screen trigger flag.
 * @param filterHistory Reference to history flag.
 * @param need2Sort Reference to sorting requirement flag.
 * @param currentPage Current pagination index.
 * @param pendingIndices Pending processing indices vector.
 * @param hasPendingProcess Reference to pending process flag.
 * @param state Shared refresh state reference.
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

    if (state) {
        cfg.listType          = "IMAGE_FILES";
        cfg.listSubType       = "convert2iso";
        cfg.pendingIndices    = &pendingIndices;
        cfg.hasPendingProcess = &hasPendingProcess;
        cfg.state             = state;
    }

    runSharedFilterFlow(inputString, cfg);
}
