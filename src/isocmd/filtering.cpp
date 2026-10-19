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
// stand-in, on every keystroke. The list is passed as a non-owning
// StringListView, so the preview never has to copy the matching strings.
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
 * @brief Tests whether @p pattern occurs anywhere in @p text, using SSE2
 * to scan 16 start positions at a time.
 *
 * Algorithm: broadcast the pattern's first and last byte into two 128-bit
 * registers, load two 16-byte blocks of @p text (at @c i and @c i + m - 1),
 * compare each against its byte, and AND the results into a bitmask. Every
 * set bit is a start position whose first and last bytes both match; only
 * those candidates are verified by comparing the @c m - 2 middle bytes with
 * @c memcmp. This removes most false candidates when the first byte is
 * common (e.g. '/' or 't' in file paths). Single-byte patterns use
 * @c memchr. The remaining start positions (fewer than 16) are checked by a
 * scalar loop that begins exactly where the vector loop stopped, so no start
 * position is skipped or checked twice.
 *
 * Complexity: O(n) scan plus one verification per first/last-byte hit.
 * Worst case is O(n * m) for adversarial input where many positions share
 * both end bytes with the pattern.
 *
 * Platform: requires x86/x86-64 (SSE2 is part of the x86-64 baseline) and a
 * compiler providing @c __builtin_ctz (GCC/Clang). It will not build on other
 * architectures or MSVC as written.
 *
 * Safety: the vector loop runs only while @c i + m + 15 <= n, so both
 * unaligned 16-byte loads stay inside @p text. All bounds are written as
 * additions (@c i + m <= n) rather than @c n - m, and @c m <= n is checked
 * first, so no unsigned subtraction can wrap.
 *
 * Case handling is the caller's job: pass an already-lowercased text/pattern
 * pair for a case-insensitive match.
 *
 * @param text    The text to search within (the entry being tested).
 * @param pattern The substring to look for.
 * @return true if @p pattern occurs in @p text; false otherwise. An empty
 *         @p pattern, or one longer than @p text, never matches.
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
 * (see @c QueryToken::negated) are parsed into their group but are applied
 * globally, to the whole query, not just that group (see @c matchesAnyGroup).
 */
using QueryGroup = std::vector<QueryToken>;

/**
 * @brief True if @p query ends in an unpaired backslash, i.e. an escape
 * still waiting for the character it escapes. Backslashes pair up left to
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
 *   escape := '\\&' | '\\;' | '\\!' | '\\\\'   // literal '&', ';', '!' or '\' inside a term
 * @endcode
 * AND binds tighter than OR, so "a&b;c" means (a AND b) OR c.
 * Use "\\&" or "\\;" to search for a literal delimiter, "\\!" to search for
 * a literal '!', and "\\\\" to search for a literal backslash. A backslash
 * before any other character is preserved as part of the term.
 *
 * Negation: an unescaped '!' at the very START of a term negates that term.
 * Negation is global: a negated term excludes entries containing it no
 * matter which OR-group it was written in. "a;b&!t&!g" means
 * "(a OR b) AND NOT t AND NOT g", and "!a;b" means "contains b and does not
 * contain a". A '!' anywhere else in a term is literal ("foo!bar" searches
 * for "foo!bar"). Only the first '!' of a term negates: "!!a" is the
 * negation of the literal text "!a". A lone "!" (no text after it) is an
 * empty term and is dropped, like any other empty term.
 *
 * Empty terms and empty groups are dropped: "a&&b" is "a&b", "a;;b" is
 * "a;b", and "a&" is "a".
 *
 * Matching is "smart case" per term: a term containing at least one
 * uppercase letter is matched case-sensitively against the original text,
 * while an all-lowercase term is matched case-insensitively (its @c lower
 * form is searched against the lowercased text). This applies to negated
 * terms the same way.
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

        // A backslash with nothing after it is a dangling escape (the user
        // is mid-typing, or backspaced onto it). It has no delimiter to
        // escape, so ignore it instead of searching for a literal '\'.
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

    // The last OR-branch is still empty if nothing has been accumulated
    // since the final ';' (checked before finishGroup() flushes it). A
    // pending '!' with no text yet counts as a started term, so "clon;!"
    // is not an empty branch.
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
 * Negation is GLOBAL: every negated term, in any group, is an exclusion that
 * applies to the whole query. An entry matches when
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
 * @brief Filters a list of entries against a query and returns the indices
 * of the entries that match, using SIMD substring search on a thread pool.
 *
 * The list is split into contiguous chunks, one per worker (bounded by the
 * pool size, the list size and @c GlobalConcurrency::FILTER_THREAD_CAP).
 * Each worker tests its chunk against the query and returns the matching
 * indices in ascending order; the chunks are concatenated in order, so the
 * result is ascending overall. If any worker throws, the first exception is
 * rethrown after all workers have been collected.
 *
 * @param files Vector of entries to filter.
 * @param query Search query; unescaped ';' separates OR-ed groups and
 *        unescaped '&' separates AND-ed terms. A leading unescaped '!' on a
 *        term negates it. Use '\\&', '\\;', '\\!' and '\\\\' to search for
 *        literal delimiters / '!' / backslash (see @c buildQueryGroups).
 * @param precomputedLower Optional parallel array, same size as @p files, holding
 *        each entry of @p files already lowercased. When provided (and its size
 *        matches @p files), it is used in place of lowercasing each entry inline,
 *        letting a caller that runs this same @p files list through many queries
 *        in a row (e.g. the live filter preview, once per keystroke) lowercase
 *        each entry exactly once instead of on every call. Pass nullptr (the
 *        default) to preserve the original per-call lowercasing behavior.
 *        A cache whose size does not match is ignored, never trusted.
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
 * the previous query (e.g. "is" -> "iso" -> "isob"), every entry matching
 * the new query must already have matched the old one, so only the previous
 * match set needs to be re-tested. No strings are copied.
 *
 * The returned indices remain indices into @p files, not positions within
 * @p candidateIndices, and stay in the candidates' order (ascending when
 * @p candidateIndices is ascending, as produced by the filter functions).
 *
 * Correctness precondition: the caller must only pass a candidate set that
 * is a superset of the new query's matches. The live preview guarantees
 * this by using it only when the new query strictly extends the previous
 * one, neither contains ';', and the new query contains no negated term
 * (see @c computeLivePreviewMatches). Queries using '&' qualify: with a
 * single AND-group, extending the text can only lengthen a term or add
 * another AND-ed term, both of which only tighten the match. Negated ('!')
 * terms do NOT qualify: extending a negated term ("!a" -> "!ab") widens the
 * match set, so the previous matches would be an incomplete candidate set.
 *
 * @param files Vector of all entries; candidates index into this.
 * @param candidateIndices Indices into @p files to evaluate (typically the
 *        previous keystroke's matches).
 * @param query Search query to apply (same syntax as @c filterFilesIndices).
 * @param precomputedLower Optional parallel array of lowercased entries,
 *        same size as @p files; ignored if the size does not match.
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
 * This is the COMMIT path, run when the user presses Enter. It is separate
 * from the live preview (see the "Live incremental filter preview" section),
 * which never touches `filteringStack` or `ctx.files`.
 *
 * @param searchString The query to filter by (saved for state recovery).
 *        Unescaped ';' separates OR-ed groups and unescaped '&' separates
 *        AND-ed terms; a leading unescaped '!' on a term negates it.
 *        '\\;', '\\&', '\\!' and '\\\\' match a literal ';', '&', '!'
 *        and '\'.
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return true if matches were found and the filter stack was updated;
 * false if the query is empty or no matches exist. Also returns true, without
 * pushing a new level, when the query matches every entry (the filter would
 * change nothing).
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
//
// Matches used to be computed (and shown) only after the user pressed Enter
// on the FilterTerms prompt. This section adds a live, "type-to-narrow"
// repaint of the REAL on-screen list: GNU Readline calls
// rl_redisplay_function every time it redraws the input line — i.e. after
// essentially every keystroke, including backspaces — so we hook that call
// to recompute matches against the in-progress (uncommitted) query and
// repaint the same printList() the rest of the app uses, right where the
// list is already displayed.
//
// Unpaginated rendering: the live preview is ALWAYS rendered unpaginated.
// Every repaint (including the empty-query frame) temporarily sets
// GlobalState::ITEMS_PER_PAGE = 0 around the printList() call via
// UnpaginatedScope, which makes printList() take its disablePagination path
// (all items, no "Page x/y" header, no PgUp/PgDn footer). The real setting
// is restored immediately afterward, so committed (Enter) results and every
// other screen keep their normal pagination.
//
// Repaint mechanics: each repaint does printList(),
// which wipes and redraws the whole screen, so readline's own idea of "what's
// currently on screen" (used for its normal incremental redraw) is now
// stale. We correct that with rl_forced_update_display(), which — unlike
// the rl_redisplay_function pointer we've hooked — is a real, directly
// callable Readline entry point that unconditionally repaints the prompt
// and in-progress line fresh, ignoring its stale cache. When the query text
// is unchanged since the last frame (e.g. pure cursor movement), we skip the
// repaint and just call the ordinary rl_redisplay() instead.
//
// Purely visual until Enter: filteringStack and ctx.files are never
// permanently modified here — Enter still runs the exact same commit path
// as before (applyFilterCore + saveQueryToHistory), so history and
// nested-filter-stack semantics are unchanged. (The only touch on
// filteringStack is a temporary entry pushed for the duration of one
// printList() call and popped by an RAII guard; see liveFilterRedisplayHook.)
//
// Zero-copy rendering: a frame never copies the matching strings. The
// matches are computed as indices, and printList() receives a
// StringListView — a non-owning view over the preview's source list,
// optionally narrowed by the matched indices. An empty query shows the
// whole source list through the view with no allocation at all.
//
// Search-corpus caching: the text actually searched per source entry (its
// basename, its unmount key, or the raw path) and that text's lowercased
// form depend only on `sourceList` + the useNameOnly/useUnmountKey toggles
// — never on the in-progress query. Both are therefore computed exactly
// once per runFilterLoop() invocation (primeLivePreviewCaches, called
// before the readline() loop starts) rather than being rebuilt from
// scratch on every keystroke.
//
// Incremental narrowing: when the new query strictly extends the previous
// one ("is" -> "iso", or "iso" -> "iso&boot"), every match must already be
// in the previous match set, so only that set is re-tested
// (filterFilesIndicesSubset) instead of the whole list. Backspacing, pasting
// a different query, going back to an empty query, or using ';' (OR-ed,
// multi-group) queries fall back to a full search. '&' (AND) queries are
// fine: with a single AND-group, extending the text can only lengthen a
// term or add another AND-ed term, which only tightens the match. This is
// valid for the smart-case rules too: extending a query can only keep or
// add uppercase letters, and any entry containing the longer text also
// contains the shorter prefix. A previous query that ends in an unpaired
// backslash (see hasDanglingBackslash) is not trusted as a prefix, since
// the next character may turn that backslash into an escape.
//
// Negated ('!') queries ALWAYS take the full-search path. Extending a
// negated term ("!a" -> "!ab") widens the match set (entries containing "a"
// but not "ab" begin to match), so the previous matches are not a valid
// superset of the new ones. Checking the new query alone is sufficient: an
// extension of a query that has an unescaped leading '!' on a term still
// has that '!', so a query without negation can never extend one with it.
//
// Trade-offs worth knowing about:
//  - Index tags: for a non-empty query the preview pushes a temporary
//    filteringStack entry so printList() draws the "[123]^" original-index
//    tags relative to the full list while typing. For an empty query nothing
//    is pushed and printList() uses the real stack, so an already-committed
//    filter's tags show as usual.
//  - A full printList() every keystroke is heavier than
//    a delta redraw and can flicker on slow/high-latency terminals. Since
//    the preview is unpaginated, each frame's cost scales with the whole
//    match set rather than one page. Above LIVE_FILTER_LIMIT source items we
//    skip live repainting entirely and fall back to the old Enter-only
//    behavior, to keep typing responsive on very large lists.
//  - The derived/lowercase caches are primed once per runFilterLoop() call
//    and intentionally never refreshed mid-loop: sourceList only changes on
//    a successful filter commit, which immediately ends the loop, so there
//    is no point in the loop's lifetime where the cache could go stale
//    while still being read from.

namespace {

/**
 * @brief Result of a single live-preview filter pass. Holds indices only;
 * no string is ever copied.
 *
 * @c local indexes into the preview's source list (or its derived label
 * cache — both share the same indexing) and is what the StringListView is
 * built from. @c global holds the same entries translated back to
 * @c globalIsoFileList indices (see @c resolveGlobalIndex) and feeds the
 * temporary filteringStack entry used for the "[N]^" tags. When @c identity
 * is true the preview shows the whole source list unchanged (empty query)
 * and both vectors are left empty.
 */
struct LivePreviewResult {
    std::vector<size_t> local;
    std::vector<size_t> global;
    bool                identity = false;
};

/**
 * @brief Holds all state for one live filter-preview session (i.e. one
 * @c runFilterLoop() invocation): the source list and how to derive its
 * searchable labels, the printList() wiring needed to repaint the real
 * on-screen list, the per-invocation derived/lowercase search caches
 * (see @c primeLivePreviewCaches), the previous query and its matches
 * (for incremental narrowing), and the bookkeeping used to decide whether
 * a given readline() keystroke needs a fresh repaint.
 */
struct LiveFilterPreview {
    const std::vector<std::string>* sourceList     = nullptr;
    bool                             hasLastMatches = false; // lastMatches is valid for lastQuery
    bool                             useNameOnly    = false;
    bool                             useUnmountKey  = false;

    // Set up per runFilterLoop() call; mirror the *actual* current ctx
    // state. (Currently only stored, not read by the preview itself.)
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

    // Incremental-narrowing state, reset at the start of every readline()
    // call. lastMatches holds the previous frame's local match indices
    // (into the searchable list) and is only meaningful while
    // hasLastMatches is true.
    std::string lastQuery;
    std::vector<size_t> lastMatches;

    bool        primed        = false;  // a frame has been drawn for the *current* readline() call
    bool        everRepainted = false;  // a real repaint has happened at least once this runFilterLoop call
    bool        active        = false;  // a readline() call is currently in flight
};

LiveFilterPreview g_livePreview;

/**
 * @brief True once sourceList + the printList() wiring have been set up and
 * are within the size cap, regardless of whether a readline() call is
 * currently in flight. Used to decide whether it is worth priming the
 * derived/lowercase caches before the runFilterLoop while-loop starts.
 *
 * @c GlobalState::LIVE_FILTER_LIMIT == 0 is a deliberate kill-switch: it
 * always disables live filtering, regardless of source list size (including
 * an empty source list, which would otherwise satisfy the size check
 * trivially).
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
 *
 * Note that lowerCache is a full lowercased copy of the searchable list
 * and stays allocated for the whole prompt; it trades memory for not
 * lowercasing every entry on every keystroke.
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
 * @brief Computes the "would-be" match set for the in-progress query,
 * without touching filteringStack — mirrors applyFilterCore's source
 * resolution and name/unmount-key handling, but is purely a preview.
 * Returns indices only; the caller wraps them in a zero-copy StringListView.
 *
 * Reads from the per-invocation derived/lowercase caches
 * (primeLivePreviewCaches) instead of rebuilding them on every keystroke,
 * and narrows incrementally when possible: if @p query strictly extends
 * @c g_livePreview.lastQuery (and neither contains ';', and @p query has no
 * negated term), only the previous frame's matches are re-tested via
 * @c filterFilesIndicesSubset; otherwise the whole list is searched. '&'
 * (AND) queries are eligible for the incremental path (see the "Incremental
 * narrowing" note above); negated ('!') queries are not, because extending
 * a negated term widens the match set. Because it reads the previous query,
 * the caller must update @c lastQuery only AFTER this returns.
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
 * input buffer.
 * @return The matching local indices plus their @c globalIsoFileList
 * translation, or an identity result (whole source list).
 */
LivePreviewResult computeLivePreviewMatches(const std::string& query)
{
    LivePreviewResult result;

    // Parse once; reused below for the identity check and the negation gate.
    bool trailingEmptyGroup = false;
    const std::vector<QueryGroup> groups = buildQueryGroups(query, &trailingEmptyGroup);

    // An empty query, one that parses to no terms (e.g. "\", ";", "&", "!",
    // ";;"), or one whose last OR-branch is still empty ("clon;") matches
    // everything, so show the plain source list (identity), not a
    // "filtered" view with index tags. Not when a negated term exists:
    // negation is global, so its exclusion still applies ("!a;" shows the
    // same list as "!a").
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
    // turn it into an escape and change the meaning of the term.
    // Negation is excluded too: "!a" -> "!ab" WIDENS the match set, so the
    // old matches would not be a superset of the new ones.
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
 * @brief RAII guard that forces printList() into its unpaginated
 * path (GlobalState::ITEMS_PER_PAGE == 0) for one live-preview repaint and
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
 * @brief Installed as rl_redisplay_function for the lifetime of one
 * runFilterLoop() call; invoked by readline on (almost) every keystroke,
 * including backspace, so the real list both narrows and widens live as
 * the query changes.
 *
 * Per invocation it: (1) falls back to plain rl_redisplay() if the preview
 * is not enabled or the query text is unchanged; (2) computes the match
 * set; (3) records the query as the new @c lastQuery (after the compute, so
 * incremental narrowing sees the previous one); (4) clears the screen and
 * repaints the real list through printList() using a zero-copy
 * StringListView, unpaginated; (5) asks readline to redraw its prompt from
 * scratch via rl_forced_update_display().
 *
 * For a non-empty query, a temporary filteringStack entry (holding the
 * matches' global indices, moved in, not copied) is pushed so printList()
 * shows original-list index tags; an RAII guard pops it even if printList()
 * throws. The view, the stack guard and the unpaginated scope all live in
 * an inner block so they are released BEFORE the readline redraw.
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
    // Hold the list only while the user is typing forward onto it (pure
    // append to the last painted query). When arriving at a dangling
    // backslash any other way (backspace, paste, history recall), nothing
    // is pending, so fall through and repaint. A completed pair ("\\") is
    // not dangling and repaints normally.
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
        // Text unchanged (e.g. pure cursor movement) — the terminal still
        // matches what we last painted, so the ordinary incremental
        // redisplay is correct, and cheaper than a full repaint.
        rl_redisplay();
        return;
    }

    // Must run before lastQuery is overwritten: the incremental-narrowing
    // check compares the new query against the previous one.
    LivePreviewResult preview = computeLivePreviewMatches(query);

    g_livePreview.lastQuery = query;

    // unpaginated => everything lives on "page 0"; printList()
    // ignores this value when pagination is disabled.
    size_t previewPage = 0;

    {
        // Zero-copy view over the source list (optionally narrowed by the
        // matched local indices). Nothing is copied; both referenced vectors
        // outlive the printList() call below.
        const std::vector<std::string>& source = *g_livePreview.sourceList;
        const StringListView view = preview.identity
            ? StringListView(source)
            : StringListView(source, preview.local);

        // Temporary index mapping so printList() displays indexes relative
        // to the original ISO list without modifying the real filtering
        // stack. RAII so the stack is restored even if printList() throws.
        // The index vector is moved in, not copied. (Empty query: nothing
        // is pushed.)
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
                  g_livePreview.state,
                  true,
                  false);
    }   // unpaginated restored, then stackGuard pops, before the readline redraw below

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
 * @brief Runs an interactive filter session: shows the Filter prompt,
 * (optionally) live-previews matches as the user types, and commits the
 * filter on Enter.
 *
 * Setup, done once per call: loads filter history, installs the live preview
 * hook (LivePreviewGuard), points the preview at the list this function would
 * search (same source-resolution rule as applyFilterCore), and primes the
 * derived/lowercase search caches. ctx.files is only ever reassigned on a
 * successful filter commit (inside applyFilterCore), and a successful commit
 * immediately breaks out of the loop, so none of that setup can change out
 * from under the loop while it runs.
 *
 * Loop, one readline() per iteration: the per-call incremental-narrowing
 * state (lastQuery / lastMatches / primed) is reset each time. Then:
 *  - Esc as the first character, or EOF (Ctrl+D), cancels: history is
 *    cleared and the loop ends. If the preview repainted the screen,
 *    ctx.needsClrScrn is set so the caller redraws the normal paginated list
 *    instead of leaving the long unpaginated one behind.
 *  - Otherwise the entered text is committed via applyFilterCore. On success
 *    the query is saved to history, @p onSuccess runs and the loop ends; on
 *    failure (empty query or no matches) the prompt is shown again.
 *
 * @param promptText   The prompt text to display.
 * @param ctx          FilterContext containing state.
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
    // whole source list) and, if done per keystroke, wasteful. Done exactly
    // once here, up front. Only worth doing if the live preview will
    // actually run for this session; livePreviewConfigured() mirrors
    // liveMainListEnabled() minus the "readline() call currently in flight"
    // check, which can't be true yet at this point in the function.
    if (livePreviewConfigured())
        primeLivePreviewCaches();

    while (true) {
        // Per-readline()-call reset only. sourceList / useNameOnly /
        // useUnmountKey / derivedCache / lowerCache are deliberately left
        // untouched here — see the priming block above for why they stay
        // valid across every iteration of this loop. The incremental
        // narrowing state, by contrast, belongs to a single readline() call.
        g_livePreview.lastQuery.clear();
        g_livePreview.lastMatches.clear();
        g_livePreview.primed = false;
        g_livePreview.active = true;

        std::unique_ptr<char, decltype(&std::free)> raw(
            readline(promptText.c_str()), &std::free);

        g_livePreview.active = false;

        // If the query got live-repainted onto the real list (any branch
        // below that loops back for another attempt, or that cancels out),
        // the outer caller's own needsClrScrn-driven refresh — set on
        // success by applyFilterCore below, or forced on cancel below —
        // repaints the screen properly afterward.

        //---- Robust handling of FilterTerms prompt ----
        if (!raw || raw.get()[0] == 27) {
            if (!raw) {
                std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
            }
            // The live preview painted an unpaginated list over the
            // screen. Make the caller redo its normal (paginated) refresh so
            // cancelling doesn't leave the long list behind.
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
 * @brief Performs multi-stage filtering on the global ISO file list.
 *
 * Re-applies every level of @ref filteringStack, in order, against the
 * current @p globalIsoFileList, so that the filtered view and each level's
 * stored indices stay consistent after the underlying list changes (e.g. a
 * background refresh). Each level applies its saved query (with the same
 * ';' OR / '&' AND / '!' negation / '\\' escape syntax as the commit path)
 * to the results of the previous level.
 *
 * @section filtering_logic Logic Flow:
 * 1.  **Initialization**: Starts with the full range of indices [0, N) over
 *     @p globalIsoFileList.
 * 2.  **Iterative filtering**: For each @ref FilteringState in the stack:
 *     - Builds the search strings for the surviving entries: basenames when
 *       @ref displayConfig::toggleNamesOnly is set, full paths otherwise.
 *     - Runs @ref filterFilesIndices with the level's saved query.
 *     - Maps the resulting local indices back to global file indices and
 *       stores them in the level's @c originalIndices.
 *     - Uses them as the working set for the next level.
 * 3.  **Break condition**: If any level yields zero matches, the stack is
 *     cleared, @p filteredFiles is emptied and @p isFiltered is set to false.
 * 4.  **Finalization**: Otherwise @p filteredFiles is rebuilt from the final
 *     set of surviving global indices.
 *
 * @note Indices are moved between levels (not copied) to avoid extra
 * allocations. This is the same stack the commit path (applyFilterCore) and
 * printList()'s "[N]^" tags rely on; @ref resolveGlobalIndex depends on every
 * level storing fully-resolved global indices.
 *
 * @param globalIsoFileList The full, current ISO list to filter.
 * @param filteringStack   Filter levels to re-apply; updated in place.
 * @param filteredFiles    Output: the subset of @p globalIsoFileList that
 *                         satisfies all levels (cleared if the stack broke).
 * @param isFiltered       In/out: must be true on entry for any work to be
 *                         done; set to false if the stack broke.
 *
 * @pre `isFiltered` is true and `filteringStack` is not empty (otherwise the
 *      function returns immediately without changes).
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

/**
 * @brief Refreshes source indices for the currently filtered ISO files.
 *
 * Builds a mapping of ISO file paths to their current indices in
 * @p globalIsoFileList, then updates the @c originalIndices of the last
 * filtering state using the existing @p filteredFiles without reapplying
 * any filter queries. If either input list is empty, the filtering stack
 * is empty, or any filtered path cannot be mapped to a source index, the
 * function returns without modifying the filtering stack.
 *
 * This keeps source indices synchronized after the global ISO list is
 * reordered, such as when toggling filenames-only display mode, while
 * preserving the current filtered results.
 *
 * @param globalIsoFileList The current global ISO file list.
 * @param filteringStack    The stack of active filtering states; only the
 *                          last state's source indices are updated.
 * @param filteredFiles     The currently filtered ISO file paths.
 */
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles)
{
    if (filteringStack.empty() || filteredFiles.empty()) {
        return;
    }

    // Map each existing filtered path to its current global source index.
    std::unordered_map<std::string, size_t> sourceIndices;
    sourceIndices.reserve(globalIsoFileList.size());

    for (size_t i = 0; i < globalIsoFileList.size(); ++i) {
        sourceIndices.emplace(globalIsoFileList[i], i);
    }

    // Refresh the indices of the currently displayed filtered results.
    std::vector<size_t> currentIndices;
    currentIndices.reserve(filteredFiles.size());

    for (const auto& path : filteredFiles) {
        const auto it = sourceIndices.find(path);
        if (it == sourceIndices.end()) {
            return; // Don't overwrite the stack with incomplete mappings.
        }

        currentIndices.push_back(it->second);
    }

    // Update only the final filter state's indices.
    filteringStack.back().originalIndices = std::move(currentIndices);
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
 * suppression is lifted. The same suppression is what makes the live
 * preview's temporary ITEMS_PER_PAGE override (UnpaginatedScope) safe.
 *
 * The live main-list preview is opt-in: it is only wired up when @p cfg
 * supplies everything printList() needs (listType, pendingIndices,
 * hasPendingProcess and state); otherwise the prompt behaves as the classic
 * Enter-only filter.
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
 * When @p state is provided it also enables the live main-list preview:
 * the list type is @c "MOUNTED_ISOS" in unmount mode and @c "ISO_FILES"
 * otherwise, and @c pendingIndices / @c hasPendingProcess / the list
 * subtype are taken from the shared @c RefreshState, so no extra parameters
 * are needed.
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
