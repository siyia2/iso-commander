// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// Sorts items in a case-insensitive manner
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    std::sort(files.begin(), files.end(), 
        [](const std::string& a, const std::string& b) {
            return strcasecmp(a.c_str(), b.c_str()) < 0;
        }
    );
}


// Conver string s to lowercase efficiently
void toLowerInPlace(std::string& str) {
    for (char& c : str) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
}

// Boyer-Moore string search implementation for files
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text) {
    const size_t patternLen = pattern.length();
    const size_t textLen = text.length();
    
    // Early exit conditions
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) 
        return {};
    
    // Single-character optimization
    if (patternLen == 1) {
        std::vector<size_t> matches;
        for (size_t i = 0; i < textLen; ++i) 
            if (text[i] == pattern[0]) 
                matches.push_back(i);
        return matches;
    }
    
    // Preprocess bad character shifts
    std::vector<size_t> badCharShifts(256, patternLen);
    for (size_t i = 0; i < patternLen - 1; ++i) 
        badCharShifts[static_cast<unsigned char>(pattern[i])] = patternLen - i - 1;
    
    // Preprocess good suffix shifts
    std::vector<size_t> goodSuffixShifts(patternLen, patternLen);
    std::vector<size_t> suffixLengths(patternLen, 0);
    
    // Compute suffix lengths and good suffix shifts
    suffixLengths[patternLen - 1] = patternLen;
    size_t lastPrefixPosition = patternLen;
    
    for (int i = patternLen - 2; i >= 0; --i) {
        if (memcmp(pattern.c_str() + i + 1, pattern.c_str() + patternLen - lastPrefixPosition, lastPrefixPosition - (i + 1)) == 0) {
            suffixLengths[i] = lastPrefixPosition - (i + 1);
            lastPrefixPosition = i + 1;
        }
    }
    
    // Update good suffix shifts
    for (size_t i = 0; i < patternLen; ++i) {
        if (suffixLengths[i] == i + 1) {
            for (size_t j = 0; j < patternLen - i - 1; ++j) 
                if (goodSuffixShifts[j] == patternLen) 
                    goodSuffixShifts[j] = patternLen - i - 1;
        }
        
        if (suffixLengths[i] > 0) 
            goodSuffixShifts[patternLen - suffixLengths[i]] = patternLen - i - 1;
    }
    
    // Search phase
    std::vector<size_t> matches;
    for (size_t i = 0; i <= textLen - patternLen;) {
        size_t skip = 0;
        while (skip < patternLen && pattern[patternLen - 1 - skip] == text[i + patternLen - 1 - skip]) 
            ++skip;
        
        // Pattern found
        if (skip == patternLen) 
            matches.push_back(i);
        
        // Compute shift
        size_t badCharShift = (i + patternLen < textLen) 
            ? badCharShifts[static_cast<unsigned char>(text[i + patternLen - 1])] 
            : 1;
        
        size_t goodSuffixShift = goodSuffixShifts[skip];
        
        // Take the maximum shift
        i += std::max(1ul, std::min(badCharShift, goodSuffixShift));
    }
    
    return matches;
}


// Function to filter cached ISO files or mountpoints based on search query (case-insensitive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    std::vector<std::string> filteredFiles;
    std::set<std::string> queryTokens;

    // Tokenize the query and convert each token to lowercase
    std::stringstream ss(query);
    std::string token;
    
    while (std::getline(ss, token, ';')) {
        toLowerInPlace(token);
        if (token.length() > 1) { // Only insert if length is greater than 1
            queryTokens.insert(token);
        }
    }

    // This mutex will only be used for the final merge
    std::mutex filterMutex;

    auto filterTask = [&](size_t start, size_t end) {
        std::vector<std::string> localFilteredFiles;
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];
            std::string fileName = file;
            toLowerInPlace(fileName);

            // Check if any of the query tokens is found in the file name
            bool matchFound = false;
            for (const std::string& queryToken : queryTokens) {
                if (!boyerMooreSearch(queryToken, fileName).empty()) {
                    matchFound = true;
                    break;
                }
            }

            if (matchFound) {
                localFilteredFiles.push_back(file);
            }
        }

        // Merge local results into the global filteredFiles vector
        std::lock_guard<std::mutex> lock(filterMutex);
        filteredFiles.insert(filteredFiles.end(), localFilteredFiles.begin(), localFilteredFiles.end());
    };

    size_t numFiles = files.size();
    size_t numThreads = std::min(static_cast<size_t>(maxThreads), numFiles);
    size_t filesPerThread = numFiles / numThreads;

    std::vector<std::future<void>> futures;

    // Launch threads
    for (size_t i = 0; i < numThreads - 1; ++i) {
        size_t start = i * filesPerThread;
        size_t end = start + filesPerThread;
        futures.push_back(std::async(std::launch::async, filterTask, start, end));
    }

    // Handle the remaining files for the last thread
    filterTask((numThreads - 1) * filesPerThread, numFiles);

    // Wait for all threads to finish
    for (auto& future : futures) {
        future.wait();
    }

    return filteredFiles;
}
