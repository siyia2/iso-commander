// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"
#include "../filtering.h"


// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {
    constexpr const char* CLEAR_LINE_ABOVE = "\033[1A\033[K";
    constexpr const char* CLEAR_TWO_LINES  = "\033[2A\033[K";
}


// ─── Global state ────────────────────────────────────────────────────────────

// Filtering stack: tracks successive filter states for potential undo
std::vector<FilteringState> filteringStack;

// Long-lived thread pool — created once, reused across all filterFiles() calls.
// Capped at 4 threads (or hardware max if fewer than 4 cores available).
static ThreadPool& getThreadPool() {
    static ThreadPool pool([] {
        const unsigned hw = maxThreads;
        return (hw >= 4) ? 4u : std::max(1u, hw);
    }());
    return pool;
}

// Returns the number of worker threads in the shared pool.
// filterFiles() uses this so chunk count always matches actual worker count.
static size_t poolThreadCount() {
    return getThreadPool().threadCount();
}


// ─── Boyer-Moore implementation ──────────────────────────────────────────────

// Structure to hold precomputed Boyer-Moore tables and token metadata
struct QueryToken {
    std::string original;           // Original token (used for case-sensitive search)
    std::string lower;              // Lowercased version (used for case-insensitive search)
    bool        isCaseSensitive;    // True if token contains any uppercase character

    // Precomputed BM tables for case-sensitive search
    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;

    // Precomputed BM tables for case-insensitive search
    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};


// Precompute Boyer-Moore bad-character and good-suffix tables for a pattern
void precomputeBoyerMooreTables(const std::string& pattern, std::vector<int>& badCharTable, std::vector<int>& goodSuffixTable)
{
    const size_t m             = pattern.size();
    const int    ALPHABET_SIZE = 256;

    // Bad character table: last occurrence of each character in the pattern
    badCharTable.assign(ALPHABET_SIZE, -1);
    for (int i = 0; i < static_cast<int>(m); ++i)
        badCharTable[static_cast<unsigned char>(pattern[i])] = i;

    // Good suffix table
    goodSuffixTable.resize(m, static_cast<int>(m));
    std::vector<int> suffix(m, 0);

    // Phase 1: compute suffix lengths
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

    // Phase 2a: suffix matches a prefix of the pattern
    for (int i = 0; i < static_cast<int>(m) - 1; ++i)
        goodSuffixTable[i] = static_cast<int>(m) - 1 - suffix[0];

    // Phase 2b: substring of pattern matches a suffix of the pattern
    for (int i = 0; i <= static_cast<int>(m) - 2; ++i) {
        const int j = static_cast<int>(m) - 1 - suffix[i];
        if (goodSuffixTable[j] > static_cast<int>(m) - 1 - i)
            goodSuffixTable[j] = static_cast<int>(m) - 1 - i;
    }
}


// Returns true as soon as the first occurrence of pattern is found in text
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

// Parse a semicolon-separated query string into QueryToken objects with
// precomputed Boyer-Moore tables.
static std::vector<QueryToken> buildQueryTokens(const std::string& query) {
    std::vector<QueryToken> tokens;
    std::stringstream ss(query);
    std::string token;

    while (std::getline(ss, token, ';')) {
        // Trim leading/trailing whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;

        QueryToken qt;
        qt.original        = token;
        qt.isCaseSensitive = std::any_of(token.begin(), token.end(),
                                 [](unsigned char c) { return std::isupper(c); });

        // Always precompute case-sensitive tables
        precomputeBoyerMooreTables(qt.original, qt.originalBadChar, qt.originalGoodSuffix);

        // Precompute case-insensitive tables only when needed
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

// Filter a list of strings in parallel using Boyer-Moore search.
// Returns matching strings preserving their original relative order.
//
// Thread count is derived from the shared pool (via poolThreadCount()) so that
// the number of chunks always matches the number of available workers — avoiding
// the bug where hardware_concurrency() > pool size produced more chunks than
// workers and left half the futures queued behind the others.
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query)
{
    if (files.empty() || query.empty()) return {};

    const std::vector<QueryToken> queryTokens = buildQueryTokens(query);
    if (queryTokens.empty()) return files;

    const bool needLower = std::any_of(queryTokens.begin(), queryTokens.end(),
                               [](const QueryToken& qt) { return !qt.isCaseSensitive; });

    // Use the pool's actual thread count — not hardware_concurrency() — so chunk
    // count and worker count are always in sync.
    ThreadPool&  pool       = getThreadPool();
    const size_t numThreads = std::min(poolThreadCount(), files.size());
    const size_t chunkSize  = (files.size() + numThreads - 1) / numThreads;

    std::vector<std::future<std::vector<std::string>>> futures;
    futures.reserve(numThreads);

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(files.size(), start + chunkSize);
        if (start >= end) break;

        futures.emplace_back(pool.enqueue([&files, start, end, needLower, queryTokens] {
            std::vector<std::string> localMatches;
            for (size_t j = start; j < end; ++j) {
                const std::string& file = files[j];

                std::string fileLower;
                if (needLower) {
                    fileLower = file;
                    toLowerInPlace(fileLower);
                }

                for (const auto& qt : queryTokens) {
                    bool match;
                    if (qt.isCaseSensitive) {
                        match = boyerMooreSearchExists(file,      qt.original,
                                                       qt.originalBadChar, qt.originalGoodSuffix);
                    } else {
                        match = boyerMooreSearchExists(fileLower, qt.lower,
                                                       qt.lowerBadChar,    qt.lowerGoodSuffix);
                    }
                    if (match) {
                        localMatches.push_back(file);
                        break;  // Any token matching is sufficient
                    }
                }
            }
            return localMatches;
        }));
    }

    std::vector<std::string> filteredFiles;
    filteredFiles.reserve(files.size());
    for (auto& fut : futures) {
        auto chunk = fut.get();
        filteredFiles.insert(filteredFiles.end(),
                             std::make_move_iterator(chunk.begin()),
                             std::make_move_iterator(chunk.end()));
    }
    return filteredFiles;
}


// ─── Shared filtering core ───────────────────────────────────────────────────

// Parameters bundled to avoid long argument lists across the two public helpers
struct FilterContext {
    std::vector<std::string>& files;        // In/out: list being filtered
    bool&                      isFiltered;   // In/out: whether a filter is active
    bool&                      needsClrScrn; // Out: whether the screen needs clearing
    bool&                      filterHistory;
    size_t&                    currentPage;

    // Source list to filter from (may differ from `files` when unmounting)
    const std::vector<std::string>* sourceOverride = nullptr;

    // Unmount-specific: whether to strip the trailing ~mountpoint suffix
    bool isUnmount            = false;
    bool toggleFullListUmount = false;
};


// Applies a single filter pass. Returns true if the call was handled
// (even if no results were found), false only if searchString is empty.
//
// On a successful filter the following side-effects occur:
//   - ctx.files       is replaced with the filtered subset
//   - filteringStack  is updated (push on first filter, replace on refinement)
//   - ctx.isFiltered  is set to true
//   - ctx.currentPage is reset to 0
static bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {
    if (searchString.empty()) return false;

    // Choose the correct source list
    const std::vector<std::string>& sourceList =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

    // ── Build the list to actually search ────────────────────────────────────
    // In names-only or unmount-abbreviated mode we search a derived list and
    // then map matches back to the original paths.  Otherwise we search paths
    // directly and can skip the mapping step entirely.

    auto extractUnmountKey = [](const std::string& path) -> std::string {
        size_t lastSlash = path.find_last_of('/');
        std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
        size_t lastTilde = name.find_last_of('~');
        return (lastTilde != std::string::npos) ? name.substr(0, lastTilde) : name;
    };

    std::vector<std::string> tempFiltered;
    std::vector<size_t>      tempIndices;

    const bool useNameOnly   = displayConfig::toggleNamesOnly && !ctx.isUnmount;
    const bool useUnmountKey = ctx.isUnmount && !ctx.toggleFullListUmount;

    if (useNameOnly || useUnmountKey) {
        // Build a derived list for searching
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

        auto matched = filterFiles(derived, searchString);
        if (matched.empty()) return false;

        // Map matched derived keys back to original paths.
        // Build a multimap to handle the (rare) case of duplicate derived keys.
        std::unordered_multimap<std::string, size_t> keyToIdx;
        keyToIdx.reserve(derived.size());
        for (size_t i = 0; i < derived.size(); ++i)
            keyToIdx.emplace(derived[i], i);

        std::unordered_set<std::string> matchedSet(matched.begin(), matched.end());
        for (size_t i = 0; i < derived.size(); ++i) {
            if (matchedSet.count(derived[i])) {
                tempFiltered.push_back(sourceList[i]);
                tempIndices.push_back(i);
            }
        }
    } else {
        // Search paths directly; recover indices in a single pass
        tempFiltered = filterFiles(sourceList, searchString);
        if (tempFiltered.empty()) return false;

        std::unordered_set<std::string> matchedSet(tempFiltered.begin(), tempFiltered.end());
        for (size_t i = 0; i < sourceList.size(); ++i) {
            if (matchedSet.count(sourceList[i]))
                tempIndices.push_back(i);
        }
    }

    if (tempFiltered.empty())                     return false;
    if (tempFiltered.size() == sourceList.size()) return true;  // No change

    // ── Commit the filter result ──────────────────────────────────────────────
    ctx.currentPage  = 0;
    ctx.needsClrScrn = true;
    ctx.files        = std::move(tempFiltered);

    // Build the new FilteringState, translating indices through any prior filter
    FilteringState newState;
    newState.originalIndices.reserve(tempIndices.size());
    for (size_t idx : tempIndices) {
        size_t originalIdx = (ctx.isFiltered && !filteringStack.empty())
                             ? filteringStack.back().originalIndices[idx]
                             : idx;
        newState.originalIndices.push_back(originalIdx);
    }
    newState.isFiltered = true;

    if (ctx.isFiltered && !filteringStack.empty())
        filteringStack.back() = std::move(newState);   // Refine existing filter
    else
        filteringStack.push_back(std::move(newState)); // First filter — push new level

    ctx.isFiltered = true;
    return true;
}


// ─── History helpers ─────────────────────────────────────────────────────────

static void saveQueryToHistory(const std::string& query, bool& filterHistory) {
    filterHistory = true;
    loadHistory(filterHistory);
    add_history(query.c_str());
    saveHistory(filterHistory);
    clear_history();
}


// ─── Interactive / quick filter driver ───────────────────────────────────────

// Shared interactive loop used by both public handlers.
//
// promptText   – the readline prompt string
// quickPattern – non-empty → skip readline and filter immediately (quick mode)
// ctx          – mutable filter context
// onSuccess    – called once when a filter is successfully applied
//                (used for caller-specific side-effects, e.g. need2Sort)
// onEmptyInput – called when the user submits an empty query or EOF; replaces
//                the default "clear two lines + needsClrScrn=false" behaviour.
//                If nullptr the default behaviour is used.
static void runFilterLoop(const std::string&           promptText,
                          const std::string&           quickPattern,
                          FilterContext&                ctx,
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

    if (quickPattern.empty()) {
        // ── Interactive mode ──────────────────────────────────────────────────
        std::cout << AnsiEscape::CLEAR_LINE_ABOVE;

        while (true) {
            clear_history();
            ctx.filterHistory = true;
            loadHistory(ctx.filterHistory);

            std::unique_ptr<char, decltype(&std::free)> raw(
                readline(promptText.c_str()), &std::free);

            if (!raw || raw.get()[0] == '\0' || strcmp(raw.get(), "/") == 0) {
                handleEmpty();
                break;
            }

            std::string query(raw.get());
            if (tryFilter(query)) {
                saveQueryToHistory(query, ctx.filterHistory);
                onSuccess();
                break;
            }

            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;  // Bad query — reprompt
        }
    } else {
        // ── Quick mode (/pattern) ─────────────────────────────────────────────
        if (tryFilter(quickPattern)) {
            saveQueryToHistory(quickPattern, ctx.filterHistory);
            onSuccess();
        } else {
            handleEmpty();
        }
    }
}


// ─── Public API ──────────────────────────────────────────────────────────────

// Handles filtering for selectForIsoFiles (ISO list and unmount views)
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, const std::string& operation,
const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage)
{
    if (inputString != "/" && (inputString.empty() || inputString[0] != '/'))
        return false;

    const std::vector<std::string>& baseSource =
        isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);

    FilterContext ctx {
        filteredFiles,
        isFiltered,
        needsClrScrn,
        filterHistory,
        currentPage,
        &baseSource,
        isUnmount,
        displayConfig::toggleFullListUmount
    };

    const std::string prompt =
        "\001\033[1;96m\002FilterTerms\001\033[1;94m\002 ↵ for \001" +
        operationColor + "\002" + operation +
        "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";

    // handleFilteringForISO has a special empty-input exit: it sets
    // needsClrScrn = isFiltered (to redraw only when a filter is already
    // active) rather than unconditionally clearing the screen.
    auto onEmptyInput = [&]() {
        clear_history();
        needsClrScrn = isFiltered;
    };

    const std::string quickPattern =
        (inputString == "/") ? "" : inputString.substr(1);

    runFilterLoop(prompt, quickPattern, ctx, []{ /* no extra side-effects */ }, onEmptyInput);
    return true;
}


// Handles filtering for select_and_convert_to_iso
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, bool& isFiltered, bool& needsClrScrn,
bool& filterHistory, bool& need2Sort, size_t& currentPage)
{
    if (mainInputString.empty() ||
        (mainInputString != "/" && mainInputString[0] != '/'))
        return;

    FilterContext ctx {
        files,
        isFiltered,
        needsClrScrn,
        filterHistory,
        currentPage
        // sourceOverride = nullptr → applyFilterCore uses ctx.files directly
    };

    const bool isInteractive   = (mainInputString == "/");
    const std::string quickPat = isInteractive ? "" : mainInputString.substr(1);

    const std::string prompt =
        "\001\033[1;96m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" +
        fileExtensionWithOutDots +
        "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";

    runFilterLoop(prompt, quickPat, ctx, [&] { need2Sort = true; });
}
