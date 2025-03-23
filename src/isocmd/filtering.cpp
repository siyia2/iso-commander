// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../filtering.h"


// Global vector to track if filtering stack has items
std::vector<FilteringState> filteringStack;


// Function to hanlde filtering for selectForIsoFiles
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, const std::string& operation, const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount) {
    // Check if the input indicates a filtering operation (must start with '/')
    if (inputString != "/" && (inputString.empty() || inputString[0] != '/')) {
        return false;  // Not a filtering operation
    }
    
    bool isFilterPrompt = (inputString == "/"); // Determine if this is an interactive filter prompt
    
    // Function to apply filtering based on search string
    auto applyFilter = [&](const std::string& searchString) -> bool {
        if (searchString.empty()) {
            return false;
        }
        
        const std::vector<std::string>& sourceList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : globalIsoFileList);
        auto tempFiltered = filterFiles(sourceList, searchString);
        
        // Convert the tempFiltered vector to an unordered_set for O(1) lookups
        std::unordered_set<std::string> filteredSet(tempFiltered.begin(), tempFiltered.end());
        
        // Build new filtered files and update the filtering stack
        std::vector<std::string> newFilteredFiles;
        std::vector<size_t> newIndices;
        
        // Pre-allocate memory to avoid reallocations
        newFilteredFiles.reserve(filteredSet.size());
        newIndices.reserve(filteredSet.size());
        
        for (size_t i = 0; i < sourceList.size(); ++i) {
            // O(1) lookup using unordered_set instead of O(n) with std::find
            if (filteredSet.find(sourceList[i]) != filteredSet.end()) {
                newFilteredFiles.push_back(sourceList[i]); // Store only the file string without index
                
                // Map to the original index, accounting for previous filtering
                size_t originalIdx = i;
                if (isFiltered && i < filteringStack.back().originalIndices.size()) {
                    originalIdx = filteringStack.back().originalIndices[i];
                }
                newIndices.push_back(originalIdx);
            }
        }
        
        bool filterUnchanged = newFilteredFiles.size() == sourceList.size();
        bool hasResults = !newFilteredFiles.empty();
        
        // If filtering has results and changes the list, update the filtered list
        if (!filterUnchanged && hasResults) {
			currentPage = 0;
            needsClrScrn = true;
            
            // Use move semantics to avoid unnecessary copying
            filteredFiles = std::move(newFilteredFiles);
            
            // Update filtering stack
            FilteringState newState;
            newState.originalIndices = std::move(newIndices); // Using move semantics
            newState.isFiltered = true;
            
            if (isFiltered) {
                // Replace the top of the stack with the new filtering state
                filteringStack.back() = std::move(newState); // Using move semantics
            } else {
                // Add a new filtering state to the stack
                filteringStack.push_back(std::move(newState)); // Using move semantics
            }
            
            isFiltered = true;
            return true;
        }
        
        return hasResults;  // Return if there were any results
    };
    
    if (isFilterPrompt) {
        // Interactive filter prompt loop
        while (true) {
            filterHistory = true; // Enable filter history
            loadHistory(filterHistory); // Load filter history
            std::cout << "\033[1A\033[K"; // Move cursor up and clear line
            
            // Construct the prompt message with color formatting
            std::string filterPrompt = "\001\033[1;38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001" + 
                                         operationColor + "\002" + operation + 
                                         "\001\033[1;94m\002, or ↵ to return: \001\033[0;1m\002";
            
            // Get user input using readline
            std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);
            
            // If input is empty or just '/', exit filtering mode
            if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
                clear_history();
                needsClrScrn = isFiltered ? true : false;
                return true;
            }
            
            std::string searchString = searchQuery.get();
            
            if (applyFilter(searchString)) {
                add_history(searchQuery.get()); // Save filter query to history
                saveHistory(filterHistory);
                clear_history();
                return true;
            }
        }
    } else {
        // Quick filtering mode with /pattern
        std::string searchString = inputString.substr(1); // Extract search term after '/'
        
        if (applyFilter(searchString)) {
            filterHistory = true;
            loadHistory(filterHistory);
            add_history(searchString.c_str()); // Save query to history
            saveHistory(filterHistory);
        }
    }
    return true;  // Filtering operation was handled
}


// Handle filtering for select_and_convert_to_iso
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort) {
    // Exit early if not a filtering command
    if (mainInputString.empty() || (mainInputString != "/" && mainInputString[0] != '/')) {
        return;
    }
    
    // Function to apply filtering based on search string
    auto applyFilter = [&](const std::string& searchString) -> bool {
        if (searchString.empty()) {
            return false;
        }
        
        auto tempFiltered = filterFiles(files, searchString);
        
        // Check if the filter produces valid, different results
        if (tempFiltered.empty() || tempFiltered.size() == files.size()) {
            return false;
        }
        
        // Convert tempFiltered to unordered_set for O(1) lookups
        std::unordered_set<std::string> filteredSet(tempFiltered.begin(), tempFiltered.end());
        
        // Build new filtered files and update filtering stack
        std::vector<std::string> newFiltered;
        std::vector<size_t> newIndices;
        
        // Pre-allocate memory to avoid reallocations
        newFiltered.reserve(filteredSet.size());
        newIndices.reserve(filteredSet.size());
        
        for (size_t i = 0; i < files.size(); ++i) {
            // O(1) lookup using unordered_set instead of O(n) with std::find
            if (filteredSet.find(files[i]) != filteredSet.end()) {
                newFiltered.push_back(files[i]); // Store the original item without index
                
                // Map to the original index, accounting for previous filtering
                size_t originalIdx = i;
                if (isFiltered && !filteringStack.empty() && i < filteringStack.back().originalIndices.size()) {
                    originalIdx = filteringStack.back().originalIndices[i];
                }
                newIndices.push_back(originalIdx);
            }
        }
        
        // Use move semantics to transfer ownership
        files = std::move(newFiltered); // Update the file list with the filtered results
        
        // Update filtering stack
        FilteringState newState;
        newState.originalIndices = std::move(newIndices);
        newState.isFiltered = true;
        
        if (isFiltered && !filteringStack.empty()) {
            filteringStack.back() = std::move(newState); // Replace the top state using move
        } else {
            filteringStack.push_back(std::move(newState)); // Add new state using move
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
        } catch (const std::exception& e) {
            // Optionally, log the error or handle it
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

            // Prompt for search query
            std::unique_ptr<char, decltype(&std::free)> rawSearchQuery(readline(filterPrompt.c_str()), &std::free);
            
            // Check for null before dereferencing
            if (!rawSearchQuery) {
                std::cout << "\033[2A\033[K";
                needsClrScrn = false;
                need2Sort = false;
                break;
            }
            
            std::string inputSearch(rawSearchQuery.get());

            // Exit if input is empty or "/"
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
                std::cout << "\033[1A\033[K"; // Clear line for retry
                continue;
            }
        }
    } else if (mainInputString.size() > 1) {
        // Direct filtering mode with /pattern
        std::string inputSearch = mainInputString.substr(1); // Skip the '/' character
        
        if (applyFilter(inputSearch)) {
            handleHistory(inputSearch);
            clear_history();
        } else {
            std::cout << "\033[2A\033[K"; // Clear the line if no files match
            need2Sort = false;
            needsClrScrn = false;
        }
    }
}


// Boyer-Moore string search implementation for files
std::vector<size_t> boyerMooreSearch(const std::string &pattern, const std::string &text) {
    std::vector<size_t> matches;
    size_t m = pattern.size();
    size_t n = text.size();
    
    // Early exit conditions
    if(m == 0 || m > n)
        return matches;
    
    // --- Bad Character Rule Preprocessing ---
    const int ALPHABET_SIZE = 256;
    // For each character, record its last index in the pattern.
    std::vector<int> badChar(ALPHABET_SIZE, -1);
    for (int i = 0; i < static_cast<int>(m); i++) {
        badChar[static_cast<unsigned char>(pattern[i])] = i;
    }
    
    // --- Good Suffix Rule Preprocessing ---
    // suffix[i] will hold the length of the longest substring ending at position i
    // which is also a suffix of the pattern.
    std::vector<int> suffix(m, 0);
    suffix[m - 1] = m;
    int g = static_cast<int>(m) - 1; // the rightmost position of a matching suffix
    int f = static_cast<int>(m) - 1; // a working index
    
    for (int i = static_cast<int>(m) - 2; i >= 0; i--) {
        if (i > g && suffix[i + m - 1 - f] < i - g)
            suffix[i] = suffix[i + m - 1 - f];
        else {
            if (i < g)
                g = i;
            f = i;
            while (g >= 0 && pattern[g] == pattern[g + m - 1 - f])
                g--;
            suffix[i] = f - g;
        }
    }
    
    // Now build the goodSuffix shift table
    std::vector<int> goodSuffix(m, static_cast<int>(m));
    // First phase: set shift for the case where a suffix of the pattern matches a prefix.
    int j = 0;
    for (int i = static_cast<int>(m) - 1; i >= -1; i--) {
        if (i == -1 || suffix[i] == i + 1) {
            for (; j < static_cast<int>(m) - 1 - i; j++) {
                if (goodSuffix[j] == static_cast<int>(m))
                    goodSuffix[j] = static_cast<int>(m) - 1 - i;
            }
        }
    }
    // Second phase: for the remaining positions
    for (int i = 0; i <= static_cast<int>(m) - 2; i++) {
        goodSuffix[m - 1 - suffix[i]] = static_cast<int>(m) - 1 - i;
    }
    
    // --- Search Phase ---
    int s = 0;  // s is the shift of the pattern with respect to the text
    while (s <= static_cast<int>(n - m)) {
        int j = static_cast<int>(m) - 1;
        // Move backwards through the pattern while characters match.
        while (j >= 0 && pattern[j] == text[s + j])
            j--;
        
        if (j < 0) {
            // A match is found at position s
            matches.push_back(s);
            // Shift pattern to align the next possible match using goodSuffix[0]
            s += goodSuffix[0];
        } else {
            // Calculate the shift based on the bad character rule:
            // j - last occurrence index of text[s+j] in the pattern.
            int bcShift = j - badChar[static_cast<unsigned char>(text[s + j])];
            // Use the precomputed good suffix shift.
            int gsShift = goodSuffix[j];
            // Advance by the maximum shift to ensure progress.
            s += std::max(1, std::max(bcShift, gsShift));
        }
    }
    
    return matches;
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


// Function to filter cached ISO files or mountpoints based on search query (case-adaptive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    // Reserve memory for filteredFiles (we will merge per thread later).
    std::vector<std::string> filteredFiles;
    ThreadPool pool(maxThreads);  // Initialize ThreadPool

    // Structure to hold token information.
    struct QueryToken {
        std::string original;  // The original token from the query.
        std::string lower;     // Lowercase version of the token (only if needed).
        bool isCaseSensitive;  // Flag to indicate if the token is case-sensitive.
    };
    std::vector<QueryToken> queryTokens;
    
    // Tokenize the query using ';' as a delimiter and remove leading/trailing spaces.
    std::stringstream ss(query);
    std::string token;
    while (std::getline(ss, token, ';')) {
        // Trim leading and trailing spaces.
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        if (!token.empty()) {
            // Determine if the token contains uppercase letters (implying case sensitivity).
            bool hasUpperCase = std::any_of(token.begin(), token.end(),
                [](unsigned char c) { return std::isupper(c); });

            // Construct the QueryToken object.
            QueryToken qt;
            qt.original = token;
            qt.isCaseSensitive = hasUpperCase;

            // Precompute the lowercase version if the token is case-insensitive.
            if (!hasUpperCase) {
                qt.lower = token;
                toLowerInPlace(qt.lower);
            }

            queryTokens.push_back(std::move(qt)); // Store the token.
        }
    }
    
    // Check if at least one query token is case-insensitive.
    bool needLowerCaseFile = std::any_of(queryTokens.begin(), queryTokens.end(),
                                         [](const QueryToken& qt) { return !qt.isCaseSensitive; });
        
    const size_t totalFiles = files.size();
    
    // Determine the chunk size for dividing work among threads.
    size_t chunkSize = (totalFiles + maxThreads - 1) / maxThreads;
    
    // Vector to store futures for thread pool tasks
    std::vector<std::future<std::vector<std::string>>> futures;

    // Submit tasks to the thread pool
    for (unsigned int i = 0; i < maxThreads; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(totalFiles, (i + 1) * chunkSize);
        
        // If the start index is out of range, break.
        if (start >= end)
            break;
        
        // Submit a task to the thread pool
        futures.emplace_back(pool.enqueue([start, end, &files, &queryTokens, needLowerCaseFile]() -> std::vector<std::string> {
            std::vector<std::string> localMatches; // Store matching files for this thread.

            for (size_t j = start; j < end; ++j) {
                const std::string& file = files[j];

                // Remove ANSI escape codes (e.g., color codes in terminal output).
                std::string cleanFileName = removeAnsiCodes(file);

                // Convert filename to lowercase if necessary.
                std::string fileNameLower;
                if (needLowerCaseFile) {
                    fileNameLower = cleanFileName;
                    toLowerInPlace(fileNameLower);
                }
                
                bool matchFound = false;

                // Iterate through query tokens and search in file names.
                for (const auto& qt : queryTokens) {
                    if (qt.isCaseSensitive) {
                        // Perform a case-sensitive Boyer-Moore search.
                        if (!boyerMooreSearch(qt.original, cleanFileName).empty()) {
                            matchFound = true;
                            break;
                        }
                    } else {
                        // Perform a case-insensitive search.
                        if (!boyerMooreSearch(qt.lower, fileNameLower).empty()) {
                            matchFound = true;
                            break;
                        }
                    }
                }

                // If any query token matches, add the file to results.
                if (matchFound) {
                    localMatches.push_back(file);
                }
            }
            return localMatches; // Return matches found in this chunk.
        }));
    }
    
    // Merge results from all thread pool tasks
    for (auto& fut : futures) {
        std::vector<std::string> localResult = fut.get();
        filteredFiles.insert(filteredFiles.end(),
                             localResult.begin(), localResult.end());
    }
    
    return filteredFiles; // Return the final list of matching files.
}
