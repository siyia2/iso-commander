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
