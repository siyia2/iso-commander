// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Conver strings to lowercase efficiently
void toLowerInPlace(std::string& str) {
    for (char& c : str) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
}


// Boyer-Moore string search implementation for files
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text) {
    const size_t patternLen = pattern.length();
    const size_t textLen = text.length();
    std::vector<size_t> matches;
    // Early exit conditions
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) 
        return matches;
    // Single character optimization
    if (patternLen == 1) {
        matches.reserve(textLen / 2); // Heuristic preallocation
        for (size_t i = 0; i < textLen; ++i) 
            if (text[i] == pattern[0]) 
                matches.push_back(i);
        return matches;
    }
    // Preprocess bad character shifts using last occurrence
    std::vector<ssize_t> lastOccurrence(256, -1);
    for (size_t i = 0; i < patternLen; ++i) 
        lastOccurrence[static_cast<unsigned char>(pattern[i])] = i;
    
    // Preprocess good suffix shifts
    std::vector<size_t> goodSuffixShifts(patternLen + 1, patternLen);
    std::vector<size_t> suffixLengths(patternLen + 1, 0);
    
    // Calculate suffix lengths
    // Use signed integers for f and g to avoid sign comparison issues
    ssize_t f = 0, g = patternLen - 1;
    suffixLengths[patternLen - 1] = patternLen;
    
    for (ssize_t i = patternLen - 2; i >= 0; --i) {
        if (i > g && suffixLengths[i + patternLen - 1 - f] < static_cast<size_t>(i - g)) {
            suffixLengths[i] = suffixLengths[i + patternLen - 1 - f];
        } else {
            if (i < g) g = i;
            f = i;
            while (g >= 0 && pattern[g] == pattern[g + patternLen - 1 - f]) {
                --g;
            }
            suffixLengths[i] = f - g;
        }
    }
    
    // Calculate the shifts based on suffix lengths
    for (size_t i = 0; i < patternLen; ++i) {
        goodSuffixShifts[i] = patternLen;
    }
    
    // Case 1: For each mismatch position, find the rightmost matching suffix
    for (ssize_t i = patternLen - 1; i >= 0; --i) {
        if (suffixLengths[i] == static_cast<size_t>(i + 1)) {  // This is a proper suffix that matches a prefix
            for (size_t j = 0; j < patternLen - 1 - i; ++j) {
                if (goodSuffixShifts[j] == patternLen) {
                    goodSuffixShifts[j] = patternLen - 1 - i;
                }
            }
        }
    }
    
    // Case 2: For each position, find the longest suffix that matches elsewhere
    for (size_t i = 0; i < patternLen - 1; ++i) {
        goodSuffixShifts[patternLen - 1 - suffixLengths[i]] = 
            patternLen - 1 - i;
    }
    
    // Search phase
    matches.reserve((textLen - patternLen + 1) / 2); // Heuristic preallocation
    size_t i = 0;
    while (i <= textLen - patternLen) {
        ssize_t j = patternLen - 1;  // Use ssize_t for j since we check j >= 0
        while (j >= 0 && pattern[j] == text[i + j]) 
            --j;
        if (j < 0) {
            matches.push_back(i);
            i += goodSuffixShifts[0];
        } else {
            size_t badCharShift = j - lastOccurrence[static_cast<unsigned char>(text[i + j])];
            if (badCharShift < 1) 
                badCharShift = 1;
            size_t goodSuffixShift = goodSuffixShifts[j];
            i += std::max(badCharShift, goodSuffixShift);
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

    // Structure to hold token information.
    struct QueryToken {
        std::string original;
        std::string lower;  // computed only if needed
        bool isCaseSensitive;
    };
    std::vector<QueryToken> queryTokens;
    
    // Tokenize the query and precompute lowercase for case-insensitive tokens.
    std::stringstream ss(query);
    std::string token;
    while (std::getline(ss, token, ';')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) {
            bool hasUpperCase = std::any_of(token.begin(), token.end(),
                [](unsigned char c) { return std::isupper(c); });
            QueryToken qt;
            qt.original = token;
            qt.isCaseSensitive = hasUpperCase;
            if (!hasUpperCase) {
                qt.lower = token;
                toLowerInPlace(qt.lower);
            }
            queryTokens.push_back(std::move(qt));
        }
    }
    
    // Determine if we need to convert file names to lowercase.
    bool needLowerCaseFile = std::any_of(queryTokens.begin(), queryTokens.end(),
                                         [](const QueryToken& qt) { return !qt.isCaseSensitive; });
        
    const size_t totalFiles = files.size();
    size_t chunkSize = (totalFiles + maxThreads - 1) / maxThreads;
    
    // Launch asynchronous tasks to process chunks of files.
    std::vector<std::future<std::vector<std::string>>> futures;
    for (unsigned int i = 0; i < maxThreads; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(totalFiles, (i + 1) * chunkSize);
        if (start >= end)
            break;
        
        futures.push_back(std::async(std::launch::async, [start, end, &files, &queryTokens, needLowerCaseFile]() -> std::vector<std::string> {
            std::vector<std::string> localMatches;
            for (size_t j = start; j < end; ++j) {
                const std::string& file = files[j];
                std::string cleanFileName = removeAnsiCodes(file);
                std::string fileNameLower;
                if (needLowerCaseFile) {
                    fileNameLower = cleanFileName;
                    toLowerInPlace(fileNameLower);
                }
                
                bool matchFound = false;
                for (const auto& qt : queryTokens) {
                    if (qt.isCaseSensitive) {
                        if (!boyerMooreSearch(qt.original, cleanFileName).empty()) {
                            matchFound = true;
                            break;
                        }
                    } else {
                        if (!boyerMooreSearch(qt.lower, fileNameLower).empty()) {
                            matchFound = true;
                            break;
                        }
                    }
                }
                if (matchFound) {
                    localMatches.push_back(file);
                }
            }
            return localMatches;
        }));
    }
    
    // Merge the results from all threads.
    for (auto& fut : futures) {
        std::vector<std::string> localResult = fut.get();
        filteredFiles.insert(filteredFiles.end(),
                             localResult.begin(), localResult.end());
    }
    
    return filteredFiles;
}
