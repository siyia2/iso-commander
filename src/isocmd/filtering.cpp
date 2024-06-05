#include "../headers.h"
#include "../threadpool.h"


// Function to filter cached ISO files based on search query (case-insensitive)
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query) {
    // Vector to store filtered file names
    std::vector<std::string> filteredFiles;

    // Set to store query tokens (lowercased)
    std::set<std::string> queryTokens;

    // Split the query string into tokens using the delimiter ';' and store them in a set
    std::stringstream ss(query);
    std::string token;
    while (std::getline(ss, token, ';')) {
        // Convert token to lowercase
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        // Insert token into the set
        queryTokens.insert(token);
    }

    // Create a thread pool with the desired number of threads
    ThreadPool pool(maxThreads);

    // Vector to store futures for tracking tasks' completion
    std::vector<std::future<void>> futures;

    // Function to filter files
    auto filterTask = [&](size_t start, size_t end) {
        for (size_t i = start; i < end; ++i) {
            const std::string& file = files[i];

            // Find the position of the last '/' character to extract file name
            size_t lastSlashPos = file.find_last_of('/');
            // Extract file name (excluding path) or use the full path if no '/' is found
            std::string fileName = (lastSlashPos != std::string::npos) ? file.substr(lastSlashPos + 1) : file;
            // Convert file name to lowercase
            std::transform(fileName.begin(), fileName.end(), fileName.begin(), ::tolower);

            // Flag to track if a match is found for the file
            bool matchFound = false;
            // Iterate through each query token
            for (const std::string& queryToken : queryTokens) {
                // If the file name contains the current query token
                if (fileName.find(queryToken) != std::string::npos) {
                    // Set matchFound flag to true and break out of the loop
                    matchFound = true;
                    break;
                }
            }

            // If a match is found, add the file to the filtered list
            if (matchFound) {
                // Lock access to the shared vector
                std::lock_guard<std::mutex> lock(Mutex4Med);
                filteredFiles.push_back(file);
            }
        }
    };

    // Calculate the number of files per thread
    size_t numFiles = files.size();
    size_t numThreads = maxThreads;
    size_t filesPerThread = numFiles / numThreads;

    // Enqueue filter tasks into the thread pool
    for (size_t i = 0; i < numThreads - 1; ++i) {
        size_t start = i * filesPerThread;
        size_t end = start + filesPerThread;
        futures.emplace_back(pool.enqueue(filterTask, start, end));
    }

    // Handle the remaining files in the main thread
    filterTask((numThreads - 1) * filesPerThread, numFiles);

    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.wait();
    }

    // Return the vector of filtered file names
    return filteredFiles;
}


// Function to filter mounted isoDirs
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
            // If the directory matches the current filter pattern
            if (dirLower.find(pattern) != std::string::npos) {
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
