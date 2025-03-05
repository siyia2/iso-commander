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
    
    // Early exit conditions
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) 
        return {};
    
    // Single character optimization
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


// Remove AnsiCodes from filenames
std::string removeAnsiCodes(const std::string& input) {
    std::string result;
    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\033' && i + 1 < input.length() && input[i+1] == '[') {
            // Skip the entire ANSI escape sequence
            while (i < input.length() && !isalpha(input[i])) {
                ++i;
            }
        } else {
            result += input[i];
        }
    }
    return result;
}


// Function to filter cached ISO files or mountpoints based on search query (case-insensitive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    std::vector<std::string> filteredFiles;
    std::unordered_set<std::string> queryTokens;

    // Tokenize the query and convert each token to lowercase
    std::stringstream ss(query);
    std::string token;
    
    while (std::getline(ss, token, ';')) {
        toLowerInPlace(token);
        queryTokens.insert(token);
    }

    // This mutex will only be used for the final merge
    std::mutex filterMutex;

    auto filterTask = [&](size_t start, size_t end) {
    std::vector<std::string> localFilteredFiles;
    for (size_t i = start; i < end; ++i) {
        const std::string& file = files[i];
        std::string cleanFileName = removeAnsiCodes(file);  // Remove ANSI codes first
        std::string fileName = cleanFileName;  // Copy the clean file name
        toLowerInPlace(fileName);  // Convert once to lowercase

        // Check if any of the query tokens is found in the file name
        bool matchFound = false;
        for (const std::string& queryToken : queryTokens) {
            if (!boyerMooreSearch(queryToken, fileName).empty()) {
                matchFound = true;
                break;
            }
        }

        if (matchFound) {
            localFilteredFiles.push_back(file);  // Push back the original file name with color codes
        }
    }

        // Merge local results into the global filteredFiles vector
        std::lock_guard<std::mutex> lock(filterMutex);
        filteredFiles.insert(filteredFiles.end(), localFilteredFiles.begin(), localFilteredFiles.end());
    };

    size_t numFiles = files.size();
    size_t numThreads = std::min(static_cast<size_t>(maxThreads), numFiles);

    // Calculate the batch size based on the number of threads
    size_t batchSize = (numFiles + numThreads - 1) / numThreads; // This ensures at least one file per thread

    std::vector<std::future<void>> futures;

    // Launch threads to process files in batches
    for (size_t i = 0; i < numFiles; i += batchSize) {
        size_t start = i;
        size_t end = std::min(i + batchSize, numFiles);
        
        // Launch each batch processing task asynchronously
        futures.push_back(std::async(std::launch::async, filterTask, start, end));
    }

    // Wait for all threads to finish
    for (auto& future : futures) {
        future.wait();
    }

    return filteredFiles;
}

