// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <signal.h>
#include <sys/stat.h>

import std;
import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.inputHandling;
import isocmd.input.tokenize;
import isocmd.ops.cpMvRm;
import isocmd.proc.process;
import isocmd.select.select;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

/**
 * @brief High-level processing functions for copy, move and remove operations.
 */

/**
 * @brief Groups file indices into chunks for parallel processing, preventing
 *        race conditions by grouping identical filenames.
 *
 * When @p isDelete is false, indices whose files share a base filename are
 * kept in the same chunk so concurrent workers cannot race on the same
 * destination name (e.g. two copies into the same directory that would
 * collide). Remaining unique-name indices are then spread evenly across the
 * leftover threads. When @p isDelete is true, no such collision is possible,
 * so indices are simply round-robined across @p numThreads chunks.
 *
 * @param processedIndices Set of indices selected by the user.
 * @param isoFiles         Master list of file paths.
 * @param numThreads       Desired concurrency level.
 * @param isDelete         True for deletion (skips name-collision grouping).
 * @return A vector of chunks, where each chunk is a vector of indices.
 */
static std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete)
{

	std::vector<int> processedIndicesVector(processedIndices.begin(), processedIndices.end());
	std::vector<std::vector<int>> indexChunks;

	if (processedIndicesVector.empty()) return indexChunks;

	if (!isDelete) {
		std::unordered_map<std::string, std::vector<int>> groups;
		for (int idx : processedIndicesVector) {
			std::string baseName = std::filesystem::path(isoFiles[idx - 1]).filename().string();
			groups[baseName].push_back(idx);
		}

		std::vector<int> uniqueNameFiles;
		for (auto& [baseName, indices] : groups) {
			if (indices.size() > 1) {
				indexChunks.push_back(std::move(indices));
			} else {
				uniqueNameFiles.push_back(indices[0]);
			}
		}

		if (!uniqueNameFiles.empty()) {
			size_t usedChunks = indexChunks.size();
			size_t remainingThreads = (numThreads > usedChunks) ? numThreads - usedChunks : 1;

			std::vector<std::vector<int>> uniqueChunks(remainingThreads);
			for (size_t i = 0; i < uniqueNameFiles.size(); ++i)
			    uniqueChunks[i % remainingThreads].push_back(uniqueNameFiles[i]);

			for (auto& c : uniqueChunks)
			    if (!c.empty()) indexChunks.push_back(std::move(c));
		}
	} else {
		std::vector<std::vector<int>> deleteChunks(numThreads);
		for (size_t i = 0; i < processedIndicesVector.size(); ++i)
		    deleteChunks[i % numThreads].push_back(processedIndicesVector[i]);

		for (auto& c : deleteChunks)
		    if (!c.empty()) indexChunks.push_back(std::move(c));
	}

	return indexChunks;

}

/**
 * @brief Calculates the total byte size of all files contained within the index chunks.
 *
 * Iterates through each chunk of file indices, queries the filesystem using `stat`,
 * and accumulates the file sizes for progress tracking.
 *
 * @param indexChunks Vector of index chunks grouping the file indices to process.
 * @param isoFiles    Master list of files used for index resolution.
 * @return size_t     Total size in bytes of all targeted files.
 */
static size_t calculateTotalBytes(const std::vector<std::vector<int>>& indexChunks,
                                  const std::vector<std::string>& isoFiles) {

	size_t totalBytes = 0;
	for (const auto& chunk : indexChunks) {
		for (int idx : chunk) {
			struct ::stat st;
			if (::stat(isoFiles[idx - 1].c_str(), &st) == 0)
			    totalBytes += st.st_size;
		}
	}
	return totalBytes;

}

/**
 * @brief Generates a color-coded process string for terminal progress display.
 *
 * Associates specific ANSI color codes from the UI palette depending on whether
 * the operation is a deletion, move, or copy.
 *
 * @param process  Operation type string ("cp", "mv", or "rm").
 * @param isDelete Flag indicating if the operation is a deletion.
 * @param isMove   Flag indicating if the operation is a move.
 * @param isCopy   Flag indicating if the operation is a copy.
 * @return std::string Formatted and colored process string.
 */
static std::string getColoredProcess(const std::string& process, bool isDelete, bool isMove, bool isCopy) {

	if (isDelete) return std::string(UI::Palette::Red)    + process + std::string(UI::Palette::BoldReset);
	if (isMove)   return std::string(UI::Palette::Yellow) + process + std::string(UI::Palette::BoldReset);
	if (isCopy)   return std::string(UI::Palette::Green)  + process + std::string(UI::Palette::BoldReset);
	return process;

}

/**
 * @brief Enqueues file operation tasks into the static thread pool and waits
 *        for completion.
 *
 * Distributes the provided @p indexChunks across the pool, invoking
 * `handleIsoFileOperation` for each chunk with the shared @p options and
 * @p progress state, then blocks until every submitted future completes.
 *
 * @param pool        Reference to the static thread pool instance.
 * @param indexChunks Grouped index chunks of files to process.
 * @param options     Immutable operation configuration (paths, flags).
 * @param progress    Shared mutable progress counters and output-path collection.
 */
static void executeFileOperations(ThreadPool& pool,
                                  const std::vector<std::vector<int>>& indexChunks,
                                  const FileOperationOptions& options,
                                  FileOperationProgress& progress) {

	std::vector<std::future<void>> futures;
	futures.reserve(indexChunks.size());

	for (const auto& chunk : indexChunks) {
		futures.emplace_back(pool.enqueue([chunk = chunk, &options, &progress]() {

			handleIsoFileOperation(options, chunk, progress);

		}));
	}

	for (auto& future : futures) {
		future.wait();
	}

}

namespace {

	/**
	 * @brief Computes the target worker thread count for a bulk operation.
	 *
	 * Clamps the requested task count against the operation-specific concurrency cap
	 * and the available thread pool capacity, with a minimum floor of 2 threads.
	 *
	 * @param[in] taskCount Number of selected file indices to process.
	 * @param[in] isDelete  True when executing a `rm` operation; false for `cp` or `mv`.
	 * @param[in] poolSize  Total number of threads available in the shared pool.
	 * @return Worker thread count to allocate for chunking.
	 */
	size_t computeTargetThreadCount(size_t taskCount, bool isDelete, size_t poolSize) {

		const size_t cap = isDelete ? GlobalThreadCaps::RM_THREAD_CAP : GlobalThreadCaps::CPMV_THREAD_CAP;
		return std::max(size_t(2), std::min({taskCount, cap, poolSize}));

	}

	/**
	 * @brief Calculates total workload size in bytes and tasks across destinations.
	 *
	 * Scales base byte and task totals by the number of semicolon-delimited destination
	 * directories when running a copy (`cp`) or move (`mv`) operation.
	 *
	 * @param[in]  indexChunks          Grouped file indices per worker chunk.
	 * @param[in]  isoFiles             Master list of ISO file paths.
	 * @param[in]  processedUserDestDir Semicolon-separated destination directory string.
	 * @param[in]  taskCount            Base count of selected indices.
	 * @param[in]  isCopyOrMove         True for `cp` or `mv`; false for `rm`.
	 * @param[out] totalBytes           Computed total byte size across all tasks and destinations.
	 * @param[out] totalTasks           Computed total task count across all destinations.
	 */
	void computeWorkloadTotals(const std::vector<std::vector<int>>& indexChunks,
	                                  const std::vector<std::string>& isoFiles,
	                                  const std::string& processedUserDestDir,
	                                  size_t taskCount,
	                                  bool isCopyOrMove,
	                                  size_t& totalBytes,
	                                  size_t& totalTasks) {

		totalBytes = calculateTotalBytes(indexChunks, isoFiles);
		totalTasks = taskCount;

		if (isCopyOrMove) {
			const size_t destCount = static_cast<size_t>(
			    std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';')) + 1;
			totalBytes *= destCount;
			totalTasks *= destCount;
		}

	}

	/**
	 * @brief Builds and prints the status header shown before progress tracking starts.
	 *
	 * @param[in] totalTasks     Total number of tasks to be processed.
	 * @param[in] operationColor ANSI color code for the active operation.
	 * @param[in] process        Operation name string ("cp", "mv", or "rm").
	 * @return Formatted header string passed to the progress display thread.
	 */
	std::string printCpMvRmOperationHeader(size_t totalTasks,
	                                              const std::string& operationColor,
	                                              const std::string& process) {

		std::ostringstream hdr;
		hdr << "\n" << color << " Processing "
		    << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process
		    << color << "... ("
		    << UI::Palette::Red << "Ctrl+c"
		    << color << ":cancel)\n";
		const std::string header = hdr.str();
		std::cout << header;
		return header;

	}

	/**
	 * @brief Synchronizes the database with destination paths after non-delete operations.
	 *
	 * Joins non-empty destination paths with `;` and invokes `updateDatabaseAfterOperations`
	 * when at least one task succeeded on a `cp` or `mv` operation.
	 *
	 * @param[in] successfulDestPaths Destination paths produced by completed tasks.
	 * @param[in] completedTaskCount  Count of tasks that completed successfully.
	 * @param[in] isDelete            True for `rm` (skips database update); false otherwise.
	 */
	void syncDatabaseForDestinations(const std::vector<std::string>& successfulDestPaths,
	                                        size_t completedTaskCount,
	                                        bool isDelete) {

		if (completedTaskCount == 0 || isDelete) {
			return;
		}

		std::string exactPaths;
		for (const auto& destPath : successfulDestPaths) {
			if (!exactPaths.empty()) {
				exactPaths += ';';
			}
			exactPaths += destPath;
		}

		if (!exactPaths.empty()) {
			updateDatabaseAfterOperations(exactPaths);
		}

	}

} // namespace

/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 *
 * Orchestrates the lifecycle of filesystem operations (`cp`, `mv`, `rm`) on selected
 * image files, from user confirmation and destination selection through multi-threaded
 * execution and database synchronization.
 *
 * @par Workflow
 * 1. **Input parsing:** Tokenizes the user string to map selections onto the master ISO list.
 * 2. **Pre-processing & validation:** Determines thread caps and triggers destination selection/confirmation.
 * 3. **Progress estimation:** Computes total byte size and task counts, scaling by destination count if needed.
 * 4. **Execution:** Spawns a progress bar thread and distributes workloads into the thread pool.
 * 5. **Post-processing & cleanup:** Cleans up signals, joins threads, and triggers database sync if required.
 *
 * @param[in]     input           Raw user input string (indices, ranges, or keywords).
 * @param[in]     isoFiles        Master list of files used for index mapping.
 * @param[in]     process         Operation type string: "cp", "mv", or "rm".
 * @param[out]    umountMvRmBreak Set to false if no tasks were successfully processed.
 * @param[in,out] filterHistory   Indicates if the current file list is a filtered view.
 * @param[out]    verbose         Toggle for detailed per-file progress output; receives user prompt answers.
 */
void processInputForCpMvRm(const std::string& input,
                           const std::vector<std::string>& isoFiles,
                           const std::string& process,
                           bool& umountMvRmBreak, bool& filterHistory,
                           bool& verbose) {

	setupSignalHandlerCancellations();

	bool isDelete = (process == "rm");
	bool isMove   = (process == "mv");
	bool isCopy   = (process == "cp");

	std::unordered_set<int> processedIndices;
	tokenizeInput(input, isoFiles, processedIndices);

	if (processedIndices.empty()) {
		umountMvRmBreak = false;
		return;
	}

	std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
	std::string operationColor = std::string(isDelete ? UI::Palette::Red : (isCopy ? UI::Palette::Green : UI::Palette::Yellow));

	ThreadPool& pool = getStaticThreadPool();
	const size_t numThreads = computeTargetThreadCount(processedIndices.size(), isDelete, pool.threadCount());
	std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

	bool overwriteExisting = false;
	std::string userDestDir;
	bool abortDel = false;

	CpMvPromptContext promptCtx{
		.isoFiles = isoFiles,
		.indexChunks = indexChunks,
		.operationColor = operationColor,
		.operationDescription = operationDescription,
		.isDelete = isDelete,
		.isCopy = isCopy,
		.userDestDir = userDestDir,
		.umountMvRmBreak = umountMvRmBreak,
		.filterHistory = filterHistory,
		.abortDel = abortDel,
		.overwriteExisting = overwriteExisting,
	};
	const std::string processedUserDestDir = userDestDirCpMv(promptCtx);

	// CpMvPromptContext holds copies of these in/out values; propagate the
	// results back to this function's variables.
	umountMvRmBreak   = promptCtx.umountMvRmBreak;
	filterHistory     = promptCtx.filterHistory;
	abortDel          = promptCtx.abortDel;
	overwriteExisting = promptCtx.overwriteExisting;

	GlobalState::g_operationCancelled.store(false);
	verboseSets.uniqueErrorTokenMessages.clear();

	if ((processedUserDestDir.empty() && (isCopy || isMove)) || abortDel) {
		return;
	}

	clearScrollBuffer();

	size_t totalBytes = 0;
	size_t totalTasks = 0;
	computeWorkloadTotals(indexChunks, isoFiles, processedUserDestDir,
	                      processedIndices.size(), isCopy || isMove,
	                      totalBytes, totalTasks);

	const std::string header = printCpMvRmOperationHeader(totalTasks, operationColor, process);
	const std::string coloredProcess = getColoredProcess(process, isDelete, isMove, isCopy);

	std::atomic<size_t> completedBytes(0);
	std::atomic<size_t> completedTasks(0);
	std::atomic<size_t> failedTasks(0);
	std::atomic<bool> isProcessingComplete(false);

	std::thread progressThread = launchProgressBar(
	    &completedBytes, totalBytes, &completedTasks, &failedTasks,
	    totalTasks, &isProcessingComplete, &verbose,
	    coloredProcess, header);

	std::vector<std::string> successfulDestPaths;
	std::mutex destPathsMutex;

	FileOperationOptions options{
		isoFiles,
		processedUserDestDir,
		isMove,
		isCopy,
		isDelete,
		overwriteExisting
	};

	FileOperationProgress progress{
		completedBytes,
		completedTasks,
		failedTasks,
		successfulDestPaths,
		destPathsMutex
	};

	executeFileOperations(pool, indexChunks, options, progress);

	const size_t finalCompletedTasks = completedTasks.load();
	if (finalCompletedTasks == 0) {
		umountMvRmBreak = false;
	}

	isProcessingComplete.store(true);
	signal(SIGINT, SIG_IGN);
	progressThread.join();

	syncDatabaseForDestinations(successfulDestPaths, finalCompletedTasks, isDelete);

}
