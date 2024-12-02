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
    size_t patternLen = pattern.length();
    size_t textLen = text.length();
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) return {};
    
    std::vector<size_t> matches;
    
    // Single-character pattern optimization
    if (patternLen == 1) {
        char singleChar = pattern[0];
        for (size_t i = 0; i < textLen; ++i) {
            if (text[i] == singleChar) {
                matches.push_back(i);
            }
        }
        return matches;
    }
    
    // Bad character rule preprocessing
    std::vector<size_t> badCharShifts(256, patternLen);
    for (size_t i = 0; i < patternLen - 1; ++i) {
        badCharShifts[static_cast<unsigned char>(pattern[i])] = patternLen - i - 1;
    }
    
    // Good suffix rule preprocessing
    std::vector<size_t> goodSuffixShifts(patternLen, patternLen);
    std::vector<size_t> suffixLengths(patternLen, 0);
    
    // Compute suffix lengths from right to left
    suffixLengths[patternLen - 1] = patternLen;
    size_t lastPrefixPosition = patternLen;
    
    for (size_t i = patternLen - 1; i > 0; --i) {
        // Cast i-1 to size_t to avoid signed/unsigned comparison
        size_t idx = i - 1;
        
        // If we can extend the prefix
        if (memcmp(pattern.c_str() + idx + 1, pattern.c_str() + patternLen - lastPrefixPosition, lastPrefixPosition - (idx + 1)) == 0) {
            suffixLengths[idx] = lastPrefixPosition - (idx + 1);
            lastPrefixPosition = idx + 1;
        }
    }
    
    // Compute good suffix shifts
    for (size_t i = 0; i < patternLen; ++i) {
        goodSuffixShifts[i] = patternLen;
    }
    
    for (size_t i = patternLen - 1; i < patternLen; --i) {
        if (suffixLengths[i] == i + 1) {
            for (size_t j = 0; j < patternLen - i - 1; ++j) {
                if (goodSuffixShifts[j] == patternLen) {
                    goodSuffixShifts[j] = patternLen - i - 1;
                }
            }
        }
        
        // Prevent unsigned underflow
        if (i == 0) break;
    }
    
    for (size_t i = patternLen - 1; i < patternLen; --i) {
        if (suffixLengths[i] > 0) {
            goodSuffixShifts[patternLen - suffixLengths[i]] = patternLen - i - 1;
        }
        
        // Prevent unsigned underflow
        if (i == 0) break;
    }
    
    // Search phase
    size_t i = 0;
    while (i <= textLen - patternLen) {
        size_t skip = 0;
        while (skip < patternLen && pattern[patternLen - 1 - skip] == text[i + patternLen - 1 - skip]) {
            skip++;
        }
        
        // Pattern found
        if (skip == patternLen) {
            matches.push_back(i);
        }
        
        // Compute the maximum shift using both bad character and good suffix rules
        size_t badCharShift = (i + patternLen < textLen) 
            ? badCharShifts[static_cast<unsigned char>(text[i + patternLen - 1])] 
            : 1;
        
        size_t goodSuffixShift = goodSuffixShifts[skip];
        
        // Take the maximum shift
        i += std::max(1ul, std::min(badCharShift, goodSuffixShift));
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
