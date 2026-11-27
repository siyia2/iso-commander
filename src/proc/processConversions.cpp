// SPDX-License-Identifier: GPL-3.0-or-later

// Resolves selected disc images and dispatches conversion jobs with shared progress state.

#include <signal.h>

import std;
import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.terminalControl;
import isocmd.input.tokenize;
import isocmd.ops.convert;
import isocmd.proc.process;
import isocmd.select.select;
import isocmd.threadpool.threadpool;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;

/**
 * @brief Builds the colorized operation label (e.g. "mdf2iso") for the header.
 *
 * Falls back to "ccd2iso" when no other mode flag is set.
 *
 * @param modeMdf True if converting MDF images.
 * @param modeNrg True if converting NRG images.
 * @param modeChd True if converting CHD images.
 * @param modeDaa True if converting DAA/GBI images.
 * @return Orange-highlighted operation name followed by the base color code.
 */
static std::string buildOperationLabel(const bool modeMdf, const bool modeNrg,
                                       const bool modeChd, const bool modeDaa)
{

	const std::pair<bool, const char*> modes[] = {
		{modeMdf, "mdf2iso"},
		{modeNrg, "nrg2iso"},
		{modeChd, "chd2iso"},
		{modeDaa, "daa2iso"},
	};

	const char* name = "ccd2iso";
	for (const auto& [enabled, label] : modes) {
		if (enabled) { name = label; break; }
	}

	return std::string(UI::Palette::Orange) + name + std::string(color);

}

/**
 * @brief Worker loop executed by each pool thread.
 *
 * Repeatedly claims the next file index from the shared atomic counter and
 * converts that file until no indices remain, keeping the load balanced
 * regardless of per-file size.
 *
 * @param fileList  Master list of image files.
 * @param indices   1-based indices of the selected files.
 * @param nextIndex Shared atomic counter of the next unclaimed position.
 * @param mode      Conversion mode flags.
 * @param batchData Shared progress counters and output-path collection.
 */
static void conversionWorker(const std::vector<std::string>& fileList,
                             const std::vector<int>& indices,
                             std::atomic<std::size_t>& nextIndex,
                             const ConversionMode& mode,
                             const ConversionBatchData& batchData)
{

	for (std::size_t i; (i = nextIndex.fetch_add(1)) < indices.size();) {
		convertToISO(
		    {fileList[indices[i] - 1]},
		    mode,
		    batchData);
	}

}

/**
 * @brief Joins the successful output paths with ';' and triggers a
 *        synchronous database update.
 *
 * Does nothing if no conversion succeeded.
 *
 * @param successfulOutputPaths Output paths of successfully converted files.
 */
static void updateDatabaseForSuccessfulPaths(
    const std::vector<std::string>& successfulOutputPaths)
{

	if (successfulOutputPaths.empty()) return;

	std::string exactPaths;
	for (const auto& outPath : successfulOutputPaths) {
		if (!exactPaths.empty()) exactPaths += ';';
		exactPaths += outPath;
	}
	updateDatabaseAfterOperations(exactPaths);

}

/**
 * @brief Handles bulk image-to-ISO conversions with threading and progress
 *        visualization.
 *
 * Orchestrates the lifecycle of image conversions (mdf2iso, nrg2iso, chd2iso,
 * daa2iso, ccd2iso) on selected image files, with multi-threaded execution and
 * database synchronization.
 *
 * @par Workflow
 * 1. **Input parsing:** tokenizes the user string to map selections onto the
 *    master file list.
 * 2. **Pre-processing & validation:** determines the thread count as the
 *    smallest of the selected file count, CONV_THREAD_CAP, and the static
 *    pool size (with a floor of 2).
 * 3. **Progress estimation:** computes the total byte size of the selected
 *    files via calculateTotalBytesForConversions() and the task count (one per
 *    file).
 * 4. **Execution:** spawns a dedicated thread for the live progress bar and
 *    enqueues at most numThreads workers into the static thread pool. Each
 *    worker pulls the next file index from a shared atomic counter and calls
 *    convertToISO() until none remain. This caps concurrency at
 *    CONV_THREAD_CAP while keeping load balanced, regardless of whether the
 *    file count divides evenly or individual files vary widely in size.
 * 5. **Post-processing & cleanup:** disables signal handlers, joins the
 *    progress thread, and — if any conversion succeeded — triggers a
 *    synchronous database update for the output paths before returning.
 *
 * @param input    Raw user input string (indices, ranges, or keywords).
 * @param fileList Master list of image files used for index mapping (passed
 *                 non-const for compatibility with `tokenizeInput`).
 * @param modeMdf  True if converting MDF images.
 * @param modeNrg  True if converting NRG images.
 * @param modeChd  True if converting CHD images.
 * @param modeDaa  True if converting DAA/GBI images.
 * @param verbose  Toggle for detailed per-file progress output; also receives
 *                 the user's answer to the verbose prompt.
 */
void processInputForConversions(const std::string& input,
                                 std::vector<std::string>& fileList,
                                 const bool& modeMdf, const bool& modeNrg,
                                 const bool& modeChd, const bool& modeDaa,
                                 bool& verbose
                                 )
{

	std::vector<std::string> successfulOutputPaths;
	std::mutex outPathsMutex;

	setupSignalHandlerCancellations();

	const MainTheme* theme = getActiveTheme();
	const bool isOrig = (globalTheme == "original");

	GlobalState::g_operationCancelled.store(false);
	std::unordered_set<int> processedIndices;

	if (!(input.empty() || std::all_of(input.begin(), input.end(), [](unsigned char ch) {

	return std::isspace(ch);}))) {
		tokenizeInput(input, fileList, processedIndices);
	} else return;

	if (processedIndices.empty()) {
		auto render = [&] {

			SyncOutput sync;
			clearScrollBuffer();
			std::cout << "\n" << (isOrig ? UI::Palette::Red : theme->secondary)
			          << "No valid input provided." << UI::Palette::BoldReset << "\n";

		};

		pressEnterToReturnWithRepaint(render);
		return;
	}

	ThreadPool& pool = getStaticThreadPool();
	const std::size_t numThreads = std::max(std::size_t(2),
	    std::min({processedIndices.size(), GlobalThreadCaps::CONV_THREAD_CAP,
			      pool.threadCount()}));

	std::vector<std::string> filesToProcess;
	filesToProcess.reserve(processedIndices.size());
	for (int idx : processedIndices)
	    filesToProcess.push_back(fileList[idx - 1]);
	const std::size_t totalBytes = calculateTotalBytesForConversions(
	    filesToProcess, modeMdf, modeNrg, modeChd, modeDaa);

	const std::size_t totalTasks = processedIndices.size();
	const std::string suffix = (totalTasks > 1 ? "tasks" : "task");
	const std::string operation =
	    buildOperationLabel(modeMdf, modeNrg, modeChd, modeDaa);

	clearScrollBuffer();
	std::ostringstream hdr;
	hdr << "\n" << color << " Processing " << suffix << " for "
	    << operation << color << "... ("
	    << UI::Palette::Red << "Ctrl+c"
	    << color << ":cancel)\n";
	const std::string header = hdr.str();
	std::cout << header;

	std::atomic<std::size_t> completedBytes(0);
	std::atomic<std::size_t> completedTasks(0);
	std::atomic<std::size_t> failedTasks(0);
	std::atomic<bool>   isProcessingComplete(false);

	std::thread progressThread = launchProgressBar(
	    &completedBytes, totalBytes,
	    &completedTasks, &failedTasks, totalTasks,
	    &isProcessingComplete, &verbose, operation, header);

	const std::vector<int> indices(processedIndices.begin(), processedIndices.end());
	std::atomic<std::size_t> nextIndex(0);

	std::vector<std::future<void>> futures;
	futures.reserve(numThreads);

	const ConversionMode mode{
		.mdf = modeMdf,
		.nrg = modeNrg,
		.chd = modeChd,
		.daa = modeDaa
	};

	const ConversionBatchData batchData{
		.completedBytes = &completedBytes,
		.completedTasks = &completedTasks,
		.failedTasks = &failedTasks,
		.successfulOutputPaths = &successfulOutputPaths,
		.outPathsMutex = &outPathsMutex
	};

	for (std::size_t t = 0; t < numThreads; ++t) {
		futures.emplace_back(pool.enqueue(
		    [&fileList, &indices, &nextIndex, mode, batchData]() {

				conversionWorker(fileList, indices, nextIndex, mode, batchData);

			}));
	}

	for (auto& future : futures)
	    future.wait();

	isProcessingComplete.store(true);
	signal(SIGINT, SIG_IGN);
	progressThread.join();

	updateDatabaseForSuccessfulPaths(successfulOutputPaths);

}
