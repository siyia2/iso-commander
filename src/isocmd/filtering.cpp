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

    if (patternLen == 0 || textLen == 0 || patternLen > textLen)
        return {};

    // Preprocess bad character shifts
    std::vector<size_t> badCharShifts(256, patternLen);
    for (size_t i = 0; i < patternLen - 1; ++i)
        badCharShifts[static_cast<unsigned char>(pattern[i])] = patternLen - i - 1;

    // Preprocess good suffix shifts
    std::vector<size_t> goodSuffixShifts(patternLen + 1, patternLen);
    std::vector<size_t> suffixLengths(patternLen + 1, 0);

    // Compute suffix lengths and good suffix shifts
    size_t lastPrefixPos = patternLen;
    for (size_t i = patternLen; i > 0; --i) {
        if (pattern.compare(i - 1, patternLen - i + 1, pattern) == 0)
            lastPrefixPos = i - 1;
        goodSuffixShifts[patternLen - i] = lastPrefixPos + (patternLen - i);
    }

    for (size_t i = 0; i < patternLen - 1; ++i) {
        size_t slen = 0;
        while (i >= slen && pattern[i - slen] == pattern[patternLen - slen - 1])
            ++slen;
        suffixLengths[patternLen - slen - 1] = slen;
    }

    for (size_t i = 0; i < patternLen - 1; ++i)
        goodSuffixShifts[suffixLengths[i]] = patternLen - i - 1;

    // Search for pattern in text
    std::vector<size_t> matches;
    size_t i = 0;
    while (i <= textLen - patternLen) {
        size_t j = patternLen - 1;
        while (j != static_cast<size_t>(-1) && pattern[j] == text[i + j]) {
            --j;
        }
        if (j == static_cast<size_t>(-1)) {
            matches.push_back(i);
            i += goodSuffixShifts[0];
        } else {
            i += std::max(goodSuffixShifts[j], badCharShifts[static_cast<unsigned char>(text[i + j])]);
        }
    }

    return matches;
}


// Function to filter cached ISO files based on search query (case-insensitive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    std::vector<std::string> filteredFiles;
    std::set<std::string> queryTokens;
    
    std::stringstream ss(query);
    std::string token;
    
    while (std::getline(ss, token, ';')) {
		toLowerInPlace(token);
		if (token.length() > 1) { // Only insert if length is greater than 1
			queryTokens.insert(token);
		}
	}

    std::shared_mutex filterMutex;
    
    auto filterTask = [&](size_t start, size_t end) {
        std::vector<std::string> localFilteredFiles;
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];
            std::string fileName = file;
			toLowerInPlace(fileName);
            
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
        
        std::unique_lock<std::shared_mutex> lock(filterMutex);
        filteredFiles.insert(filteredFiles.end(), localFilteredFiles.begin(), localFilteredFiles.end());
    };

    size_t numFiles = files.size();
    size_t numThreads = std::min(static_cast<size_t>(maxThreads), numFiles);
    size_t filesPerThread = numFiles / numThreads;
    
    std::vector<std::future<void>> futures;
    
    for (size_t i = 0; i < numThreads - 1; ++i) {
        size_t start = i * filesPerThread;
        size_t end = start + filesPerThread;
        futures.emplace_back(std::async(std::launch::async, filterTask, start, end));
    }
    
    filterTask((numThreads - 1) * filesPerThread, numFiles);
    
    for (auto& future : futures) {
        future.wait();
    }

    return filteredFiles;
}
