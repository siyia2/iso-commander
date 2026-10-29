// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Project Headers
#include "../concurrency.h"
#include "../display.h"
#include "../globalMutexes.h"
#include "../state.h"
#include "../threadpool.h"

/**
 * @brief Compares two strings using case-insensitive natural ordering.
 *
 * Compares the supplied string views using a human-friendly "natural" ordering,
 * where embedded numeric sequences are compared according to their numeric value
 * rather than lexicographically.
 *
 * For example:
 * @code
 * "file2.txt"  < "file10.txt"
 * "file9.txt"  < "file10.txt"
 * @endcode
 *
 * Alphabetic characters are compared case-insensitively. When two characters
 * compare equal after ASCII case folding but differ in case, the original
 * character case is retained as a deferred tie-breaker. This ensures that
 * strings which are otherwise identical under case-insensitive comparison still
 * have a deterministic ordering.
 *
 * Numeric sequences are compared by their number of significant digits and then
 * by their digit contents. Leading zeros are ignored when determining numeric
 * value, but are retained as a deferred tie-breaker. Consequently, numerically
 * equivalent values are ordered according to their number of leading zeros:
 *
 * @code
 * "007" < "07" < "7"
 * @endcode
 *
 * The comparison operates directly on std::string_view objects and does not
 * allocate memory or construct temporary strings.
 *
 * @param a First string view to compare.
 * @param b Second string view to compare.
 *
 * @return A negative value if @p a sorts before @p b.
 * @return A positive value if @p a sorts after @p b.
 * @return 0 if @p a and @p b are equivalent according to all comparison rules.
 *
 * @note Character case folding is ASCII-only. Non-ASCII characters are compared
 *       according to their original byte values.
 */
int naturalCompare(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    const size_t size_a = a.size();
    const size_t size_b = b.size();

    while (i < size_a && j < size_b) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) &&
            std::isdigit(static_cast<unsigned char>(b[j]))) {

            size_t start_a = i, start_b = j;

            while (start_a < size_a && a[start_a] == '0')
                ++start_a;

            while (start_b < size_b && b[start_b] == '0')
                ++start_b;

            size_t end_a = start_a, end_b = start_b;

            while (end_a < size_a &&
                   std::isdigit(static_cast<unsigned char>(a[end_a])))
                ++end_a;

            while (end_b < size_b &&
                   std::isdigit(static_cast<unsigned char>(b[end_b])))
                ++end_b;

            const size_t nz_len_a = end_a - start_a;
            const size_t nz_len_b = end_b - start_b;

            if (nz_len_a != nz_len_b)
                return (nz_len_a < nz_len_b) ? -1 : 1;

            for (size_t k = 0; k < nz_len_a; ++k) {
                const char ca = a[start_a + k];
                const char cb = b[start_b + k];

                if (ca != cb)
                    return (ca < cb) ? -1 : 1;
            }

            const size_t zeros_a = start_a - i;
            const size_t zeros_b = start_b - j;

            if (zeros_a != zeros_b)
                return (zeros_a < zeros_b) ? -1 : 1;

            i = end_a;
            j = end_b;
        } else {
            const char ca = static_cast<char>(
                std::tolower(static_cast<unsigned char>(a[i])));

            const char cb = static_cast<char>(
                std::tolower(static_cast<unsigned char>(b[j])));

            if (ca != cb)
                return (ca < cb) ? -1 : 1;

            if (a[i] != b[j])
                return (a[i] < b[i]) ? -1 : 1;

            ++i;
            ++j;
        }
    }

    if (i < size_a)
        return 1;

    if (j < size_b)
        return -1;

    return 0;
}

/**
 * @brief Sorts file paths using case-insensitive natural ordering.
 *
 * Sorts the supplied file paths using a parallel divide-and-conquer merge-sort
 * strategy. Natural ordering is used so that embedded numeric portions are
 * compared by numeric value rather than lexicographically.
 *
 * When @c displayConfig::toggleNamesOnly is enabled, only the filename component
 * after the final '/' is used for comparison. The original path strings are not
 * modified.
 *
 * The sorting operation:
 * - Divides the input into multiple independently sortable chunks.
 * - Sorts the chunks concurrently using the application's static ThreadPool.
 * - Merges adjacent sorted chunks in parallel over multiple passes.
 * - Limits the number of concurrently used sorting threads through
 *   @c GlobalConcurrency::SORT_THREAD_CAP.
 *
 * The comparator uses std::string_view when extracting filename components,
 * avoiding allocations and copies when @p namesOnly is enabled. Path
 * manipulation is therefore performed without constructing temporary strings.
 *
 * @param files Vector of file paths to sort. The vector is reordered in place.
 *
 * @note The function returns immediately when @p files is empty.
 *
 * @note The sorting algorithm uses the application's shared static ThreadPool
 *       rather than creating dedicated worker threads.
 *
 * @note Parallel sorting is particularly beneficial for larger collections.
 *       The number of chunks is scaled according to the input size and available
 *       sorting threads.
 *
 * @note The function does not allocate memory for filename substrings. The
 *       std::string_view objects refer directly to the existing strings in
 *       @p files.
 *
 * @warning The strings referenced by any temporary std::string_view must not be
 *          modified or destroyed while the corresponding comparison is active.
 *          This requirement is satisfied internally because the views are used
 *          only during individual comparator invocations.
 */
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    if (files.empty())
        return;

    const bool namesOnly = displayConfig::toggleNamesOnly;
    ThreadPool& pool = getStaticThreadPool();

    const size_t numThreads =
        std::min(pool.threadCount(),
                 GlobalConcurrency::SORT_THREAD_CAP);

    const size_t n = files.size();

    const size_t numChunks =
        std::min<size_t>(numThreads * 2, n / 1000 + 1);

    const size_t chunkSize =
        (n + numChunks - 1) / numChunks;

    auto comparator = [namesOnly](const std::string& a,
                                  const std::string& b) {
        if (namesOnly) {
            const size_t a_slash = a.find_last_of('/');
            const size_t b_slash = b.find_last_of('/');

            std::string_view a_view = a;
            std::string_view b_view = b;

            if (a_slash != std::string::npos)
                a_view.remove_prefix(a_slash + 1);

            if (b_slash != std::string::npos)
                b_view.remove_prefix(b_slash + 1);

            return naturalCompare(a_view, b_view) < 0;
        }

        return naturalCompare(a, b) < 0;
    };

    std::vector<std::pair<size_t, size_t>> chunks;
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < numChunks; ++i) {
        const size_t start = i * chunkSize;
        const size_t end = std::min(n, (i + 1) * chunkSize);

        if (start >= end)
            break;

        chunks.emplace_back(start, end);

        futures.emplace_back(
            pool.enqueue([comparator, start, end, &files]() {
                std::sort(files.begin() + start,
                          files.begin() + end,
                          comparator);
            }));
    }

    for (auto& future : futures)
        future.get();

    futures.clear();

    while (chunks.size() > 1) {
        std::vector<std::pair<size_t, size_t>> newChunks;
        std::vector<std::future<void>> mergeFutures;

        for (size_t i = 0; i < chunks.size(); i += 2) {
            if (i + 1 >= chunks.size()) {
                newChunks.push_back(chunks[i]);
                break;
            }

            const size_t start = chunks[i].first;
            const size_t mid   = chunks[i].second;
            const size_t end   = chunks[i + 1].second;

            mergeFutures.emplace_back(
                pool.enqueue([comparator, start, mid, end, &files]() {
                    std::inplace_merge(files.begin() + start,
                                       files.begin() + mid,
                                       files.begin() + end,
                                       comparator);
                }));

            newChunks.emplace_back(start, end);
        }

        for (auto& future : mergeFutures)
            future.get();

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
    workers.emplace_back(sortJob, std::ref(GlobalState::globalIsoFileList), std::ref(GlobalMutexes::updateListMutex));
    workers.emplace_back(sortJob, std::ref(GlobalState::binImgFilesCache),  std::ref(GlobalMutexes::binImgCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalState::mdfMdsFilesCache),  std::ref(GlobalMutexes::mdfMdsCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalState::nrgFilesCache),     std::ref(GlobalMutexes::nrgCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalState::chdFilesCache),     std::ref(GlobalMutexes::chdCacheMutex));
    workers.emplace_back(sortJob, std::ref(GlobalState::daaGbiFilesCache),  std::ref(GlobalMutexes::daaGbiCacheMutex));

    // Block here until every single thread is finished to ensure sorting correctness for large lists
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}
