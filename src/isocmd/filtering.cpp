// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
#include <unordered_map>
#include <unordered_set>
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
#include "../listResizeWatcher.h"
#include "../readline.h"
#include "../sharedRefreshState.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../state.h"
#include "../stringListView.h"
#include "../threadpool.h"

// Defined in printList.cpp; forward-declared so the live filter preview can
// repaint the real list on every keystroke. The list is passed as a non-owning
// StringListView, so no strings are copied.
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst = true, bool showPendingIndices = true);

// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {
    constexpr const char* CLEAR_LINE_ABOVE = "\033[1A\033[K";
    constexpr const char* CLEAR_TWO_LINES_ABOVE  = "\033[2A\033[K";
    constexpr const char* CLEAR_LINE_BELOW  = "\033[1B\033[K";
}

// ─── SIMD Search Implementation (SSE2) ───────────────────────────────────────

/**
 * @brief Returns true if @p pattern occurs anywhere in @p text, using SSE2
 * to scan 16 start positions per iteration.
 *
 * Broadcasts the pattern's first and last byte into two 128-bit registers,
 * loads two 16-byte blocks of @p text (at @c i and @c i + m - 1), and ANDs
 * their equality masks. Every set bit is a start position whose first and
 * last bytes match; only those candidates are verified by comparing the
 * @c m - 2 middle bytes with @c memcmp. Single-byte patterns use @c memchr.
 * The final (fewer than 16) start positions are handled by a scalar loop
 * that begins exactly where the vector loop stopped, so no position is
 * skipped or tested twice.
 *
 * Complexity: O(n) scan plus one verification per first/last-byte hit; worst
 * case O(n * m) for adversarial input where many positions share both end
 * bytes with the pattern.
 *
 * Platform: requires x86/x86-64 (SSE2 is part of the x86-64 baseline) and a
 * compiler providing @c __builtin_ctz (GCC/Clang). Not buildable on other
 * architectures or MSVC as written.
 *
 * Safety: the vector loop runs only while @c i + m + 15 <= n, so both
 * unaligned 16-byte loads stay inside @p text. Bounds are written as
 * additions (@c i + m <= n) rather than @c n - m, and @c m <= n is checked
 * first, so no unsigned subtraction can wrap.
 *
 * Case handling is the caller's job: pass already-lowercased text/pattern
 * for a case-insensitive match.
 *
 * @param text    The text to search within.
 * @param pattern The substring to look for.
 * @return true if @p pattern occurs in @p text. An empty @p pattern, or one
 *         longer than @p text, never matches.
 */
inline bool simdSearchExists(std::string_view text, std::string_view pattern)
{

    const size_t n = text.size();
    const size_t m = pattern.size();
    if (m == 0 || m > n) return false;

    if (m == 1) {
        return std::memchr(text.data(), pattern[0], n) != nullptr;
    }

    const char* const t = text.data();
    const char* const p = pattern.data();
    const __m128i first = _mm_set1_epi8(p[0]);
    const __m128i last = _mm_set1_epi8(p[m - 1]);
    size_t i = 0;

    for (; i + m + 15 <= n; i += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(t + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(t + i + m - 1));
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(
            _mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last))));

        while (mask != 0) {
            const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
            // First and last bytes already match; compare the middle (m - 2 bytes).
            if (std::memcmp(t + i + bit + 1, p + 1, m - 2) == 0) return true;
            mask &= mask - 1;
        }
    }

    // Scalar tail for the remaining start positions.
    for (; i + m <= n; ++i) {
        if (t[i] == p[0] && std::memcmp(t + i + 1, p + 1, m - 1) == 0) return true;
    }

    return false;
}

// ─── Query parsing ───────────────────────────────────────────────────────────

/**
 * One OR-branch of a query: every positive term in the group must match
 * (AND semantics). A query is a list of such groups; an entry matches if ANY
 * group's positive terms are all satisfied (OR semantics). Negated terms
 * (see @c QueryToken::negated) are parsed into their group but applied
 * globally, to the whole query (see @c matchesAnyGroup).
 */
using QueryGroup = std::vector<QueryToken>;

/**
 * @brief True if @p query ends in an unpaired backslash, i.e. an escape
 * still waiting for the character it escapes. Backslashes pair left to
 * right ("\\\\" is one literal '\'), so only an odd-length trailing run is
 * dangling.
 */
static bool hasDanglingBackslash(const std::string& query) {
    size_t run = 0;
    for (size_t i = query.size(); i > 0 && query[i - 1] == '\\'; --i)
        ++run;
    return (run % 2) == 1;
}

/**
 * @brief Parses a query string into OR-ed groups of AND-ed terms.
 *
 * Grammar:
 * @code
 *   query := group (';' group)*       // unescaped ';' = OR
 *   group := term  ('&' term)*        // unescaped '&' = AND
 *   term  := '!'? text                // leading unescaped '!' negates the term
 *   escape := '\\&' | '\\;' | '\\!' | '\\\\'   // literal '&', ';', '!' or '\'
 * @endcode
 * AND binds tighter than OR, so "a&b;c" means (a AND b) OR c.
 * Use "\\&" or "\\;" to search for a literal delimiter, "\\!" for a literal
 * '!', and "\\\\" for a literal backslash. A backslash before any other
 * character is preserved as part of the term.
 *
 * Negation: an unescaped '!' at the very START of a term negates that term.
 * Negation is global: a negated term excludes entries containing it
 * regardless of which OR-group it was written in. "a;b&!t&!g" means
 * "(a OR b) AND NOT t AND NOT g", and "!a;b" means "contains b and does not
 * contain a". A '!' elsewhere in a term is literal. Only the first '!' of a
 * term negates: "!!a" negates the literal text "!a". A lone "!" is an empty
 * term and is dropped, like any other empty term.
 *
 * Empty terms and empty groups are dropped: "a&&b" is "a&b", "a;;b" is
 * "a;b", and "a&" is "a".
 *
 * Matching is "smart case" per term: a term containing at least one
 * uppercase letter is matched case-sensitively against the original text,
 * while an all-lowercase term is matched case-insensitively (its @c lower
 * form is searched against the lowercased text). Applies to negated terms
 * the same way.
 *
 * @param query The query string to parse.
 * @param trailingEmptyGroup Optional out-parameter. Set to true when the
 *        query contains an unescaped ';' AND its last OR-branch has no term
 *        yet ("clon;", "clon;&", "clon;!"). The returned groups are
 *        unaffected (the empty branch is still dropped); this only lets the
 *        live preview tell "clon;" apart from "clon". Set to false
 *        otherwise, including for "a;;b" and ";a", where the empty branch is
 *        not last.
 * @return The groups in query order. Empty if the query contains no
 *         non-empty term (e.g. "", ";;", "&" or "!").
 */
static std::vector<QueryGroup> buildQueryGroups(const std::string& query,
                                                bool* trailingEmptyGroup = nullptr) {
    std::vector<QueryGroup> groups;
    QueryGroup group;
    std::string term;
    bool negate = false;   // the term currently being built started with an unescaped '!'
    bool sawOr  = false;   // at least one unescaped ';' has been seen
    term.reserve(query.size());

    const auto finishTerm = [&]() {
        const bool wasNegated = negate;
        negate = false;    // reset even when the term is empty (a lone "!")
        if (term.empty())
            return;

        QueryToken qt;
        qt.original        = std::move(term);
        qt.negated         = wasNegated;
        qt.isCaseSensitive = std::any_of(
            qt.original.begin(), qt.original.end(),
            [](unsigned char c) { return std::isupper(c); });

        if (!qt.isCaseSensitive) {
            qt.lower = qt.original;
            toLowerInPlace(qt.lower);
        }

        group.push_back(std::move(qt));
        term.clear();
    };

    const auto finishGroup = [&]() {
        finishTerm();
        if (!group.empty()) {
            groups.push_back(std::move(group));
            group.clear();
        }
    };

    for (size_t i = 0; i < query.size(); ++i) {
        const char c = query[i];

        // A backslash with nothing after it is a dangling escape (mid-typing,
        // or backspaced onto it). It has no delimiter to escape, so ignore it
        // instead of searching for a literal '\'.
        if (c == '\\' && i + 1 == query.size())
            continue;

        // Backslash escapes only '&', ';', '!' and '\' itself. A backslash
        // before any other character remains part of the search term.
        if (c == '\\' && i + 1 < query.size() &&
            (query[i + 1] == '&' || query[i + 1] == ';' ||
             query[i + 1] == '!' || query[i + 1] == '\\')) {
            term.push_back(query[++i]);
        } else if (c == '&') {
            finishTerm();
        } else if (c == ';') {
            sawOr = true;
            finishGroup();
        } else if (c == '!' && term.empty() && !negate) {
            // Unescaped '!' at the start of a term: negate it. Any later '!'
            // in the same term (including a second leading one) is literal.
            negate = true;
        } else {
            term.push_back(c);
        }
    }

    // The last OR-branch is still empty if nothing has accumulated since the
    // final ';' (checked before finishGroup() flushes it). A pending '!' with
    // no text yet counts as a started term, so "clon;!" is not an empty branch.
    if (trailingEmptyGroup)
        *trailingEmptyGroup = sawOr && group.empty() && term.empty() && !negate;

    finishGroup();
    return groups;
}

/**
 * @brief True if any term in any group is case-insensitive, i.e. a
 * lowercased copy of each entry is needed to evaluate the query.
 */
static bool groupsNeedLower(const std::vector<QueryGroup>& groups) {
    for (const auto& group : groups)
        for (const auto& qt : group)
            if (!qt.isCaseSensitive) return true;
    return false;
}

/**
 * @brief True if any term in any group is negated ('!').
 *
 * Negation breaks the monotonicity that incremental narrowing relies on:
 * extending a negated term ("!a" -> "!ab") WIDENS the match set, because
 * entries containing "a" but not "ab" start to match. The live preview
 * therefore never narrows incrementally from a negated query.
 */
static bool groupsHaveNegation(const std::vector<QueryGroup>& groups) {
    for (const auto& group : groups)
        for (const auto& qt : group)
            if (qt.negated) return true;
    return false;
}

/**
 * @brief Tests one entry against a parsed query.
 *
 * Negation is GLOBAL: every negated term, in any group, excludes the whole
 * query. An entry matches when
 *  1. none of the negated terms occurs in it, AND
 *  2. at least one group is satisfied by its positive (non-negated) terms
 *     (OR across groups, AND within a group). Groups that contain only
 *     negated terms are skipped. If the query has no positive term at all,
 *     step 2 is skipped and the entry matches once step 1 passes.
 *
 * So "a;b&!t&!g" means "(a OR b) AND NOT t AND NOT g". Short-circuits at
 * every level; the cheap exclusion pass runs first.
 *
 * @param groups    Parsed query (see @c buildQueryGroups); must be non-empty.
 * @param file      The entry text, original case.
 * @param fileLower The entry text lowercased. May be nullptr only when
 *                  @c groupsNeedLower(groups) is false.
 */
static inline bool matchesAnyGroup(const std::vector<QueryGroup>& groups,
                                   const std::string& file,
                                   const std::string* fileLower)
{
    const auto termHit = [&](const QueryToken& qt) {
        return qt.isCaseSensitive
            ? simdSearchExists(file, qt.original)
            : simdSearchExists(*fileLower, qt.lower);
    };

    // Pass 1: global exclusions. Also notes whether any positive term exists.
    bool hasPositive = false;
    for (const auto& group : groups) {
        for (const auto& qt : group) {
            if (qt.negated) {
                if (termHit(qt)) return false;
            } else {
                hasPositive = true;
            }
        }
    }
    if (!hasPositive) return true;   // exclusion-only query

    // Pass 2: OR over groups, AND over each group's positive terms.
    for (const auto& group : groups) {
        bool groupHasPositive = false;
        bool allMatch = true;
        for (const auto& qt : group) {
            if (qt.negated) continue;
            groupHasPositive = true;
            if (!termHit(qt)) {
                allMatch = false;
                break;
            }
        }
        if (groupHasPositive && allMatch) return true;
    }
    return false;
}

// ─── Core filter engine ──────────────────────────────────────────────────────

/**
 * @brief Returns the ascending indices of @p files matching @p query, using
 * SIMD substring search on a thread pool.
 *
 * The list is split into contiguous chunks, one per worker (bounded by the
 * pool size, the list size and @c GlobalConcurrency::FILTER_THREAD_CAP).
 * Each worker tests its chunk and returns matching indices in ascending
 * order; chunks are concatenated in order, so the result is ascending
 * overall. If any worker throws, the first exception is rethrown after all
 * workers have been collected.
 *
 * @param files Vector of entries to filter.
 * @param query Search query; unescaped ';' separates OR-ed groups and
 *        unescaped '&' separates AND-ed terms. A leading unescaped '!' on a
 *        term negates it. Use '\\&', '\\;', '\\!' and '\\\\' to search for
 *        literal delimiters / '!' / backslash (see @c buildQueryGroups).
 * @param precomputedLower Optional array parallel to @p files holding each
 *        entry already lowercased. When provided (and its size matches
 *        @p files), it is used in place of lowercasing inline, letting a
 *        caller that runs this same list through many queries in a row (e.g.
 *        the live filter preview, once per keystroke) lowercase each entry
 *        exactly once instead of on every call. Pass nullptr (the default)
 *        for per-call lowercasing. A cache whose size does not match is
 *        ignored, never trusted.
 * @return Ascending indices into @p files of the matching entries. Empty if
 *         @p files or @p query is empty. If the query has no non-empty term
 *         (e.g. ";", "&" or "!"), every index is returned.
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query,
                                        const std::vector<std::string>* precomputedLower)
{
    if (files.empty() || query.empty()) return {};

    const std::vector<QueryGroup> queryGroups = buildQueryGroups(query);
    if (queryGroups.empty()) {
        std::vector<size_t> allIndices(files.size());
        std::iota(allIndices.begin(), allIndices.end(), 0);
        return allIndices;
    }

    const bool needLower = groupsNeedLower(queryGroups);

    // Only trust a caller-supplied lowercase cache if it lines up with `files`;
    // otherwise fall back to lowercasing inline so a stale/mismatched cache
    // can never produce wrong indices.
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
            [&files, start, end, needLower, &queryGroups, precomputedLower, useCachedLower]() -> std::vector<size_t> {
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

                    if (matchesAnyGroup(queryGroups, file, fileLower))
                        localMatches.push_back(j);
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
 * @brief Like @c filterFilesIndices, but tests only a previously established
 * subset of @p files instead of the whole list.
 *
 * Used by the live preview for incremental narrowing: when the user extends
 * the previous query ("is" -> "iso" -> "isob"), every entry matching the new
 * query must already have matched the old one, so only the previous match set
 * needs re-testing. No strings are copied.
 *
 * Returned indices remain indices into @p files (not positions within
 * @p candidateIndices) and stay in the candidates' order (ascending when
 * @p candidateIndices is ascending, as produced by the filter functions).
 *
 * Correctness precondition: the caller must only pass a candidate set that is
 * a superset of the new query's matches. The live preview guarantees this by
 * using it only when the new query strictly extends the previous one, neither
 * contains ';', and the new query has no negated term (see
 * @c computeLivePreviewMatches). '&' queries qualify: with a single AND-group,
 * extending the text can only lengthen a term or add another AND-ed term, both
 * of which only tighten the match. Negated ('!') terms do NOT qualify:
 * extending a negated term ("!a" -> "!ab") widens the match set, so the
 * previous matches would be an incomplete candidate set.
 *
 * @param files Vector of all entries; candidates index into this.
 * @param candidateIndices Indices into @p files to evaluate (typically the
 *        previous keystroke's matches).
 * @param query Search query to apply (same syntax as @c filterFilesIndices).
 * @param precomputedLower Optional array of lowercased entries parallel to
 *        @p files; ignored if the size does not match.
 * @return Indices into @p files (a subset of @p candidateIndices) that match.
 *         Empty if @p files, @p candidateIndices or @p query is empty; the
 *         candidates unchanged if the query has no non-empty term.
 */
static std::vector<size_t> filterFilesIndicesSubset(
    const std::vector<std::string>& files,
    const std::vector<size_t>& candidateIndices,
    const std::string& query,
    const std::vector<std::string>* precomputedLower)
{
    if (files.empty() || candidateIndices.empty() || query.empty())
        return {};

    const std::vector<QueryGroup> queryGroups = buildQueryGroups(query);
    if (queryGroups.empty())
        return candidateIndices;

    const bool needLower = groupsNeedLower(queryGroups);

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
             needLower, &queryGroups, precomputedLower,
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

                    if (matchesAnyGroup(queryGroups, file, fileLower)) {
                        // Preserve the original index into `files`.
                        localMatches.push_back(fileIndex);
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
 * @brief Executes the core filter commit: transforms source paths into
 * searchable strings (filename only, unmount keys, or raw paths), runs the
 * query, and chains the result through @c filteringStack so local indices map
 * back to global database indices. Resets pagination and marks the screen for
 * refresh.
 *
 * This is the COMMIT path, run when the user presses Enter. It is separate
 * from the live preview (see the "Live incremental filter preview" section),
 * which never touches @c filteringStack or @c ctx.files.
 *
 * @param searchString The query to filter by (saved for state recovery).
 *        Unescaped ';' separates OR-ed groups and unescaped '&' separates
 *        AND-ed terms; a leading unescaped '!' on a term negates it.
 *        '\\;', '\\&', '\\!' and '\\\\' match a literal ';', '&', '!' and '\'.
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return true if matches were found and the filter stack was updated; false
 *         if the query is empty or no matches exist. Also returns true,
 *         without pushing a new level, when the query matches every entry
 *         (the filter would change nothing).
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
    newState.namesOnly  = useNameOnly;
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
// History/motivation and the overall design are summarized inline below;
// the invariants a reader must know are:
//
//  - Every keystroke triggers rl_redisplay_function; we hook it to recompute
//    matches for the in-progress (uncommitted) query and repaint the REAL
//    list via printList(), right where it is already displayed.
//  - The preview is ALWAYS rendered unpaginated (UnpaginatedScope sets
//    ITEMS_PER_PAGE = 0 around the printList() call and restores it
//    immediately). Committed (Enter) results keep normal pagination.
//  - After a full printList() repaint, readline's screen cache is stale, so
//    we call rl_forced_update_display() to redraw the prompt and in-progress
//    line fresh. When the query is unchanged (cursor movement), plain
//    rl_redisplay() suffices.
//  - Purely visual until Enter: filteringStack and ctx.files are never
//    permanently modified here. Enter runs the same commit path as before
//    (applyFilterCore + saveQueryToHistory).
//  - Zero-copy rendering: matches are computed as indices; printList()
//    receives a StringListView over the source list, optionally narrowed by
//    the matched indices. An empty query shows the whole source list with no
//    allocation at all.
//  - Search-corpus caching: the searched text (basename, unmount key, or
//    raw path) and its lowercased form depend only on the source list and
//    the name-only/unmount toggles — never the in-progress query — so both
//    are built once per runFilterLoop() call (primeLivePreviewCaches) rather
//    than rebuilt per keystroke.
//  - Incremental narrowing: when the new query strictly extends the previous
//    one ("is" -> "iso", or "iso" -> "iso&boot"), every match must already be
//    in the previous match set, so only that set is re-tested
//    (filterFilesIndicesSubset). Backspacing, pasting a different query, an
//    empty query, or ';' (OR-ed) queries fall back to a full search. '&'
//    (AND) queries qualify; negated ('!') queries never do, because
//    extending a negated term widens the match set. A previous query ending
//    in an unpaired backslash is not trusted as a prefix (see
//    hasDanglingBackslash), since the next character may turn it into an
//    escape.
//
// Trade-offs:
//  - Index tags: for a non-empty query the preview pushes a temporary
//    filteringStack entry so printList() draws the "[123]^" original-index
//    tags relative to the full list while typing. For an empty query nothing
//    is pushed and printList() uses the real stack, so an already-committed
//    filter's tags show as usual.
//  - A full printList() per keystroke is heavier than a delta redraw and can
//    flicker on slow terminals. Above LIVE_FILTER_LIMIT source items we skip
//    live repainting entirely and fall back to Enter-only behavior.
//  - The derived/lowercase caches are primed once per runFilterLoop() call
//    and never refreshed mid-loop: the source list only changes on a
//    successful commit, which immediately ends the loop.

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
 * preview frame (clear + list). Does NOT touch Readline.
 *
 * Shared by two callers, both on the Readline thread:
 *  - @c liveFilterRedisplayHook, which follows it with
 *    @c rl_forced_update_display();
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
 * with a zero-copy StringListView; (3) asks readline to redraw its prompt
 * from scratch via rl_forced_update_display().
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
 * @brief Runs an interactive filter session: shows the Filter prompt,
 * live-previews matches as the user types (when enabled), and commits the
 * filter on Enter.
 *
 * Setup, done once per call: loads filter history, installs the live preview
 * hook (LivePreviewGuard), points the preview at the list this function would
 * search (same source-resolution rule as applyFilterCore), and primes the
 * derived/lowercase search caches. ctx.files is only reassigned on a
 * successful commit (inside applyFilterCore), and a successful commit
 * immediately breaks the loop, so none of that setup can change under the
 * loop while it runs.
 *
 * Loop, one readline() per iteration: the per-call incremental-narrowing
 * state (lastQuery / lastMatches / primed) is reset each time, and the
 * readline() is wrapped by readlineWithResize() so the list is repainted if
 * the terminal width changes while the prompt waits (the watcher lives only
 * for that call, so it never overlaps another prompt's watcher). The resize
 * repaint is paintLivePreviewFrame() on the current input while the live
 * preview is running, otherwise @c ctx.repaintList (the caller's normal-list
 * repaint), otherwise nothing. Then:
 *  - Esc as the first character, or EOF (Ctrl+D), cancels: history is
 *    cleared and the loop ends. If the preview repainted the screen,
 *    ctx.needsClrScrn is set so the caller redraws the normal paginated list
 *    instead of leaving the long unpaginated one behind.
 *  - Otherwise the entered text is committed via applyFilterCore. On success
 *    the query is saved to history, @p onSuccess runs and the loop ends; on
 *    failure (empty query or no matches) the prompt is shown again.
 *
 * @param promptText   The prompt text to display.
 * @param ctx          FilterContext containing state, including the optional
 *                     @c repaintList used for resize repaints.
 * @param onSuccess    Callback invoked when a filter is successfully applied.
 * @param onEmptyInput Optional callback for empty/cancelled input. Note: it
 *                     is currently bound but not invoked by the loop body;
 *                     cancellation is handled inline as described above.
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
    (void)handleEmpty;   // bound but not invoked by the loop body (see doc comment)

    std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    // Installed for the whole loop; restores rl_redisplay_function on every exit path.
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

    // Terminal-resize repaint for this prompt. While the live preview is
    // running the screen shows the unpaginated preview of the current query,
    // so repaint that. Otherwise (preview off: list above LIVE_FILTER_LIMIT,
    // or not configured) the screen shows the normal list, so use the
    // caller's repaint.
    const auto resizeRepaint = [&] {
        if (liveMainListEnabled())
            paintLivePreviewFrame(rl_line_buffer ? rl_line_buffer : "");
        else if (ctx.repaintList)
            ctx.repaintList();
    };

    while (true) {
        g_livePreview.lastQuery.clear();
        g_livePreview.lastMatches.clear();
        g_livePreview.primed = false;
        g_livePreview.active = true;

        RlLine raw = readlineWithResize(promptText, resizeRepaint);

        g_livePreview.active = false;

        //---- Robust handling of FilterTerms prompt ----
        if (!raw || raw.get()[0] == 27) {
            if (!raw) {
                std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            }
            // The live preview painted an unpaginated list over the screen.
            // Make the caller redo its normal (paginated) refresh.
            if (g_livePreview.everRepainted)
                ctx.needsClrScrn = true;

            // EOF (Ctrl+D) or Esc - exit
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
 * @brief Refreshes the active filter after the global ISO list was reloaded,
 * WITHOUT re-evaluating the entries that are already shown.
 *
 * - Entries currently displayed (@p filteredFiles) are kept as they are, as
 *   long as they still exist in @p newGlobal. They are never re-tested, so
 *   toggling filenames-only between commits cannot change them.
 * - Only paths that were NOT in the old list (@p oldPaths) are tested. Each
 *   must satisfy every level of @p filteringStack, each level evaluated
 *   against the text mode it was committed with
 *   (@c FilteringState::namesOnly).
 * - If nothing is left, the filter is cleared (same as the old "broken" case).
 *
 * Only the top level's @c originalIndices is rewritten: resolveGlobalIndex()
 * and printList()'s "[N]^" tags read only the top of the stack. Lower levels'
 * stored indices go stale after a reload but are never read.
 *
 * @param newGlobal        The freshly loaded, sorted global ISO list.
 * @param oldPaths         Paths of the global list BEFORE the reload.
 * @param filteringStack   Active filter levels; updated in place.
 * @param filteredFiles    In/out: the displayed filtered list.
 * @param isFiltered       In/out: set to false if nothing remains.
 */
void refreshFilterKeepingResults(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered)
{
    if (!isFiltered || filteringStack.empty()) return;

    const auto textFor = [](const std::string& p, bool namesOnly) -> std::string {
        if (!namesOnly) return p;
        const size_t s = p.find_last_of('/');
        return s != std::string::npos ? p.substr(s + 1) : p;
    };

    // path -> index in the new global list
    std::unordered_map<std::string, size_t> indexOf;
    indexOf.reserve(newGlobal.size());
    for (size_t i = 0; i < newGlobal.size(); ++i)
        indexOf.emplace(newGlobal[i], i);

    std::vector<size_t> result;
    result.reserve(filteredFiles.size());

    // 1) Existing results that still exist: kept untouched.
    for (const auto& p : filteredFiles) {
        const auto it = indexOf.find(p);
        if (it != indexOf.end())
            result.push_back(it->second);
    }

    // 2) Newly discovered ISOs: must pass every level, each level using the
    //    text mode it was committed with. One batched search per level.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < newGlobal.size(); ++i)
        if (!oldPaths.count(newGlobal[i]))
            candidates.push_back(i);

    for (const auto& lvl : filteringStack) {
        if (candidates.empty()) break;

        std::vector<std::string> texts;
        texts.reserve(candidates.size());
        for (size_t gi : candidates)
            texts.push_back(textFor(newGlobal[gi], lvl.namesOnly));

        const std::vector<size_t> hit = filterFilesIndices(texts, lvl.query);

        std::vector<size_t> next;
        next.reserve(hit.size());
        for (size_t h : hit)
            next.push_back(candidates[h]);
        candidates = std::move(next);
    }

    result.insert(result.end(), candidates.begin(), candidates.end());

    // Global list is sorted, so ascending index == display order.
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    if (result.empty()) {
        filteringStack.clear();
        filteredFiles.clear();
        isFiltered = false;
        return;
    }

    filteredFiles.clear();
    filteredFiles.reserve(result.size());
    for (size_t idx : result)
        filteredFiles.push_back(newGlobal[idx]);

    filteringStack.back().originalIndices = std::move(result);
}

/**
 * @brief Refreshes source indices for the currently filtered ISO files.
 *
 * Maps each path in @p filteredFiles to its position in that list, then scans
 * @p globalIsoFileList once to find each path's current index there. The
 * resulting indices replace @c originalIndices of the last filtering state,
 * in the same order as @p filteredFiles. No filter queries are reapplied.
 *
 * The stack is left unmodified if @p filteringStack or @p filteredFiles is
 * empty, or if any filtered path is not found in @p globalIsoFileList (which
 * includes an empty global list).
 *
 * Keeps source indices synchronized after the global ISO list is reordered,
 * such as when toggling filenames-only display mode, while preserving the
 * current filtered results.
 *
 * @note Paths are assumed to be unique in both @p filteredFiles and
 *       @p globalIsoFileList. Only the first occurrence of a path in the
 *       global list is used.
 *
 * @param globalIsoFileList The current global ISO file list.
 * @param filteringStack    The stack of active filtering states; only the
 *                          last state's source indices are updated.
 * @param filteredFiles     The currently filtered ISO file paths. Must
 *                          outlive the call, since the lookup map holds
 *                          views into it.
 */
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles)
{
    if (filteringStack.empty() || filteredFiles.empty()) return;

    // Only the F filtered paths are stored; views avoid copying strings.
    std::unordered_map<std::string_view, size_t> wanted;
    wanted.reserve(filteredFiles.size());
    for (size_t k = 0; k < filteredFiles.size(); ++k)
        wanted.emplace(filteredFiles[k], k);

    std::vector<size_t> currentIndices(filteredFiles.size());
    size_t found = 0;

    for (size_t i = 0; i < globalIsoFileList.size() && found < wanted.size(); ++i) {
        const auto it = wanted.find(globalIsoFileList[i]);
        if (it != wanted.end()) {
            currentIndices[it->second] = i;
            ++found;
        }
    }

    if (found != wanted.size()) return;   // incomplete mapping, leave stack alone

    filteringStack.back().originalIndices = std::move(currentIndices);
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 * @details Validates the input string, builds the readline prompt, constructs
 * a @c FilterContext from @p cfg, then delegates to @c runFilterLoop. Returns
 * early without side effects if @p inputString is not @c "/".
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
 * The live main-list preview is opt-in: it is only wired up when @p cfg
 * supplies everything printList() needs (listType, pendingIndices,
 * hasPendingProcess and state); otherwise the prompt behaves as the classic
 * Enter-only filter.
 *
 * @param inputString  Raw input from the user; must be exactly @c "/" to trigger filtering.
 * @param cfg          Configuration struct with all state pointers and display options.
 *                     All non-optional pointer fields must be non-null.
 * @return @c true if @p inputString was recognised as a filter command and
 *         handled, @c false otherwise.
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
 * -# @p filteredFiles — if a filter is already active
 * -# @p isoDirs       — if in unmount mode with no active filter
 * -# @c globalIsoFileList — otherwise
 *
 * When @p state is provided it also enables the live main-list preview: the
 * list type is @c "MOUNTED_ISOS" in unmount mode and @c "ISO_FILES" otherwise,
 * and @c pendingIndices / @c hasPendingProcess / the list subtype are taken
 * from the shared @c RefreshState.
 *
 * @param inputString    Raw user input; must be exactly @c "/" to trigger filtering.
 * @param filteredFiles  The currently displayed (possibly already filtered) file list.
 * @param isFiltered     True if @p filteredFiles is a subset of the full source list.
 * @param needsClrScrn   Set to true when the display requires a full redraw.
 * @param filterHistory  Set to true when the filter term should be saved to history.
 * @param operation      Name of the ISO operation shown in the prompt (e.g. "Mount").
 * @param operationColor Raw ANSI escape code used to colorise @p operation in the prompt.
 * @param isoDirs        Mounted ISO paths used as the source list in unmount mode.
 * @param isUnmount      True when the caller is performing an unmount operation.
 * @param currentPage    Current page index; may be reset after filtering.
 * @param state          Shared refresh state; enables the live preview when non-null.
 * @param repaintList    Optional. Repaints the normal (paginated) list; called on
 *                       the Readline thread when the terminal is resized while the
 *                       Filter prompt waits and the live preview is not running.
 *                       Empty means no repaint in that case.
 * @return @c true if @p inputString was handled as a filter command, @c false otherwise.
 */
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles,
    bool& isFiltered, bool& needsClrScrn, bool& filterHistory,
    const std::string& operation, const std::string& operationColor,
    const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage,
    std::shared_ptr<RefreshState> state,
    std::function<void()> repaintList)
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
    cfg.repaintList = std::move(repaintList);

    return runSharedFilterFlow(inputString, cfg);
}

/**
 * @brief Filter entry point for convert-to-ISO operations.
 * @details Forwards directly to @c runSharedFilterFlow with a fixed orange
 * operation color. Unlike @c handleFilteringForISO there is no unmount mode
 * or source list override — the files vector is always used as-is.
 *
 * When @p state is provided it enables the live main-list preview for the
 * @c "IMAGE_FILES" list (subtype @c "convert2iso"). Unlike the ISO path,
 * @p pendingIndices and @p hasPendingProcess here are the caller's own locals
 * (selectForImageFiles), not fields of @p state.
 *
 * @param inputString  Raw user input; must be exactly @c "/" to trigger filtering.
 * @param files        The list of convertible files to filter in place.
 * @param operation    Name of the conversion operation shown in the prompt.
 * @param isFiltered   True if @p files is already a filtered subset.
 * @param needsClrScrn Set to true when the display requires a full redraw.
 * @param filterHistory Set to true when the filter term should be saved to history.
 * @param need2Sort    Set to true when the result list needs resorting after filtering.
 * @param currentPage  Current page index; may be reset after filtering.
 * @param pendingIndices    Caller's pending-selection indices, shown under the list.
 * @param hasPendingProcess Caller's flag: true if a process action is staged.
 * @param state        Shared refresh state; enables the live preview when non-null.
 * @param repaintList  Optional. Repaints the normal (paginated) list on terminal
 *                     resize when the live preview is not running; empty means
 *                     no repaint in that case.
 */
void handleFilteringConvert2ISO(const std::string& inputString, std::vector<std::string>& files,
    const std::string& operation, bool& isFiltered, bool& needsClrScrn,
    bool& filterHistory, bool& need2Sort, size_t& currentPage,
    std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
    std::shared_ptr<RefreshState> state,
    std::function<void()> repaintList)
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
    cfg.repaintList = std::move(repaintList);

    runSharedFilterFlow(inputString, cfg);
}
