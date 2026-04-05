// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../threadpool.h"

/**
 * @brief Compares two strings using natural order (e.g., "file2.txt" < "file10.txt").
 * * This comparison is case-insensitive but falls back to case-sensitive comparison
 * if the strings are otherwise identical. It correctly handles numeric sequences 
 * within strings by comparing their numerical value rather than their ASCII codes.
 * * @param a The first string to compare.
 * @param b The second string to compare.
 * @return int Returns -1 if a < b, 1 if a > b, and 0 if a == b.
 */
int naturalCompare(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(a[i]) && std::isdigit(b[j])) {
            size_t start_a = i, start_b = j;
            while (start_a < a.size() && a[start_a] == '0') start_a++;
            while (start_b < b.size() && b[start_b] == '0') start_b++;
            
            size_t len_a = 0, len_b = 0;
            while (i + len_a < a.size() && std::isdigit(a[i + len_a])) len_a++;
            while (j + len_b < b.size() && std::isdigit(b[j + len_b])) len_b++;
            
            size_t nz_len_a = len_a - (start_a - i);
            size_t nz_len_b = len_b - (start_b - j);
            
            if (nz_len_a != nz_len_b)
                return (nz_len_a < nz_len_b) ? -1 : 1;
            
            for (size_t k = 0; k < nz_len_a; ++k) {
                char ca = (start_a + k < a.size()) ? a[start_a + k] : '0';
                char cb = (start_b + k < b.size()) ? b[start_b + k] : '0';
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
            
            size_t zeros_a = start_a - i;
            size_t zeros_b = start_b - j;
            if (zeros_a != zeros_b)
                return (zeros_a < zeros_b) ? -1 : 1;
            
            i += len_a;
            j += len_b;
        } else {
            char ca = std::tolower(a[i]), cb = std::tolower(b[j]);
            if (ca != cb) return (ca < cb) ? -1 : 1;
            
            if (a[i] != b[j]) return (a[i] < b[j]) ? -1 : 1;
            
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    
    return 0;
}

/**
 * @brief Sorts a vector of file paths in natural order using a parallel merge sort approach.
 * * The function divides the file list into chunks, sorts them in parallel using a thread pool,
 * and then merges the sorted chunks. It respects the `displayConfig::toggleNamesOnly` setting
 * to decide whether to sort by full path or just the filename.
 * * @note Parallelism is capped by `SORT_THREAD_CAP`.
 * @param files A reference to the vector of strings (file paths) to be sorted.
 */
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    if (files.empty())
        return;

    bool namesOnly = displayConfig::toggleNamesOnly;
    
    ThreadPool& pool = getStaticThreadPool();
    const size_t numThreads = std::min(pool.threadCount(), SORT_THREAD_CAP);
    
    const size_t n = files.size();
    size_t numChunks = std::min<size_t>(numThreads * 2, n / 1000 + 1);
    size_t chunkSize = (n + numChunks - 1) / numChunks;

    std::vector<std::pair<size_t, size_t>> chunks;
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < numChunks; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(n, (i + 1) * chunkSize);
        if (start >= end)
            break;

        chunks.emplace_back(start, end);

        futures.emplace_back(pool.enqueue([namesOnly, start, end, &files]() {
            std::sort(files.begin() + start, files.begin() + end,
                [namesOnly](const std::string& a, const std::string& b) {
                    if (namesOnly) {
                        size_t a_slash = a.find_last_of('/');
                        size_t b_slash = b.find_last_of('/');
                        std::string a_name = (a_slash == std::string::npos) ? a : a.substr(a_slash + 1);
                        std::string b_name = (b_slash == std::string::npos) ? b : b.substr(b_slash + 1);
                        return naturalCompare(a_name, b_name) < 0;
                    } else {
                        return naturalCompare(a, b) < 0;
                    }
                });
        }));
    }

    for (auto& f : futures)
        f.get();
    futures.clear();

    while (chunks.size() > 1) {
        std::vector<std::pair<size_t, size_t>> newChunks;
        std::vector<std::future<void>> mergeFutures;

        for (size_t i = 0; i < chunks.size(); i += 2) {
            if (i + 1 >= chunks.size()) {
                newChunks.push_back(chunks[i]);
                break;
            }

            size_t start = chunks[i].first;
            size_t mid   = chunks[i].second;
            size_t end   = chunks[i + 1].second;

            mergeFutures.emplace_back(pool.enqueue([namesOnly, start, mid, end, &files]() {
                std::inplace_merge(files.begin() + start, files.begin() + mid, files.begin() + end,
                    [namesOnly](const std::string& a, const std::string& b) {
                        if (namesOnly) {
                            size_t a_slash = a.find_last_of('/');
                            size_t b_slash = b.find_last_of('/');
                            std::string a_name = (a_slash == std::string::npos) ? a : a.substr(a_slash + 1);
                            std::string b_name = (b_slash == std::string::npos) ? b : b.substr(b_slash + 1);
                            return naturalCompare(a_name, b_name) < 0;
                        } else {
                            return naturalCompare(a, b) < 0;
                        }
                    });
            }));

            newChunks.emplace_back(start, end);
        }

        for (auto& f : mergeFutures)
            f.get();

        chunks = std::move(newChunks);
    }
}
