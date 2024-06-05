#include "../headers.h"
#include "../threadpool.h"


// Boyer-Moore string search implementation for mount
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text) {
    std::vector<size_t> shifts(256, pattern.length());
    std::vector<size_t> matches;

    for (size_t i = 0; i < pattern.length() - 1; i++) {
        shifts[pattern[i]] = pattern.length() - i - 1;
    }

    size_t patternLen = pattern.length();
    size_t textLen = text.length();
    size_t skip = 0;

    for (size_t i = patternLen - 1; i < textLen;) {
        for (skip = 0; skip < patternLen; skip++) {
            if (pattern[patternLen - 1 - skip] != text[i - skip]) {
                break;
            }
        }

        if (skip == patternLen) {
            matches.push_back(i - patternLen + 1);
        }

        i += std::max(shifts[text[i]], patternLen - skip);
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
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        queryTokens.insert(token);
    }

    ThreadPool pool(maxThreads);
    std::vector<std::future<void>> futures;
    auto filterTask = [&](size_t start, size_t end) {
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];
            size_t lastSlashPos = file.find_last_of('/');
            std::string fileName = (lastSlashPos != std::string::npos) ? file.substr(lastSlashPos + 1) : file;
            std::transform(fileName.begin(), fileName.end(), fileName.begin(), ::tolower);
            bool matchFound = false;
            for (const std::string& queryToken : queryTokens) {
                if (!boyerMooreSearch(queryToken, fileName).empty()) {
                    matchFound = true;
                    break;
                }
            }
            if (matchFound) {
                std::lock_guard<std::mutex> lock(Mutex4Med);
                filteredFiles.push_back(file);
            }
        }
    };

    size_t numFiles = files.size();
    size_t numThreads = maxThreads;
    size_t filesPerThread = numFiles / numThreads;
    for (size_t i = 0; i < numThreads - 1; ++i) {
        size_t start = i * filesPerThread;
        size_t end = start + filesPerThread;
        futures.emplace_back(pool.enqueue(filterTask, start, end));
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
void filterMountPoints(const std::vector<std::string>& isoDirs, std::set<std::string>& filterPatterns, std::vector<std::string>& filteredIsoDirs, std::mutex& resultMutex, size_t start, size_t end) {
    // Iterate through the chunk of ISO directories
    for (size_t i = start; i < end; ++i) {
        const std::string& dir = isoDirs[i];
        std::string dirLower = dir;
        std::transform(dirLower.begin(), dirLower.end(), dirLower.begin(), ::tolower);

        // Flag to track if a match is found for the directory
        bool matchFound = false;
        // Iterate through each filter pattern
        for (const std::string& pattern : filterPatterns) {
            // If the directory matches the current filter pattern using Boyer-Moore search
            if (boyerMooreSearchMountPoints(dirLower, pattern) != std::string::npos) {
                matchFound = true;
                break;
            }
        }

        // If a match is found, add the directory to the filtered list
        if (matchFound) {
            // Lock access to the shared vector
            std::lock_guard<std::mutex> lock(resultMutex);
            filteredIsoDirs.push_back(dir);
        }
    }
}
