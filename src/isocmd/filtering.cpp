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

// Rabin-Karp string search implementation for filenames
std::vector<size_t> rabinKarpSearch(const std::string& pattern, const std::string& text) {
    std::vector<size_t> matches;
    
    // Handle edge cases
    size_t patternLen = pattern.length();
    size_t textLen = text.length();
    if (patternLen == 0 || textLen == 0 || patternLen > textLen) {
        return matches;
    }
    
    // Prime number for hash calculation
    const int prime = 101;
    
    // Calculate the power of prime for the pattern length
    long long primePow = 1;
    for (size_t i = 0; i < patternLen - 1; ++i) {
        primePow *= prime;
    }
    
    // Calculate initial hash values
    long long patternHash = 0;
    long long textHash = 0;
    
    // Compute initial hash for pattern and first window of text
    for (size_t i = 0; i < patternLen; ++i) {
        patternHash = patternHash * prime + pattern[i];
        textHash = textHash * prime + text[i];
    }
    
    // Slide the pattern over text one by one
    for (size_t i = 0; i <= textLen - patternLen; ++i) {
        // If hash matches, check characters
        if (patternHash == textHash) {
            // Verify characters to handle hash collisions
            bool match = true;
            for (size_t j = 0; j < patternLen; ++j) {
                if (text[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            
            // If all characters match, add to matches
            if (match) {
                matches.push_back(i);
            }
        }
        
        // Calculate hash for next window
        if (i < textLen - patternLen) {
            // Remove leading digit, add trailing digit
            textHash = (textHash - text[i] * primePow) * prime + text[i + patternLen];
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
    
    // Rabin-Karp search function inlined for performance
    auto rabinKarpSearch = [](const std::string& pattern, const std::string& text) -> bool {
        if (pattern.empty() || text.empty() || pattern.length() > text.length()) {
            return false;
        }
        
        // Prime number for hash calculation
        const int prime = 101;
        
        // Calculate the power of prime for the pattern length
        long long primePow = 1;
        for (size_t i = 0; i < pattern.length() - 1; ++i) {
            primePow *= prime;
        }
        
        // Calculate initial hash values
        long long patternHash = 0;
        long long textHash = 0;
        
        // Compute initial hash for pattern and first window of text
        for (size_t i = 0; i < pattern.length(); ++i) {
            patternHash = patternHash * prime + pattern[i];
            textHash = textHash * prime + text[i];
        }
        
        // Slide the pattern over text one by one
        for (size_t i = 0; i <= text.length() - pattern.length(); ++i) {
            // If hash matches, check characters
            if (patternHash == textHash) {
                // Verify characters to handle hash collisions
                bool match = true;
                for (size_t j = 0; j < pattern.length(); ++j) {
                    if (text[i + j] != pattern[j]) {
                        match = false;
                        break;
                    }
                }
                
                // If all characters match, return true
                if (match) {
                    return true;
                }
            }
            
            // Calculate hash for next window
            if (i < text.length() - pattern.length()) {
                // Remove leading digit, add trailing digit
                textHash = (textHash - text[i] * primePow) * prime + text[i + pattern.length()];
            }
        }
        
        return false;
    };
    
    auto filterTask = [&](size_t start, size_t end) {
        std::vector<std::string> localFilteredFiles;
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];
            std::string fileName = file;
            toLowerInPlace(fileName);
            
            bool matchFound = false;
            for (const std::string& queryToken : queryTokens) {
                if (rabinKarpSearch(queryToken, fileName)) {
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
