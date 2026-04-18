// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"
#include "../filtering.h"
#include "../themes.h"

// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {
    constexpr const char* CLEAR_LINE_ABOVE = "\033[1A\033[K";
    constexpr const char* CLEAR_TWO_LINES  = "\033[2A\033[K";
}

// ─── Global state ────────────────────────────────────────────────────────────

std::vector<FilteringState> filteringStack;

// ─── Boyer-Moore implementation ──────────────────────────────────────────────

/**
 * @brief Represents a single search token with precomputed Boyer-Moore tables
 * 
 * Stores both case-sensitive and case-insensitive versions of the pattern
 * with their corresponding heuristic tables for efficient searching.
 */
struct QueryToken {
    std::string original;
    std::string lower;
    bool        isCaseSensitive;

    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;

    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};

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
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
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
 * @return Vector of indices matching the search criteria
 */
std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query)
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
    
    ThreadPool&  pool       = getStaticThreadPool();
    const size_t numThreads = std::min({
        pool.threadCount(), 
        files.size(), 
        static_cast<size_t>(FILTER_THREAD_CAP)
    });
    
    const size_t chunkSize  = (files.size() + numThreads - 1) / numThreads;

    std::vector<std::future<std::vector<size_t>>> futures;
    futures.reserve(numThreads);

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(files.size(), start + chunkSize);
        if (start >= end) break;

        futures.emplace_back(pool.enqueue(
            [&files, start, end, needLower, queryTokens]() -> std::vector<size_t> {
                std::vector<size_t> localMatches;
                localMatches.reserve((end - start) / 4);

                std::string fileLower;
                if (needLower)
                    fileLower.reserve(256);

                for (size_t j = start; j < end; ++j) {
                    const std::string& file = files[j];

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
 * @brief Core filtering logic applied to a file list
 * 
 * @param searchString The search pattern to apply
 * @param ctx FilterContext containing state and file references
 * @return true if filter was applied successfully, false otherwise
 */
static bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {
    if (searchString.empty()) return false;

    const std::vector<std::string>& sourceList =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

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
    if (tempFiltered.size() == sourceList.size()) return true;

    ctx.currentPage  = 0;
    ctx.needsClrScrn = true;
    ctx.files        = std::move(tempFiltered);

    FilteringState newState;
    newState.originalIndices.reserve(tempIndices.size());

    const bool canTranslate = ctx.isFiltered &&
                              !filteringStack.empty() &&
                              !filteringStack.back().originalIndices.empty();

    for (size_t idx : tempIndices) {
        size_t originalIdx = idx;
        if (canTranslate) {
            const auto& prevIndices = filteringStack.back().originalIndices;
            if (idx < prevIndices.size())
                originalIdx = prevIndices[idx];
        }
        newState.originalIndices.push_back(originalIdx);
    }
    newState.isFiltered = true;

    if (ctx.isFiltered && !filteringStack.empty())
        filteringStack.back() = std::move(newState);
    else
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
 */
static void saveQueryToHistory(const std::string& query, bool& filterHistory) {
    filterHistory = true;
    loadHistory(filterHistory);
    add_history(query.c_str());
    saveHistory(filterHistory);
    clear_history();
}

// ─── Interactive / quick filter driver ───────────────────────────────────────

/**
 * @brief Runs an interactive or quick filter session
 * 
 * @param promptText The prompt text to display
 * @param quickPattern Pre-supplied pattern for quick mode (empty for interactive)
 * @param ctx FilterContext containing state
 * @param onSuccess Callback invoked when filter succeeds
 * @param onEmptyInput Callback invoked when input is empty or cancelled
 */
static void runFilterLoop(const std::string& promptText, const std::string& quickPattern, FilterContext& ctx, const std::function<void()>& onSuccess,
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
        std::cout << AnsiEscape::CLEAR_LINE_ABOVE;

        while (true) {
            clear_history();
            ctx.filterHistory = true;
            loadHistory(ctx.filterHistory);

            std::unique_ptr<char, decltype(&std::free)> raw(
                readline(promptText.c_str()), &std::free);

            if (!raw || raw.get()[0] == '\0' || strcmp(raw.get(), "/") == 0
			|| raw.get()[0] == ';' || (raw.get()[0] == '/' && raw.get()[1] == ';')
			|| std::count(raw.get(), raw.get() + strlen(raw.get()), '/') > 1
			|| strstr(raw.get(), ";;") != nullptr) {
                handleEmpty();
                break;
            }

            std::string query(raw.get());
            if (tryFilter(query)) {
                saveQueryToHistory(query, ctx.filterHistory);
                onSuccess();
                break;
            }

            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
        }
    } else {
        if (tryFilter(quickPattern)) {
            saveQueryToHistory(quickPattern, ctx.filterHistory);
            onSuccess();
        } else {
            handleEmpty();
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 * @details Validates the input string, builds the readline prompt, constructs a
 * @c FilterContext from @p cfg, then delegates to @c runFilterLoop. Returns early
 * without side effects if @p inputString is not a valid filter command.
 *
 * Valid filter commands are:
 * - @c "/"            — opens an interactive filter prompt with no pre-filled term
 * - @c "/term"        — opens the prompt with @c "term" as the initial pattern
 *
 * The following inputs are explicitly rejected and cause an early @c false return:
 * - Empty string or any string not starting with @c '/'
 * - Strings starting with @c ';' or @c "/;"
 * - Strings containing more than one @c '/' character
 * - Strings containing @c ";;"
 *
 * @param inputString  Raw input from the user, expected to start with @c '/'.
 * @param cfg          Configuration struct with all state pointers and display options.
 *                     All non-optional pointer fields must be non-null.
 * @return @c true if @p inputString was recognised as a filter command and handled,
 *         @c false if it was not a filter command and should be processed elsewhere.
 */
bool runSharedFilterFlow(const std::string& inputString, const FilterCallConfig& cfg)
{
    if (inputString != "/" && (inputString.empty() || inputString[0] != '/'))
        return false;
    if (inputString[0] == ';' ||
       (inputString[0] == '/' && inputString.size() > 1 && inputString[1] == ';') ||
        std::count(inputString.begin(), inputString.end(), '/') > 1 ||
        inputString.find(";;") != std::string::npos)
        return false;

    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };

    const ReadlineAndPromptTheme ft = getFilterTheme("", false);
    const std::string prompt =
        ft.filter  + "FilterTerms" +
        ft.primary + " ↵ for " +
        wrap(cfg.operationColor) + cfg.operation +
        ft.primary + ", or ↵ to return: " +
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

    auto onEmptyInput = [&]() {
        clear_history();
        *cfg.needsClrScrn = *cfg.isFiltered;
    };

    auto onSort = [&]() {
        if (cfg.need2Sort) *cfg.need2Sort = true;
    };

    const std::string quickPattern =
        (inputString == "/") ? "" : inputString.substr(1);

    runFilterLoop(prompt, quickPattern, ctx, onSort, onEmptyInput);
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
    const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage)
{
    const std::vector<std::string>& baseSource =
        isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);

    return runSharedFilterFlow(inputString, {
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
    });
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
    bool& filterHistory, bool& need2Sort, size_t& currentPage)
{
    runSharedFilterFlow(inputString, {
        .files          = &files,
        .operation      = operation,
        .operationColor = originalColors::orange,
        .isFiltered     = &isFiltered,
        .needsClrScrn   = &needsClrScrn,
        .filterHistory  = &filterHistory,
        .need2Sort      = &need2Sort,
        .currentPage    = &currentPage
    });
}
