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
 * @brief Context structure for filter operations
 */
struct FilterContext {
    std::vector<std::string>& files;
    bool&                      isFiltered;
    bool&                      needsClrScrn;
    bool&                      filterHistory;
    size_t&                    currentPage;

    const std::vector<std::string>* sourceOverride = nullptr;

    bool isUnmount            = false;
    bool toggleFullListUmount = false;
};

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

            if (!raw || raw.get()[0] == '\0' || strcmp(raw.get(), "/") == 0 || strcmp(raw.get(), ";") == 0 
			|| raw.get()[0] == ';' || std::count(raw.get(), raw.get() + strlen(raw.get()), '/') > 1) {
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
 * @brief Handles filtering for ISO file operations
 * 
 * @param inputString The input string (starting with '/' for filter mode)
 * @param filteredFiles Vector of files to filter
 * @param isFiltered Flag indicating if filtering is active
 * @param needsClrScrn Flag indicating if screen needs clearing
 * @param filterHistory Flag for history saving
 * @param operation Name of the operation being performed
 * @param operationColor ANSI color code for operation text
 * @param isoDirs ISO directories to search
 * @param isUnmount Flag for unmount mode
 * @param currentPage Reference to current page number
 * @return true if input was handled as a filter command
 */
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, const std::string& operation,
const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage)
{
    if (inputString != "/" && (inputString.empty() || inputString[0] != '/'))
        return false;
        
    if (inputString[0] == '/' && (inputString[1] == ';' || std::count(inputString.begin(), inputString.end(), '/') > 1))
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

    // Helper to wrap raw ANSI strings for readline
	auto wrap = [](std::string_view s) -> std::string {
		return "\001" + std::string(s) + "\002";
	};

	const ListTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");

	// Use pre-wrapped originalColors or wrap the raw theme members
	std::string colorPrimary = isOriginal ? std::string(originalColors::rl_blue) : wrap(theme->muted);
	std::string colorFilter  = isOriginal ? std::string(originalColors::rl_cyan) : wrap(theme->accent);
	std::string colorReset   = isOriginal ? std::string(originalColors::rl_reset) : wrap(originalColors::boldAlt);

	// Assuming operationColor comes from the raw theme, it needs wrapping
	std::string safeOpColor = wrap(operationColor);

	const std::string prompt =
		colorFilter  + "FilterTerms" +
		colorPrimary + " ↵ for " +
		safeOpColor  + operation +
		colorPrimary + ", or ↵ to return: " +
		colorReset;

    auto onEmptyInput = [&]() {
        clear_history();
        needsClrScrn = isFiltered;
    };

    const std::string quickPattern =
        (inputString == "/") ? "" : inputString.substr(1);

    runFilterLoop(prompt, quickPattern, ctx, []{}, onEmptyInput);
    return true;
}

/**
 * @brief Handles filtering for conversion to ISO operations
 * 
 * @param mainInputString The input string (starting with '/' for filter mode)
 * @param files Vector of files to filter
 * @param fileExtensionWithOutDots File extension for display
 * @param isFiltered Flag indicating if filtering is active
 * @param needsClrScrn Flag indicating if screen needs clearing
 * @param filterHistory Flag for history saving
 * @param need2Sort Flag indicating if resorting is needed
 * @param currentPage Reference to current page number
 */
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, bool& isFiltered, bool& needsClrScrn,
bool& filterHistory, bool& need2Sort, size_t& currentPage)
{
    if (mainInputString.empty() ||
        (mainInputString != "/" && mainInputString[0] != '/'))
        return;
    if (mainInputString[0] == '/' && (mainInputString[1] == ';' || std::count(mainInputString.begin(), mainInputString.end(), '/') > 1))
		return;

    FilterContext ctx {
        files,
        isFiltered,
        needsClrScrn,
        filterHistory,
        currentPage
    };

    // Helper to wrap raw ANSI strings for readline
	auto wrap = [](std::string_view s) -> std::string {
		return "\001" + std::string(s) + "\002";
	};

	const bool isInteractive = (mainInputString == "/");
	const std::string quickPat = isInteractive ? "" : mainInputString.substr(1);

	const ListTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");

	// Wrap themed colors, but keep originalColors::rl_ variants as-is
	std::string colorMuted  = isOriginal ? std::string(originalColors::rl_blue)   : wrap(theme->muted);
	std::string colorExt    = isOriginal ? std::string(originalColors::rl_orange) : wrap(theme->highlight);
	std::string colorFilter = isOriginal ? std::string(originalColors::rl_cyan)   : wrap(theme->accent);
	std::string colorReset  = isOriginal ? std::string(originalColors::rl_reset)  : wrap(originalColors::boldAlt);

	const std::string prompt = 
		colorFilter + "FilterTerms" + 
		colorMuted  + " ↵ for " + 
		colorExt    + fileExtensionWithOutDots + 
		colorMuted  + ", or ↵ to return: " + 
		colorReset;

		runFilterLoop(prompt, quickPat, ctx, [&] { need2Sort = true; });
	}
