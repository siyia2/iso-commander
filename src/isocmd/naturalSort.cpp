// SPDX-License-Identifier: GPL-2.0-or-later

#include "../headers.h"
#include "../threadpool.h"


// Compare two strings in natural order, case-insensitively
int naturalCompare(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(a[i]) && std::isdigit(b[j])) {
            // Skip leading zeros and compute lengths
            size_t start_a = i, start_b = j;
            while (start_a < a.size() && a[start_a] == '0') start_a++;
            while (start_b < b.size() && b[start_b] == '0') start_b++;
            
            // Compute total digit lengths
            size_t len_a = 0, len_b = 0;
            while (i + len_a < a.size() && std::isdigit(a[i + len_a])) len_a++;
            while (j + len_b < b.size() && std::isdigit(b[j + len_b])) len_b++;
            
            // Non-zero lengths via subtraction (optimization)
            size_t nz_len_a = len_a - (start_a - i);
            size_t nz_len_b = len_b - (start_b - j);
            
            // Compare non-zero lengths
            if (nz_len_a != nz_len_b)
                return (nz_len_a < nz_len_b) ? -1 : 1;
            
            // Compare digit by digit if lengths match
            for (size_t k = 0; k < nz_len_a; ++k) {
                char ca = (start_a + k < a.size()) ? a[start_a + k] : '0';
                char cb = (start_b + k < b.size()) ? b[start_b + k] : '0';
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
            
            // Compare leading zeros if digits are equal
            size_t zeros_a = start_a - i;
            size_t zeros_b = start_b - j;
            if (zeros_a != zeros_b)
                return (zeros_a < zeros_b) ? -1 : 1;
            
            i += len_a;
            j += len_b;
        } else {
            // Case-insensitive compare for non-digits
            char ca = std::tolower(a[i]), cb = std::tolower(b[j]);
            if (ca != cb) return (ca < cb) ? -1 : 1;
            ++i; ++j;
        }
    }
    return (i < a.size()) ? 1 : (j < b.size()) ? -1 : 0;
}


// Sort files using a natural order, case-insensitive comparator
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    if (files.empty())
        return;
    ThreadPool pool(maxThreads);
    const size_t n = files.size();
    // Determine optimal chunk size - this should be tuned based on your typical workload
    unsigned int numChunks = std::min<unsigned int>(maxThreads * 2, static_cast<unsigned int>(n / 1000 + 1));
    size_t chunkSize = (n + numChunks - 1) / numChunks;
    
    // Each pair holds the start and end indices of a sorted chunk
    std::vector<std::pair<size_t, size_t>> chunks;
    
    // Futures to wait for each sorting task
    std::vector<std::future<void>> futures;
    
    // Launch parallel sorting tasks
    for (size_t i = 0; i < numChunks; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(n, (i + 1) * chunkSize);
        if (start >= end)
            break;
            
        // Record this chunk's range
        chunks.emplace_back(start, end);
        
        // Enqueue the sorting task to the thread pool
        futures.emplace_back(pool.enqueue([start, end, &files]() {
            std::sort(files.begin() + start, files.begin() + end, [](const std::string& a, const std::string& b) {
                return naturalCompare(a, b) < 0;
            });
        }));
    }
    
    // Wait for all sorting tasks to complete
    for (auto& f : futures)
        f.get();
    
    // Merge sorted chunks pairwise until the entire vector is merged
    while (chunks.size() > 1) {
        std::vector<std::pair<size_t, size_t>> newChunks;
        std::vector<std::future<void>> mergeFutures;
        
        for (size_t i = 0; i < chunks.size(); i += 2) {
            if (i + 1 < chunks.size()) {
                size_t start = chunks[i].first;
                size_t mid = chunks[i + 1].first;
                size_t end = chunks[i + 1].second;
                
                // We could also parallelize the merging for large chunks
                mergeFutures.emplace_back(pool.enqueue([start, mid, end, &files]() {
                    std::inplace_merge(files.begin() + start, files.begin() + mid, files.begin() + end,
                        [](const std::string& a, const std::string& b) {
                            return naturalCompare(a, b) < 0;
                        });
                }));
                
                newChunks.emplace_back(start, end);
            } else {
                // Odd number of chunks: last one remains as is
                newChunks.push_back(chunks[i]);
            }
        }
        
        // Wait for all merges at this level to complete
        for (auto& f : mergeFutures)
            f.get();
            
        chunks = std::move(newChunks);
    }
}
