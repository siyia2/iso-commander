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
    // Helper lambda to convert a string to lowercase
    auto toLower = [](const std::string& str) {
        std::string lowerStr = str;
        toLowerInPlace(lowerStr);
        return lowerStr;
    };

    std::string lowerPattern = toLower(pattern);
    std::string lowerText = toLower(text);

    std::vector<size_t> shifts(256, lowerPattern.length());
    std::vector<size_t> matches;

    for (size_t i = 0; i < lowerPattern.length() - 1; i++) {
        shifts[static_cast<unsigned char>(lowerPattern[i])] = lowerPattern.length() - i - 1;
    }

    size_t patternLen = lowerPattern.length();
    size_t textLen = lowerText.length();

    size_t i = 0; // Start at the beginning of the text

    while (i <= textLen - patternLen) {
        size_t skip = 0;
        
        // Match pattern from end to start
        while (skip < patternLen && lowerPattern[patternLen - 1 - skip] == lowerText[i + patternLen - 1 - skip]) {
            skip++;
        }
        
        // If the whole pattern was found, record the match
        if (skip == patternLen) {
            matches.push_back(i);
        }

        // Move the pattern based on the last character of the current window in the text
        if (i + patternLen < textLen) {
            i += shifts[static_cast<unsigned char>(lowerText[i + patternLen - 1])];
        } else {
            break;
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
		queryTokens.insert(token);
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


// Boyer-Moore string search implementation for umount
size_t boyerMooreSearchMountPoints(const std::string& haystack, const std::string& needle) {
    size_t m = needle.length();
    size_t n = haystack.length();
    if (m == 0) return 0;
    if (n == 0 || n < m) return std::string::npos;

    // Construct the bad character heuristic table
    std::vector<int> badChar(256, -1);
    for (size_t i = 0; i < m; ++i) {
        badChar[needle[i]] = i;
    }

    // Start searching
    size_t shift = 0;
    while (shift <= n - m) {
        int j = m - 1;
        while (j >= 0 && needle[j] == haystack[shift + j]) {
            j--;
        }
        if (j < 0) {
            return shift; // Match found
        } else {
            shift += std::max(1, static_cast<int>(j - badChar[haystack[shift + j]]));
        }
    }
    return std::string::npos; // No match found
}


// Function to filter mounted isoDirs using Boyer-Moore search
void filterMountPoints(const std::vector<std::string>& isoDirs, std::vector<std::string>& filterPatterns, std::vector<std::string>& filteredIsoDirs, size_t start, size_t end) {
std::shared_mutex filterMutex;  // Shared mutex for thread-safe access to filteredFiles
    std::vector<std::string> localFiltered;
    for (size_t i = start; i < end; ++i) {
        const std::string& dir = isoDirs[i];
        std::string dirLower = dir;
        toLowerInPlace(dirLower);
        bool matchFound = false;
        for (const std::string& pattern : filterPatterns) {
            if (boyerMooreSearchMountPoints(dirLower, pattern) != std::string::npos) {
                matchFound = true;
                break;
            }
        }
        if (matchFound) {
            localFiltered.push_back(dir);
        }
    }
    std::unique_lock<std::shared_mutex> lock(filterMutex);
    filteredIsoDirs.insert(filteredIsoDirs.end(), localFiltered.begin(), localFiltered.end());
}
