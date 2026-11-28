// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import isocmd.core.globalMutexes;
import isocmd.core.lists;
import isocmd.core.threadCaps;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.display;

namespace {

	/** @brief Tests whether a byte is an ASCII decimal digit. */
	constexpr bool isDecimalDigit(char c) noexcept {

		return static_cast<unsigned int>(static_cast<unsigned char>(c)) - '0' < 10;

	}

} // namespace

/**
 * @brief Compares strings in natural order without allocating temporary storage.
 *
 * Equal non-digit bytes bypass case folding. Decimal runs are compared by
 * significant length, digit contents and finally leading-zero count. For
 * unequal bytes outside paired decimal runs, case folding and the original-case
 * tie-break retain the existing ordering at the first differing character.
 */
int naturalCompare(std::string_view a, std::string_view b) {

	std::size_t i = 0, j = 0;
	const std::size_t size_a = a.size(), size_b = b.size();

	while (i < size_a && j < size_b) {
		const char ca = a[i], cb = b[j];
		if (isDecimalDigit(ca) && isDecimalDigit(cb)) {
			const std::size_t begin_a = i, begin_b = j;
			while (i < size_a && a[i] == '0') ++i;
			while (j < size_b && b[j] == '0') ++j;
			const std::size_t zeros_a = i - begin_a, zeros_b = j - begin_b;

			// Scan both significant runs together, recording the first differing
			// digit. Run length takes precedence over that difference.
			int digit_cmp = 0;
			while (i < size_a && j < size_b &&
			       isDecimalDigit(a[i]) && isDecimalDigit(b[j])) {
				if (digit_cmp == 0 && a[i] != b[j])
				    digit_cmp = a[i] < b[j] ? -1 : 1;
				++i;
				++j;
			}
			if (i < size_a && isDecimalDigit(a[i])) return 1;
			if (j < size_b && isDecimalDigit(b[j])) return -1;
			if (digit_cmp != 0) return digit_cmp;
			if (zeros_a != zeros_b) return zeros_a < zeros_b ? -1 : 1;
		} else {
			if (ca != cb) {
				const char folded_a = static_cast<char>(std::tolower(static_cast<unsigned char>(ca)));
				const char folded_b = static_cast<char>(std::tolower(static_cast<unsigned char>(cb)));
				if (folded_a != folded_b) return folded_a < folded_b ? -1 : 1;
				return ca < cb ? -1 : 1;
			}
			++i;
			++j;
		}
	}

	return i < size_a ? 1 : j < size_b ? -1 : 0;

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
 *   @c GlobalThreadCaps::SORT_THREAD_CAP.
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

	const std::size_t numThreads =
	    std::min(pool.threadCount(),
	             GlobalThreadCaps::SORT_THREAD_CAP);

	const std::size_t n = files.size();

	const std::size_t numChunks =
	    std::min<std::size_t>(numThreads * 2, n / 1000 + 1);

	const std::size_t chunkSize =
	    (n + numChunks - 1) / numChunks;

	auto comparator = [namesOnly](const std::string& a,
	                              const std::string& b) {

		if (namesOnly) {
			const std::size_t a_slash = a.find_last_of('/');
			const std::size_t b_slash = b.find_last_of('/');

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

	std::vector<std::pair<std::size_t, std::size_t>> chunks;
	std::vector<std::future<void>> futures;

	for (std::size_t i = 0; i < numChunks; ++i) {
		const std::size_t start = i * chunkSize;
		const std::size_t end = std::min(n, (i + 1) * chunkSize);

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
		std::vector<std::pair<std::size_t, std::size_t>> newChunks;
		std::vector<std::future<void>> mergeFutures;

		for (std::size_t i = 0; i < chunks.size(); i += 2) {
			if (i + 1 >= chunks.size()) {
				newChunks.push_back(chunks[i]);
				break;
			}

			const std::size_t start = chunks[i].first;
			const std::size_t mid   = chunks[i].second;
			const std::size_t end   = chunks[i + 1].second;

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
