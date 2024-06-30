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


// Boyer-Moore string search implementation for mount
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text) {
    // Precompute lowercase pattern for case-insensitive comparison
    std::string lowerPattern = pattern;
    std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::vector<size_t> matches;
    
    size_t patternLen = pattern.length();
    size_t textLen = text.length();

    // Compute the bad character shift table
    std::vector<size_t> shifts(256, patternLen);
    for (size_t i = 0; i < patternLen - 1; ++i) {
        shifts[static_cast<unsigned char>(tolower(pattern[i]))] = patternLen - i - 1;
    }

    size_t i = 0; // Start at the beginning of the text

    while (i <= textLen - patternLen) {
        int j = patternLen - 1;
        
        // Match pattern from end to start
        while (j >= 0 && tolower(text[i + j]) == lowerPattern[j]) {
            --j;
        }
        
        // If the whole pattern was found, record the match
        if (j < 0) {
            matches.push_back(i);
        }

        // Move the pattern based on the last character of the current window in the text
        i += shifts[static_cast<unsigned char>(tolower(text[i + patternLen - 1]))];
    }

    return matches;
}


// Function to filter cached ISO files based on search query (case-insensitive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    std::vector<std::string> filteredFiles;  // Resultant vector of filtered files
    std::set<std::string> queryTokens;  // Set to store unique query tokens

    // Tokenize query by ';' and convert tokens to lowercase
    std::stringstream ss(query);
    std::string token;
    while (std::getline(ss, token, ';')) {
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        queryTokens.insert(token);
    }

    std::shared_mutex filterMutex;  // Shared mutex for thread-safe access to filteredFiles

    // Task function to filter files within a range [start, end)
    auto filterTask = [&](size_t start, size_t end) {
        std::vector<std::string> localFilteredFiles;  // Local storage for filtered files

        // Iterate through files in the specified range
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];
            std::string lowerFile = file;
            std::transform(lowerFile.begin(), lowerFile.end(), lowerFile.begin(), ::tolower);  // Convert full path to lowercase
            
            // Extract the filename from the path
            std::string fileName = lowerFile.substr(lowerFile.find_last_of("/\\") + 1);
            
            bool matchFound = false;
            // Check each query token against both the lowercase full path and filename using Boyer-Moore search
            for (const std::string& queryToken : queryTokens) {
                if (!boyerMooreSearch(queryToken, lowerFile).empty() || !boyerMooreSearch(queryToken, fileName).empty()) {
                    matchFound = true;
                    break;
                }
            }

            // If any query token matches, add the file to localFilteredFiles
            if (matchFound) {
                localFilteredFiles.push_back(file);
            }
        }

        // Lock to safely update shared filteredFiles vector
        std::unique_lock<std::shared_mutex> lock(filterMutex);
        filteredFiles.insert(filteredFiles.end(), localFilteredFiles.begin(), localFilteredFiles.end());
    };

    size_t numFiles = files.size();
    int maxThreads = std::thread::hardware_concurrency();  // Maximum number of concurrent threads supported
    size_t numThreads = std::min(static_cast<size_t>(maxThreads), numFiles);  // Determine number of threads to use
    size_t filesPerThread = numFiles / numThreads;  // Number of files each thread will process

    std::vector<std::future<void>> futures;  // Vector to hold futures for threads

    // Create async tasks for each thread (except the last one)
    for (size_t i = 0; i < numThreads - 1; ++i) {
        size_t start = i * filesPerThread;
        size_t end = start + filesPerThread;
        futures.emplace_back(std::async(std::launch::async, filterTask, start, end));
    }

    // Handle filtering for the last segment (handles any remaining files)
    filterTask((numThreads - 1) * filesPerThread, numFiles);

    // Wait for all threads to complete
    for (auto& future : futures) {
        future.wait();
    }

    return filteredFiles;  // Return the vector of filtered files
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
            std::unique_lock<std::shared_mutex> lock(filterMutex);
            filteredIsoDirs.push_back(dir);
        }
    }
}
