// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <emmintrin.h>

import std;
import isocmd.core.threadCaps;
import isocmd.filter.filter;
import isocmd.threadpool.threadpool;
import isocmd.util.stringManipulation;

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
 * GCC, providing @c __builtin_ctz. Not buildable on other architectures
 * as written.
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
static bool simdSearchExists(std::string_view text, std::string_view pattern)
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
 * @brief True if @p query ends in an unpaired backslash, i.e. an escape
 * still waiting for the character it escapes. Backslashes pair left to
 * right ("\\\\" is one literal '\'), so only an odd-length trailing run is
 * dangling.
 */
bool hasDanglingBackslash(const std::string& query) {

	size_t run = 0;
	for (size_t i = query.size(); i > 0 && query[i - 1] == '\\'; --i)
	    ++run;
	return (run % 2) == 1;

}

/**
 * @brief Checks if a character is one of the escapable query delimiters or special characters.
 * @param c The character to check.
 * @return True if the character can be escaped by a backslash, false otherwise.
 */
static inline bool isEscapableChar(char c) {

	return c == '&' || c == ';' || c == '!' || c == '\\';

}

/**
 * @brief Finalizes the current search term, constructs its QueryToken with case sensitivity analysis,
 *        and appends it to the active query group if non-empty.
 * @param term The string buffer containing the current term.
 * @param negate Reference to the negation flag for the current term.
 * @param group Reference to the active QueryGroup being built.
 */
static void finishTerm(std::string& term, bool& negate, QueryGroup& group) {

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

}

/**
 * @brief Finalizes the current term and pushes the active query group into the collection if not empty.
 * @param term The string buffer containing the current term.
 * @param negate Reference to the negation flag.
 * @param group Reference to the active QueryGroup.
 * @param groups Reference to the vector of QueryGroups.
 */
static void finishGroup(std::string& term, bool& negate, QueryGroup& group, std::vector<QueryGroup>& groups) {

	finishTerm(term, negate, group);
	if (!group.empty()) {
		groups.push_back(std::move(group));
		group.clear();
	}

}

/**
 * @brief Processes a single character from the query string during parsing.
 * @param query The full query string.
 * @param i Current index in the query string (passed by reference to allow advancing past escaped characters).
 * @param term The string buffer for the current term being accumulated.
 * @param negate Reference to the term negation flag.
 * @param sawOr Reference to the flag indicating if at least one unescaped ';' was encountered.
 * @param group Reference to the active QueryGroup.
 * @param groups Reference to the collection of QueryGroups.
 */
static void processQueryChar(const std::string& query, size_t& i,
                             std::string& term, bool& negate, bool& sawOr,
                             QueryGroup& group, std::vector<QueryGroup>& groups) {

	const char c = query[i];

	// A backslash with nothing after it is a dangling escape (mid-typing,
	// or backspaced onto it). It has no delimiter to escape, so ignore it
	// instead of searching for a literal '\'.
	if (c == '\\' && i + 1 == query.size())
	    return;

	// Backslash escapes only '&', ';', '!' and '\' itself. A backslash
	// before any other character remains part of the search term.
	if (c == '\\' && i + 1 < query.size() && isEscapableChar(query[i + 1])) {
		term.push_back(query[++i]);
	} else if (c == '&') {
		finishTerm(term, negate, group);
	} else if (c == ';') {
		sawOr = true;
		finishGroup(term, negate, group, groups);
	} else if (c == '!' && term.empty() && !negate) {
		// Unescaped '!' at the start of a term: negate it. Any later '!'
		// in the same term (including a second leading one) is literal.
		negate = true;
	} else {
		term.push_back(c);
	}

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
std::vector<QueryGroup> buildQueryGroups(const std::string& query,
                                         bool* trailingEmptyGroup) {

	std::vector<QueryGroup> groups;
	QueryGroup group;
	std::string term;
	bool negate = false;   // the term currently being built started with an unescaped '!'
	bool sawOr  = false;   // at least one unescaped ';' has been seen
	term.reserve(query.size());

	for (size_t i = 0; i < query.size(); ++i) {
		processQueryChar(query, i, term, negate, sawOr, group, groups);
	}

	// The last OR-branch is still empty if nothing has accumulated since the
	// final ';' (checked before finishGroup() flushes it). A pending '!' with
	// no text yet counts as a started term, so "clon;!" is not an empty branch.
	if (trailingEmptyGroup)
	    *trailingEmptyGroup = sawOr && group.empty() && term.empty() && !negate;

	finishGroup(term, negate, group, groups);
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
bool groupsHaveNegation(const std::vector<QueryGroup>& groups) {

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
static bool matchesAnyGroup(const std::vector<QueryGroup>& groups,
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
 * @brief Generates ascending indices `[0, count)`.
 *
 * @param count Number of indices to generate.
 * @return Vector containing values `0` through `count - 1`, or empty if @p count is `0`.
 */
static std::vector<size_t> makeAscendingIndices(size_t count)
{

	std::vector<size_t> indices(count);
	std::iota(indices.begin(), indices.end(), 0);
	return indices;

}

/**
 * @brief Filters a contiguous slice `[start, end)` of entries against @p queryGroups.
 *
 * @param files Vector of candidate entries.
 * @param start Inclusive start index in @p files.
 * @param end Exclusive end index in @p files (clamped to `files.size()`).
 * @param needLower Whether lowercased entry strings are required for matching.
 * @param queryGroups Parsed disjunctive/conjunctive query groups.
 * @param precomputedLower Optional pre-lowercased entries aligned with @p files.
 * @param useCachedLower True when @p precomputedLower is non-null and matches `files.size()`.
 * @return Ascending indices within `[start, end)` of entries matching any query group.
 */
static std::vector<size_t> filterChunkIndices(const std::vector<std::string>& files,
                                              size_t start,
                                              size_t end,
                                              bool needLower,
                                              const std::vector<QueryGroup>& queryGroups,
                                              const std::vector<std::string>* precomputedLower,
                                              bool useCachedLower)
{

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

/**
 * @brief Waits for all chunk futures, concatenates results in order, and rethrows any first error.
 *
 * All futures are drained before rethrowing so worker lifetimes and exceptions are
 * collected cleanly.
 *
 * @param futures Worker futures ordered by ascending chunk index.
 * @param totalFiles Total number of input entries for output capacity reservation.
 * @return Concatenated ascending matching indices across all completed chunks.
 */
static std::vector<size_t> collectChunkResults(std::vector<std::future<std::vector<size_t>>>& futures,
                                               size_t totalFiles)
{

	std::vector<size_t> filteredIndices;
	filteredIndices.reserve(totalFiles);

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
 * @brief Returns the ascending indices of @p files matching @p query, using
 * SIMD substring search on a thread pool.
 *
 * The list is split into contiguous chunks, one per worker (bounded by the
 * pool size, the list size and @c GlobalThreadCaps::FILTER_THREAD_CAP).
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
	if (queryGroups.empty())
	    return makeAscendingIndices(files.size());

	const bool needLower = groupsNeedLower(queryGroups);

	// Only trust a caller-supplied lowercase cache if it lines up with `files`;
	// otherwise fall back to lowercasing inline so a stale/mismatched cache
	// can never produce wrong indices.
	const bool useCachedLower = precomputedLower && precomputedLower->size() == files.size();

	ThreadPool&  pool       = getStaticThreadPool();
	const size_t numThreads = std::min({
		pool.threadCount(),
		files.size(),
		static_cast<size_t>(GlobalThreadCaps::FILTER_THREAD_CAP)
	});

	const size_t chunkSize = (files.size() + numThreads - 1) / numThreads;

	std::vector<std::future<std::vector<size_t>>> futures;
	futures.reserve(numThreads);

	for (size_t i = 0; i < numThreads; ++i) {
		const size_t start = i * chunkSize;
		const size_t end   = std::min(files.size(), start + chunkSize);
		if (start >= end) break;

		futures.emplace_back(pool.enqueue(
		    [&files, start, end, needLower, &queryGroups, precomputedLower, useCachedLower]() {

				return filterChunkIndices(files, start, end, needLower, queryGroups,
				                          precomputedLower, useCachedLower);

			}
		));
	}

	return collectChunkResults(futures, files.size());

}

/**
 * @brief Evaluates a contiguous slice of candidate file indices against the
 *        parsed query groups.
 *
 * @param files Vector of all entries indexed by @p candidateIndices.
 * @param candidateIndices Candidate indices into @p files to evaluate.
 * @param start Inclusive starting index within @p candidateIndices.
 * @param end Exclusive ending index within @p candidateIndices.
 * @param queryGroups Parsed query groups to match against each entry.
 * @param needLower Whether any query group requires a lowercased entry.
 * @param precomputedLower Optional lowercased entries parallel to @p files.
 * @param useCachedLower True when @p precomputedLower is valid for @p files.
 * @return Matching indices into @p files from the `[start, end)` slice in
 *         candidate order.
 */
static std::vector<size_t> filterSubsetChunk(
    const std::vector<std::string>& files,
    const std::vector<size_t>& candidateIndices,
    const size_t start,
    const size_t end,
    const std::vector<QueryGroup>& queryGroups,
    const bool needLower,
    const std::vector<std::string>* precomputedLower,
    const bool useCachedLower)
{

	std::vector<size_t> localMatches;
	localMatches.reserve((end - start) / 4);

	std::string fileLowerScratch;
	if (needLower && !useCachedLower)
	    fileLowerScratch.reserve(256);

	for (size_t candidatePos = start; candidatePos < end; ++candidatePos) {
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

/**
 * @brief Gathers chunk match results from worker futures and rethrows the
 *        first exception encountered, if any.
 *
 * @param futures Futures producing per-chunk matching indices.
 * @param maxMatches Upper bound used to reserve output capacity.
 * @return Combined matching indices in future submission order.
 */
static std::vector<size_t> collectSubsetResults(
    std::vector<std::future<std::vector<size_t>>>& futures,
    const size_t maxMatches)
{

	std::vector<size_t> filteredIndices;
	filteredIndices.reserve(maxMatches);

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
std::vector<size_t> filterFilesIndicesSubset(
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
		static_cast<size_t>(GlobalThreadCaps::FILTER_THREAD_CAP)
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
		     &queryGroups, needLower, precomputedLower,
		     useCachedLower]() -> std::vector<size_t> {

				return filterSubsetChunk(
				    files, candidateIndices, start, end,
				    queryGroups, needLower, precomputedLower,
				    useCachedLower);

			}
		));
	}

	return collectSubsetResults(futures, candidateIndices.size());

}
