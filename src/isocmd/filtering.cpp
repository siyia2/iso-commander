// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"
#include "../filtering.h"


// Global vector to track if filtering stack has items
std::vector<FilteringState> filteringStack;


// Function to hanlde filtering for selectForIsoFiles

bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, const std::string& operation, const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage) {
    // Early exit if not a filtering operation
    if (inputString != "/" && (inputString.empty() || inputString[0] != '/')) {
        return false;
    }

    bool isFilterPrompt = (inputString == "/");

    // --- Helper Lambdas ---
    auto extractFilename = [](const std::string& path) -> std::string {
        size_t pos = path.find_last_of('/');
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    };

    auto tokenizeSearchString = [](const std::string& searchString) -> std::vector<std::pair<std::string, bool>> {
        std::vector<std::pair<std::string, bool>> tokens;
        std::istringstream tokenStream(searchString);
        std::string token;
        while (std::getline(tokenStream, token, ';')) {
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty()) {
                bool hasUpper = std::any_of(token.begin(), token.end(),
                    [](unsigned char c) { return std::isupper(c); });
                tokens.emplace_back(token, hasUpper);
            }
        }
        return tokens;
    };

    auto applyFilter = [&](const std::string& searchString) -> bool {
        if (searchString.empty()) return false;

        const auto& sourceList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
        std::vector<std::string> tempFiltered;
        std::vector<size_t> tempIndices;

        auto tokens = tokenizeSearchString(searchString);
        if (tokens.empty()) {
            tempFiltered = sourceList;
            return true;
        }

        if (displayConfig::toggleNamesOnly) {
            // Name-only multi-term filtering
            tempFiltered.reserve(sourceList.size());
            tempIndices.reserve(sourceList.size());

            for (size_t i = 0; i < sourceList.size(); ++i) {
                const auto& entry = sourceList[i];
                std::string name = extractFilename(entry);

                if (isUnmount) {
                    auto tildePos = name.find('~');
                    if (tildePos != std::string::npos) {
                        name = name.substr(0, tildePos);
                    }
                }

                // Change to match behavior in filterFiles - match ANY token (not ALL)
                bool matchFound = false;
                for (const auto& [token, isCaseSensitive] : tokens) {
                    bool tokenMatches = false;
                    if (isCaseSensitive) {
                        if (name.find(token) != std::string::npos) {
                            tokenMatches = true;
                        }
                    } else {
                        std::string nameLower = name;
                        toLowerInPlace(nameLower);
                        std::string tokenLower = token;
                        toLowerInPlace(tokenLower);
                        if (nameLower.find(tokenLower) != std::string::npos) {
                            tokenMatches = true;
                        }
                    }
                    
                    if (tokenMatches) {
                        matchFound = true;
                        break; // Match found for this token, no need to check others
                    }
                }

                if (matchFound) {
                    tempFiltered.push_back(entry);
                    tempIndices.push_back(i);
                }
            }
        } else {
            // Use filterFiles for standard filtering
            tempFiltered = filterFiles(sourceList, searchString);
            
            // Rebuild indices for filtered results
            std::unordered_set<std::string> filteredSet(tempFiltered.begin(), tempFiltered.end());
            for (size_t i = 0; i < sourceList.size(); ++i) {
                if (filteredSet.find(sourceList[i]) != filteredSet.end()) {
                    tempIndices.push_back(i);
                }
            }
        }

        if (tempFiltered.empty()) return false;
        if (tempFiltered.size() == sourceList.size()) return true;

        currentPage = 0;
        needsClrScrn = true;
        filteredFiles = std::move(tempFiltered);

        FilteringState newState;
        newState.originalIndices.reserve(tempIndices.size());
        for (size_t idx : tempIndices) {
            size_t originalIdx = isFiltered ? filteringStack.back().originalIndices[idx] : idx;
            newState.originalIndices.push_back(originalIdx);
        }
        newState.isFiltered = true;

        if (isFiltered) {
            filteringStack.back() = std::move(newState);
        } else {
            filteringStack.push_back(std::move(newState));
        }

        isFiltered = true;
        return true;
    };

    // --- Interactive Filter Mode ---
    if (isFilterPrompt) {
        while (true) {
            filterHistory = true;
            loadHistory(filterHistory);
            std::cout << "\033[1A\033[K"; // Clear previous line

            std::string filterPrompt =
                "\001\033[1;38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" +
                operationColor + "\002" + operation +
                "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";

            std::unique_ptr<char, decltype(&std::free)> searchQuery(
                readline(filterPrompt.c_str()), &std::free);

            // Exit conditions
            if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                clear_history();
                needsClrScrn = isFiltered;
                return true;
            }

            std::string searchString = searchQuery.get();
            if (applyFilter(searchString)) {
                add_history(searchQuery.get());
                saveHistory(filterHistory);
                clear_history();
                return true;
            }
        }
    }
    // --- Quick Filter Mode (/pattern) ---
    else {
        std::string searchString = inputString.substr(1);
        if (applyFilter(searchString)) {
            filterHistory = true;
            loadHistory(filterHistory);
            add_history(searchString.c_str());
            saveHistory(filterHistory);
            clear_history();
        }
    }

    return true;
}


// Handle filtering for select_and_convert_to_iso
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort, size_t& currentPage) {
    // Exit early if not a filtering command
    if (mainInputString.empty() || (mainInputString != "/" && mainInputString[0] != '/')) {
        return;
    }

    // Helper to extract filename from path
    auto getBasename = [](const std::string& path) {
        auto pos = path.find_last_of("/\\");
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    };
    
    // Helper to tokenize search string using semicolon delimiter
    auto tokenizeSearchString = [](const std::string& searchString) -> std::vector<std::pair<std::string, bool>> {
        std::vector<std::pair<std::string, bool>> tokens;
        std::istringstream tokenStream(searchString);
        std::string token;
        while (std::getline(tokenStream, token, ';')) {
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty()) {
                bool hasUpper = std::any_of(token.begin(), token.end(),
                    [](unsigned char c) { return std::isupper(c); });
                tokens.emplace_back(token, hasUpper);
            }
        }
        return tokens;
    };

    // Function to apply filtering based on search string
    auto applyFilter = [&](const std::string& searchString) -> bool {
        if (searchString.empty()) {
            return false;
        }

        std::vector<std::string> tempFiltered;
        std::vector<size_t> tempIndices;
        tempFiltered.reserve(files.size());
        tempIndices.reserve(files.size());

        // Parse the search string into tokens using semicolon delimiter
        auto tokens = tokenizeSearchString(searchString);
        if (tokens.empty()) {
            return false;
        }

        // Single-pass filtering with index tracking
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            bool matchFound = false;

            if (displayConfig::toggleNamesOnly) {
                std::string filename = getBasename(file);
                std::string filenameLower;
                bool needLowercase = false;

                // Check if we need a lowercase version of the filename
                for (const auto& [token, hasUpper] : tokens) {
                    if (!hasUpper) {
                        needLowercase = true;
                        break;
                    }
                }

                if (needLowercase) {
                    filenameLower = filename;
                    toLowerInPlace(filenameLower);
                }

                // Check if ANY token matches (consistent with filterFiles behavior)
                for (const auto& [token, hasUpper] : tokens) {
                    if (hasUpper) {
                        // Case-sensitive search
                        if (filename.find(token) != std::string::npos) {
                            matchFound = true;
                            break;
                        }
                    } else {
                        // Case-insensitive search
                        std::string tokenLower = token;
                        toLowerInPlace(tokenLower);
                        if (filenameLower.find(tokenLower) != std::string::npos) {
                            matchFound = true;
                            break;
                        }
                    }
                }
            } else {
                // Non-name-only filtering with tokens
                std::string fileLower;
                bool needLowercase = false;

                // Check if we need a lowercase version of the file
                for (const auto& [token, hasUpper] : tokens) {
                    if (!hasUpper) {
                        needLowercase = true;
                        break;
                    }
                }

                if (needLowercase) {
                    fileLower = file;
                    toLowerInPlace(fileLower);
                }

                // Check if ANY token matches
                for (const auto& [token, hasUpper] : tokens) {
                    if (hasUpper) {
                        // Case-sensitive search
                        if (file.find(token) != std::string::npos) {
                            matchFound = true;
                            break;
                        }
                    } else {
                        // Case-insensitive search
                        std::string tokenLower = token;
                        toLowerInPlace(tokenLower);
                        if (fileLower.find(tokenLower) != std::string::npos) {
                            matchFound = true;
                            break;
                        }
                    }
                }
            }

            if (matchFound) {
                tempFiltered.push_back(file);
                tempIndices.push_back(i);
            }
        }

        // Check if the filter produces valid, different results
        if (tempFiltered.empty() || tempFiltered.size() == files.size()) {
            return false;
        }

        // Update files and filtering stack
        files = std::move(tempFiltered);

        FilteringState newState;
        newState.originalIndices.reserve(tempIndices.size());
        for (size_t idx : tempIndices) {
            size_t originalIdx = isFiltered && !filteringStack.empty() && idx < filteringStack.back().originalIndices.size()
                ? filteringStack.back().originalIndices[idx]
                : idx;
            newState.originalIndices.push_back(originalIdx);
        }
        newState.isFiltered = true;

        if (isFiltered && !filteringStack.empty()) {
            filteringStack.back() = std::move(newState);
        } else {
            filteringStack.push_back(std::move(newState));
        }

        currentPage = 0;
        needsClrScrn = true;
        isFiltered = true;
        need2Sort = true;

        return true;
    };

    // Function to handle filter history
    auto handleHistory = [&](const std::string& query) {
        filterHistory = true;
        loadHistory(filterHistory);
        try {
            add_history(query.c_str());
            saveHistory(filterHistory);
        } catch (const std::exception&) {
            // Optionally log the error
        }
    };

    if (mainInputString == "/") {
        // Interactive filter prompt loop
        std::cout << "\033[1A\033[K";
        std::string filterPrompt = "\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;38;5;208m\002" +
                                   fileExtensionWithOutDots + "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";

        while (true) {
            clear_history();
            filterHistory = true;
            loadHistory(filterHistory);

            std::unique_ptr<char, decltype(&std::free)> rawSearchQuery(
                readline(filterPrompt.c_str()), &std::free);

            if (!rawSearchQuery) {
                std::cout << "\033[2A\033[K";
                needsClrScrn = false;
                need2Sort = false;
                break;
            }

            std::string inputSearch(rawSearchQuery.get());

            if (inputSearch.empty() || inputSearch == "/") {
                std::cout << "\033[2A\033[K";
                needsClrScrn = false;
                need2Sort = false;
                break;
            }

            if (applyFilter(inputSearch)) {
                handleHistory(inputSearch);
                filterHistory = false;
                clear_history();
                break;
            } else {
                std::cout << "\033[1A\033[K";
                continue;
            }
        }
    } else if (mainInputString.size() > 1) {
        // Direct filtering mode with /pattern
        std::string inputSearch = mainInputString.substr(1);

        if (applyFilter(inputSearch)) {
            handleHistory(inputSearch);
            clear_history();
        } else {
            std::cout << "\033[2A\033[K";
            need2Sort = false;
            needsClrScrn = false;
        }
    }
}


// Remove AnsiCodes from filenames
std::string removeAnsiCodes(const std::string& input) {
    std::string result;
    result.reserve(input.size()); // Preallocate memory

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\033' && i + 1 < input.size() && input[i+1] == '[') {
            // Skip CSI sequence: \033[ ... [a-zA-Z]
            size_t j = i + 2; // Skip \033 and [
            while (j < input.size() && (input[j] < 'A' || input[j] > 'Z') && (input[j] < 'a' || input[j] > 'z')) {
                ++j;
            }
            if (j < input.size()) {
                ++j; // Skip the terminating letter
            }
            i = j - 1; // Adjust i (loop will increment to j)
        } else {
            result += input[i];
        }
    }
    return result;
}


// Structure to hold precomputed Boyer-Moore tables and token information
struct QueryToken {
    std::string original;      // Original token (case-sensitive if has uppercase)
    std::string lower;          // Lowercase version (if case-insensitive)
    bool isCaseSensitive;       // Case sensitivity flag
    
    // Precomputed Boyer-Moore tables
    std::vector<int> originalBadChar;
    std::vector<int> originalGoodSuffix;
    std::vector<int> lowerBadChar;
    std::vector<int> lowerGoodSuffix;
};


// Precomputes Boyer-Moore tables for a pattern
void precomputeBoyerMooreTables(const std::string& pattern, std::vector<int>& badCharTable, std::vector<int>& goodSuffixTable) {
    const size_t m = pattern.size();
    const int ALPHABET_SIZE = 256;

    // Bad Character Table
    badCharTable.assign(ALPHABET_SIZE, -1);
    for (int i = 0; i < static_cast<int>(m); ++i) {
        badCharTable[static_cast<unsigned char>(pattern[i])] = i;
    }

    // Good Suffix Table
    goodSuffixTable.resize(m, static_cast<int>(m));
    std::vector<int> suffix(m, 0);

    // Phase 1: Suffix array calculation
    suffix[m - 1] = static_cast<int>(m);
    int g = static_cast<int>(m) - 1;
    int f = static_cast<int>(m) - 1;

    for (int i = static_cast<int>(m) - 2; i >= 0; --i) {
        if (i > g && suffix[i + m - 1 - f] < i - g) {
            suffix[i] = suffix[i + m - 1 - f];
        } else {
            g = std::min(g, i);
            f = i;
            while (g >= 0 && pattern[g] == pattern[g + m - 1 - f]) {
                --g;
            }
            suffix[i] = f - g;
        }
    }

    // Phase 2: Good suffix heuristic rules
    // Case 1: Suffix matches prefix
    for (int i = 0; i < static_cast<int>(m) - 1; ++i) {
        goodSuffixTable[i] = static_cast<int>(m) - 1 - suffix[0];
    }

    // Case 2: Substring matches suffix
    for (int i = 0; i <= static_cast<int>(m) - 2; ++i) {
        const int j = static_cast<int>(m) - 1 - suffix[i];
        if (goodSuffixTable[j] > static_cast<int>(m) - 1 - i) {
            goodSuffixTable[j] = static_cast<int>(m) - 1 - i;
        }
    }
}

// Optimized search that returns on first match
bool boyerMooreSearchExists(const std::string& text, const std::string& pattern, const std::vector<int>& badCharTable, const std::vector<int>& goodSuffixTable) {
    const size_t n = text.size();
    const size_t m = pattern.size();
    if (m == 0 || m > n) return false;

    int s = 0;
    while (s <= static_cast<int>(n - m)) {
        int j = static_cast<int>(m) - 1;
        while (j >= 0 && text[s + j] == pattern[j]) {
            --j;
        }

        if (j < 0) {
            return true;  // Match found
        } else {
            const int bcShift = j - badCharTable[static_cast<unsigned char>(text[s + j])];
            const int gsShift = goodSuffixTable[j];
            s += std::max(1, std::max(bcShift, gsShift));
        }
    }
    return false;
}


// Function to filter cached ISO files or mountpoints based on search query (case-adaptive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    std::vector<std::string> filteredFiles;
    ThreadPool pool(maxThreads);
    std::vector<QueryToken> queryTokens;

    // Tokenize and preprocess query
    std::stringstream ss(query);
    std::string token;
    while (std::getline(ss, token, ';')) {
        // Trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;

        QueryToken qt;
        qt.original = token;
        qt.isCaseSensitive = std::any_of(token.begin(), token.end(), 
            [](unsigned char c) { return std::isupper(c); });

        // Precompute case-insensitive version if needed
        if (!qt.isCaseSensitive) {
            qt.lower = qt.original;
            toLowerInPlace(qt.lower);
            precomputeBoyerMooreTables(qt.lower, qt.lowerBadChar, qt.lowerGoodSuffix);
        }

        // Always precompute case-sensitive tables (original might have uppercase)
        precomputeBoyerMooreTables(qt.original, qt.originalBadChar, qt.originalGoodSuffix);

        queryTokens.push_back(std::move(qt));
    }

    // Determine if we need lowercase preprocessing for any token
    const bool needLowerCase = std::any_of(queryTokens.begin(), queryTokens.end(),
        [](const QueryToken& qt) { return !qt.isCaseSensitive; });

    // Parallel processing
    const size_t chunkSize = (files.size() + maxThreads - 1) / maxThreads;
    std::vector<std::future<std::vector<std::string>>> futures;

    for (unsigned i = 0; i < maxThreads; ++i) {
        const size_t start = i * chunkSize;
        const size_t end = std::min(files.size(), (i + 1) * chunkSize);
        if (start >= end) break;

        futures.emplace_back(pool.enqueue([=, &files, &queryTokens] {
            std::vector<std::string> localMatches;
            for (size_t j = start; j < end; ++j) {
                const std::string cleanFile = removeAnsiCodes(files[j]);
                std::string fileLower;

                // Preprocess filename case if needed
                if (needLowerCase) {
                    fileLower = cleanFile;
                    toLowerInPlace(fileLower);
                }

                // Check all query tokens
                for (const auto& qt : queryTokens) {
                    bool match;
                    if (qt.isCaseSensitive) {
                        match = boyerMooreSearchExists(cleanFile, qt.original, 
                                                     qt.originalBadChar, qt.originalGoodSuffix);
                    } else {
                        match = boyerMooreSearchExists(fileLower, qt.lower,
                                                     qt.lowerBadChar, qt.lowerGoodSuffix);
                    }

                    if (match) {
                        localMatches.push_back(files[j]);
                        break;
                    }
                }
            }
            return localMatches;
        }));
    }

    // Efficient result merging
    std::vector<std::vector<std::string>> threadResults;
    threadResults.reserve(futures.size());
    for (auto& fut : futures) {
        threadResults.push_back(fut.get());
    }

    // Reserve exact space needed
    size_t totalMatches = 0;
    for (const auto& res : threadResults) {
        totalMatches += res.size();
    }
    filteredFiles.reserve(totalMatches);

    // Merge results
    for (const auto& res : threadResults) {
        filteredFiles.insert(filteredFiles.end(), res.begin(), res.end());
    }

    return filteredFiles;
}
