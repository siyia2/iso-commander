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

    // Early exit for trivial cases (unchanged)
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) 
        return matches;

    // Single character optimization (unchanged)
    if (patternLen == 1) {
        for (size_t i = 0; i < textLen; ++i) 
            if (text[i] == pattern[0]) 
                matches.push_back(i);
        return matches;
    }

    // Bad Character Heuristic preprocessing (unchanged)
    std::vector<ssize_t> badCharShifts(256, -1);
    for (size_t i = 0; i < patternLen; ++i) 
        badCharShifts[static_cast<unsigned char>(pattern[i])] = i;

    // Good Suffix Heuristic preprocessing (unchanged)
    std::vector<size_t> goodSuffixShifts(patternLen + 1, patternLen);
    std::vector<size_t> suffixLengths(patternLen, 0);

    // Phase 1: Compute suffix lengths
    for (size_t i = patternLen - 1; i > 0; --i) {
        size_t k = 0;
        while (k < i && pattern[i - 1 - k] == pattern[patternLen - 1 - k])
            ++k;
        suffixLengths[i] = k;
    }

    // Phase 2: Compute prefix shifts
    std::vector<bool> isPrefix(patternLen, false);
    for (size_t i = 0; i < patternLen; ++i) {
        if (suffixLengths[i] == i + 1) 
            isPrefix[patternLen - 1 - i] = true;
    }

    // Phase 3: Compute good suffix shifts
    for (size_t i = 0; i < patternLen - 1; ++i) {
        const size_t j = patternLen - 1 - suffixLengths[i];
        goodSuffixShifts[j] = patternLen - 1 - i;
    }

    // Handle full prefix matches
    size_t longest_prefix_shift = patternLen;
	for (size_t i = 0; i < patternLen; ++i) {
		if (isPrefix[i]) {
			longest_prefix_shift = patternLen - 1 - i;
			break; // Longest prefix found first
		}
	}
	for (size_t j = 0; j <= patternLen; ++j) {
		if (goodSuffixShifts[j] == patternLen) {
			goodSuffixShifts[j] = longest_prefix_shift;
		}
	}

    // Search phase
    size_t textIndex = 0;
    while (textIndex <= textLen - patternLen) {
        int patternIndex = static_cast<int>(patternLen) - 1;
        while (patternIndex >= 0 && pattern[patternIndex] == text[textIndex + patternIndex])
            --patternIndex;

        if (patternIndex < 0) {
            matches.push_back(textIndex);
            textIndex += goodSuffixShifts[0];
        } else {
            // Fixed bad character shift calculation
            const ssize_t bcShift = patternIndex - badCharShifts[static_cast<unsigned char>(
                text[textIndex + patternIndex]
            )];
            const size_t badCharShift = std::max<ssize_t>(1, bcShift);  // Ensure ≥1
            const size_t goodSuffixShift = goodSuffixShifts[patternIndex + 1];
            
            textIndex += std::max(goodSuffixShift, badCharShift);
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
