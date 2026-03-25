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

// One-time atexit registration — avoids re-entering getThreadPool() from
// inside the atexit handler (which would be UB if the static is mid-destruct).
static void registerPoolShutdown() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        // Store pointer in a static so the atexit handler (which must be a
        // plain function pointer with no captures) can reach it without
        // re-entering getThreadPool() during destruction.
        static ThreadPool* poolPtr = &getThreadPool();
        std::atexit([]() { poolPtr->waitAndStop(); });
    });
}


// Returns the number of worker threads in the shared pool.
static size_t poolThreadCount() {
    return getThreadPool().threadCount();
}


// ─── Boyer-Moore implementation ──────────────────────────────────────────────

struct QueryToken {
    std::string original;
    std::string lower;
    bool        isCaseSensitive;

    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;

    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};


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

std::vector<size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query)
{
    if (files.empty() || query.empty()) return {};

    // Register pool shutdown once here (safe, idempotent via once_flag)
    registerPoolShutdown();

    // queryTokens is captured by VALUE in the lambda below to avoid
    // dangling references if the calling frame is torn down before all
    // futures are collected (e.g. on exception paths).
    const std::vector<QueryToken> queryTokens = buildQueryTokens(query);
    if (queryTokens.empty()) {
        std::vector<size_t> allIndices(files.size());
        std::iota(allIndices.begin(), allIndices.end(), 0);
        return allIndices;
    }

    const bool needLower = std::any_of(queryTokens.begin(), queryTokens.end(),
                               [](const QueryToken& qt) { return !qt.isCaseSensitive; });

    ThreadPool&  pool       = getThreadPool();
    const size_t numThreads = std::min(poolThreadCount(), files.size());
    const size_t chunkSize  = (files.size() + numThreads - 1) / numThreads;

    std::vector<std::future<std::vector<size_t>>> futures;
    futures.reserve(numThreads);

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(files.size(), start + chunkSize);
        if (start >= end) break;

        // Capture queryTokens and needLower by value; capture files by
        // const-ref is safe because filterFilesIndices blocks until all
        // futures are .get()'d before returning.
        futures.emplace_back(pool.enqueue(
            [&files, start, end, needLower, queryTokens /* by value */]() -> std::vector<size_t> {
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

    // Collect ALL futures before returning, even if one throws, to
    // prevent the pool workers from accessing freed stack memory.
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
            // Save first exception; drain remaining futures so workers
            // are not left dangling, then rethrow below.
            if (!firstException)
                firstException = std::current_exception();
        }
    }

    if (firstException)
        std::rethrow_exception(firstException);

    return filteredIndices;
}


// ─── Shared filtering core ───────────────────────────────────────────────────

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

    // Build the new FilteringState, translating indices through any prior filter.
    //
    // tempIndices are offsets into sourceList (the current filtered view).
    // If a filter is already active, filteringStack.back().originalIndices
    // maps positions in that view back to the master list.  Guard with a
    // bounds check: without it a stale/mismatched stack causes SIGSEGV on
    // nested filtering.
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
            // Out-of-range means the stack is out of sync — keep idx as-is.
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

static void saveQueryToHistory(const std::string& query, bool& filterHistory) {
    filterHistory = true;
    loadHistory(filterHistory);
    add_history(query.c_str());
    saveHistory(filterHistory);
    clear_history();
}


// ─── Interactive / quick filter driver ───────────────────────────────────────

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

            // Treat EOF (null), empty string, and bare "/" as "no input"
            // using a single consistent check, matching quick-mode behaviour.
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

            std::cout << AnsiEscape::CLEAR_LINE_ABOVE;
        }
    } else {
        if (tryFilter(quickPattern)) {
            saveQueryToHistory(quickPattern, ctx.filterHistory);
            onSuccess();
        } else {
            // Call handleEmpty on no-result in quick mode so the caller's
            // state (needsClrScrn etc.) is updated consistently whether the
            // user typed /pattern or used interactive mode.
            handleEmpty();
        }
    }
}


// ─── Public API ──────────────────────────────────────────────────────────────

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

    auto onEmptyInput = [&]() {
        clear_history();
        needsClrScrn = isFiltered;
    };

    const std::string quickPattern =
        (inputString == "/") ? "" : inputString.substr(1);

    runFilterLoop(prompt, quickPattern, ctx, []{ /* no extra side-effects */ }, onEmptyInput);
    return true;
}


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
    };

    const bool isInteractive   = (mainInputString == "/");
    const std::string quickPat = isInteractive ? "" : mainInputString.substr(1);

    const std::string prompt =
        "\001\033[1;96m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" +
        fileExtensionWithOutDots +
        "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";

    runFilterLoop(prompt, quickPat, ctx, [&] { need2Sort = true; });
}
