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
    for (size_t i = 0; i < patternLen; ++i) 
        badCharShifts[static_cast<unsigned char>(pattern[i])] = patternLen - i - 1;
    
    // Preprocess good suffix shifts
    std::vector<size_t> goodSuffixShifts(patternLen, patternLen);
    std::vector<size_t> suffixLengths(patternLen, 0);
    
    // Compute suffix lengths
    suffixLengths[patternLen - 1] = patternLen;
    for (size_t i = patternLen - 1; i > 0; --i) {
        size_t j = 0;
        while (i >= j + 1 && pattern[i - j - 1] == pattern[patternLen - 1 - j]) {
            ++j;
        }
        suffixLengths[i - 1] = j;
    }
    
    // Compute good suffix shifts
    for (size_t i = 0; i < patternLen; ++i) {
        size_t shift = patternLen;
        if (suffixLengths[i] > 0) {
            shift = patternLen - i - 1;
        }
        for (size_t j = patternLen - suffixLengths[i]; j < patternLen; ++j) {
            goodSuffixShifts[j] = std::min(goodSuffixShifts[j], shift);
        }
    }

    // Search for pattern in text
    std::vector<size_t> matches;
    size_t i = 0;
    while (i <= textLen - patternLen) {
        size_t j = patternLen - 1;
        while (j < patternLen && pattern[j] == text[i + j]) {
            if (j == 0) {
                matches.push_back(i);
                break;
            }
            --j;
        }
        if (j < patternLen && pattern[j] != text[i + j]) {
            unsigned char badChar = static_cast<unsigned char>(text[i + j]);
            size_t badCharShift = badCharShifts[badChar];
            size_t goodSuffixShift = goodSuffixShifts[j];
            i += std::max<size_t>(1, std::min(badCharShift, goodSuffixShift));
        } else {
            i += patternLen; // Pattern match found
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
