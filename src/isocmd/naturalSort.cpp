// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// C / System Headers
#include <stddef.h>

// Project Headers
#include "../caches.h"
#include "../concurrency.h"
#include "../display.h"
#include "../threadpool.h"

/**
 * @brief Compares two string views using natural order (e.g., "file2.txt" < "file10.txt").
 * 
 * Performs a case-insensitive natural sort comparison. If strings are alphabetically 
 * and numerically identical, it falls back to a case-sensitive comparison to ensure 
 * a stable, deterministic sort order.
 * 
 * Numeric segments are compared by value, with leading zeros preserved as a
 * tiebreaker (e.g., "007" < "07" < "7" when numeric values are equal).
 * 
 * This version uses std::string_view to provide a zero-copy comparison, making it 
 * highly efficient for large collections or path-based sorting where substrings 
 * are frequently evaluated.
 * 
 * @param a The first string_view to compare.
 * @param b The second string_view to compare.
 * @return int Returns -1 if a < b, 1 if a > b, and 0 if a == b.
 */
int naturalCompare(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    const size_t size_a = a.size();
    const size_t size_b = b.size();
    while (i < size_a && j < size_b) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) &&
            std::isdigit(static_cast<unsigned char>(b[j]))) {

            size_t start_a = i, start_b = j;
            while (start_a < size_a && a[start_a] == '0') ++start_a;
            while (start_b < size_b && b[start_b] == '0') ++start_b;

            size_t end_a = start_a, end_b = start_b;
            while (end_a < size_a && std::isdigit(static_cast<unsigned char>(a[end_a]))) ++end_a;
            while (end_b < size_b && std::isdigit(static_cast<unsigned char>(b[end_b]))) ++end_b;

            size_t nz_len_a = end_a - start_a;
            size_t nz_len_b = end_b - start_b;

            if (nz_len_a != nz_len_b)
                return (nz_len_a < nz_len_b) ? -1 : 1;

            for (size_t k = 0; k < nz_len_a; ++k) {
                char ca = a[start_a + k];
                char cb = b[start_b + k];
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }

            size_t zeros_a = start_a - i;
            size_t zeros_b = start_b - j;
            if (zeros_a != zeros_b)
                return (zeros_a < zeros_b) ? -1 : 1;

            i = end_a;
            j = end_b;
        } else {
            char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));

            if (ca != cb) return (ca < cb) ? -1 : 1;

            if (a[i] != b[j]) return (a[i] < b[j]) ? -1 : 1;

            ++i; ++j;
        }
    }
    if (i < size_a) return 1;
    if (j < size_b) return -1;

    return 0;
}

/**
 * @brief Sorts a vector of file paths in natural order using a parallel merge sort approach.
 * 
 * The function divides the file list into chunks, sorts them in parallel via a ThreadPool, 
 * and performs a multi-pass merge of the sorted results. It utilizes zero-copy 
 * string_views to handle the `displayConfig::toggleNamesOnly` logic, ensuring 
 * high performance without redundant memory allocations.
 * 
 * @note Parallelism is capped by `GlobalConcurrency::SORT_THREAD_CAP` to prevent 
 *       resource exhaustion.
 * @param files A reference to the vector of strings (file paths) to be sorted.
 */
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    if (files.empty())
        return;
    bool namesOnly = displayConfig::toggleNamesOnly;
    ThreadPool& pool = getStaticThreadPool();
    const size_t numThreads = std::min(pool.threadCount(), GlobalConcurrency::SORT_THREAD_CAP);
    const size_t n = files.size();
    size_t numChunks = std::min<size_t>(numThreads * 2, n / 1000 + 1);
    size_t chunkSize = (n + numChunks - 1) / numChunks;

    auto comparator = [namesOnly](const std::string& a, const std::string& b) {
        if (namesOnly) {
            size_t a_slash = a.find_last_of('/');
            size_t b_slash = b.find_last_of('/');
            std::string_view a_view = a;
            if (a_slash != std::string::npos) a_view.remove_prefix(a_slash + 1);
            std::string_view b_view = b;
            if (b_slash != std::string::npos) b_view.remove_prefix(b_slash + 1);
            return naturalCompare(a_view, b_view) < 0;
        }
        return naturalCompare(a, b) < 0;
    };

    std::vector<std::pair<size_t, size_t>> chunks;
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < numChunks; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(n, (i + 1) * chunkSize);
        if (start >= end) break;
        chunks.emplace_back(start, end);
        futures.emplace_back(pool.enqueue([comparator, start, end, &files]() {
            std::sort(files.begin() + start, files.begin() + end, comparator);
        }));
    }
    for (auto& f : futures) f.get();
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
            mergeFutures.emplace_back(pool.enqueue([comparator, start, mid, end, &files]() {
                std::inplace_merge(files.begin() + start, files.begin() + mid, files.begin() + end, comparator);
            }));
            newChunks.emplace_back(start, end);
        }
        for (auto& f : mergeFutures) f.get();
        chunks = std::move(newChunks);
    }
}

/**
 * @brief Triggered when the 'filenamesOnly' flag is toggled.
 * 
 * Re-sorts all global file caches simultaneously in parallel. This function 
 * blocks the calling thread until all caches are fully sorted, ensuring 
 * data consistency before the UI refreshes.
 */
void sortAfterFilenamesOnlyFlag() {
    auto sortJob = [](std::vector<std::string>& list, std::mutex& mtx) {
        std::lock_guard<std::mutex> lock(mtx);
        sortFilesCaseInsensitive(list);
    };

    std::vector<std::thread> workers;
    workers.reserve(6);

    // Launch all 6 sorts at the same time
    workers.emplace_back(sortJob, std::ref(GlobalCaches::globalIsoFileList), std::ref(GlobalCaches::updateListMutex));
    workers.emplace_back(sortJob, std::ref(GlobalCaches::binImgFilesCache),  std::ref(GlobalCaches::binImgCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalCaches::mdfMdsFilesCache),  std::ref(GlobalCaches::mdfMdsCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalCaches::nrgFilesCache),     std::ref(GlobalCaches::nrgCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalCaches::chdFilesCache),     std::ref(GlobalCaches::chdCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalCaches::daaGbiFilesCache),  std::ref(GlobalCaches::daaGbiCacheMutex));

    // Block here until every single thread is finished to ensure sorting correctness for large lists
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}
