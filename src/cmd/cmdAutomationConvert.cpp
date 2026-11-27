// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import isocmd.cmd.cmdAutomation;
import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.inputHandling;
import isocmd.ops.convert;
import isocmd.proc.process;
import isocmd.threadpool.threadpool;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

namespace fs = std::filesystem;

using namespace IsoCli;

// ─── Image file discovery (for c2iso) ───────────────────────────────────────────

enum class ImgKind { None, BinImg, Mdf, Nrg, Chd, Daa };

/**
 * @brief Map a file extension to the conversion backend that handles it.
 *        BIN/IMG/CCD share the default backend (convertCcdToIso).
 */
static ImgKind classifyImage(const fs::path& p) {

	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
	    [](unsigned char c) {

			return static_cast<char>(std::tolower(c));

		});
	if (ext == ".bin" || ext == ".img" || ext == ".ccd") return ImgKind::BinImg;
	if (ext == ".mdf") return ImgKind::Mdf;
	if (ext == ".nrg") return ImgKind::Nrg;
	if (ext == ".chd") return ImgKind::Chd;
	if (ext == ".daa" || ext == ".gbi") return ImgKind::Daa;
	return ImgKind::None;

}

/**
 * @brief Recursively scan @p dir for convertible disk images.
 *        Same depth/symlink/error semantics as scanDirectoryForISOs().
 */
static void scanDirectoryForImages(const fs::path& dir,
                                   int maxDepth,
                                   int currentDepth,
                                   std::unordered_set<std::string>& out,
                                   bool& hasErrors,
                                   bool silentMode) {

	try {
		for (const auto& entry : fs::directory_iterator(dir)) {
			if (GlobalState::g_operationCancelled.load()) return;
			if (entry.is_symlink()) continue;

			if (entry.is_regular_file()) {
				if (classifyImage(entry.path()) != ImgKind::None)
				    out.insert(fs::canonical(entry.path()).string());
			} else if (entry.is_directory() &&
			           (maxDepth == -1 || currentDepth < maxDepth)) {
				scanDirectoryForImages(entry.path(), maxDepth,
				                       currentDepth + 1,
				                       out, hasErrors, silentMode);
			}
		}
	} catch (const fs::filesystem_error& e) {
		warnMsg(silentMode,
		        std::string("Error scanning directory: ") + e.what(),
		        dir.string());
		hasErrors = true;
	}

}

// ─── Convert branch (c2iso) ─────────────────────────────────────────────────────

/// Represents a conversion task with its target file path and backend kind.
struct ConversionTask {
	std::string path;
	ImgKind kind;
};

/// Collects and validates disk image paths from command-line arguments and directories.
static bool collectAndValidateImages(const ParsedArgs& args,
                                      std::unordered_set<std::string>& images,
                                      bool& hasErrors) {

	for (const auto& rawPath : args.paths) {
		if (GlobalState::g_operationCancelled.load()) {
			verboseWarn(args.silentMode, "Operation cancelled by user.");
			return true; // Cancelled
		}

		fs::path path(rawPath);

		try {
			if (!fs::exists(path)) {
				warnMsg(args.silentMode, "does not exist, skipping.", rawPath);
				hasErrors = true;

			} else if (fs::is_regular_file(path)) {
				if (classifyImage(path) == ImgKind::None) {
					warnMsg(args.silentMode,
					        "is not a supported image (bin/img/ccd/mdf/nrg/chd/daa/gbi), skipping.",
					        rawPath);
					hasErrors = true;
				} else {
					images.insert(fs::canonical(path).string());
				}

			} else if (fs::is_directory(path)) {
				disableInput();

				std::string depthDesc = (args.maxDepth == 0)
				    ? std::string("surface scan")
				    : std::string("max depth: ") +
				      (args.maxDepth < 0
				           ? "unlimited"
				           : std::to_string(args.maxDepth));

				verboseInfo(args.silentMode,
				            std::string("Scanning directory ")
				            .append(path.string())
				            .append(" (").append(depthDesc).append(")..."));

				scanDirectoryForImages(path, args.maxDepth, 0,
				                       images, hasErrors, args.silentMode);

			} else {
				warnMsg(args.silentMode,
				        "is not a valid file or directory, skipping.", rawPath);
				hasErrors = true;
			}

		} catch (const fs::filesystem_error& e) {
			warnMsg(args.silentMode,
			        std::string("Error processing path: ") + e.what(), rawPath);
			hasErrors = true;
		}
	}
	return false;

}

/// Executes concurrent conversion workers across prepared tasks.
static void runConvertWorkers(const std::vector<ConversionTask>& tasks,
                              std::atomic<std::size_t>& completedBytes,
                              std::atomic<std::size_t>& completedTasks,
                              std::atomic<std::size_t>& failedTasks,
                              std::vector<std::string>& successfulOutputs,
                              std::mutex& outMutex) {

	ThreadPool& pool = getStaticThreadPool();
	const std::size_t poolSize = pool.threadCount();

	const std::size_t numThreads = std::min({
		tasks.size(),
		GlobalThreadCaps::CONV_THREAD_CAP,
		poolSize
	});

	std::atomic<std::size_t> nextTask{0};

	std::vector<std::future<void>> futures;
	futures.reserve(numThreads);

	for (std::size_t t = 0; t < numThreads; ++t) {
		futures.emplace_back(pool.enqueue(
		    [&tasks, &nextTask,
		     &completedBytes, &completedTasks, &failedTasks,
		     &successfulOutputs, &outMutex]() {

				while (!GlobalState::g_operationCancelled.load()) {
					const std::size_t i = nextTask.fetch_add(1);

					if (i >= tasks.size())
					    break;

					const auto& task = tasks[i];

					const ConversionMode mode{
						.mdf = task.kind == ImgKind::Mdf,
						.nrg = task.kind == ImgKind::Nrg,
						.chd = task.kind == ImgKind::Chd,
						.daa = task.kind == ImgKind::Daa
					};

					const ConversionBatchData batchData{
						.completedBytes = &completedBytes,
						.completedTasks = &completedTasks,
						.failedTasks = &failedTasks,
						.successfulOutputPaths = &successfulOutputs,
						.outPathsMutex = &outMutex
					};

					convertToISO({task.path}, mode, batchData);
				}

			}));
	}

	for (auto& future : futures)
	    future.wait();

}

// ------------------------------------------------------------------------- //
// Conversion helpers                                                        //
// ------------------------------------------------------------------------- //

/**
 * @brief Groups supported disk images by conversion backend.
 *
 * Classifies each image according to its format, appends it to the
 * corresponding backend collection, and sorts every collection
 * alphabetically to ensure deterministic processing order.
 *
 * Images that do not match a supported conversion backend are ignored.
 *
 * @param images Set of validated image paths to classify.
 * @param binImg Receives BIN/IMG image paths.
 * @param mdf    Receives MDF image paths.
 * @param nrg    Receives NRG image paths.
 * @param chd    Receives CHD image paths.
 * @param daa    Receives DAA image paths.
 */
static void groupConversionImages(
    const std::unordered_set<std::string>& images,
    std::vector<std::string>& binImg,
    std::vector<std::string>& mdf,
    std::vector<std::string>& nrg,
    std::vector<std::string>& chd,
    std::vector<std::string>& daa) {

	for (const auto& file : images) {
		switch (classifyImage(file)) {
			case ImgKind::BinImg: binImg.push_back(file); break;
			case ImgKind::Mdf:    mdf.push_back(file); break;
			case ImgKind::Nrg:    nrg.push_back(file); break;
			case ImgKind::Chd:    chd.push_back(file); break;
			case ImgKind::Daa:    daa.push_back(file); break;
			default: break;
		}
	}

	for (auto* files : {&binImg, &mdf, &nrg, &chd, &daa})
	    std::sort(files->begin(), files->end());

}

/**
 * @brief Calculates the estimated total output size of conversion tasks.
 *
 * Uses the format-specific size estimation logic for each backend and
 * accumulates the results to determine the total byte count required
 * by the conversion progress display.
 *
 * Returns a minimum value of one byte when the estimated total is zero
 * to prevent division-by-zero conditions in progress calculations.
 *
 * @param binImg BIN/IMG image paths.
 * @param mdf    MDF image paths.
 * @param nrg    NRG image paths.
 * @param chd    CHD image paths.
 * @param daa    DAA image paths.
 *
 * @return Estimated total output size in bytes, or 1 if the estimate
 *         is zero.
 */
static std::size_t calculateConversionTotalBytes(
    const std::vector<std::string>& binImg,
    const std::vector<std::string>& mdf,
    const std::vector<std::string>& nrg,
    const std::vector<std::string>& chd,
    const std::vector<std::string>& daa) {

	std::size_t totalBytes = 0;

	totalBytes += calculateTotalBytesForConversions(
	    binImg, false, false, false, false);
	totalBytes += calculateTotalBytesForConversions(
	    mdf, true, false, false, false);
	totalBytes += calculateTotalBytesForConversions(
	    nrg, false, true, false, false);
	totalBytes += calculateTotalBytesForConversions(
	    chd, false, false, true, false);
	totalBytes += calculateTotalBytesForConversions(
	    daa, false, false, false, true);

	return totalBytes == 0 ? 1 : totalBytes;

}

/**
 * @brief Initializes and starts the conversion progress display.
 *
 * Constructs the operation header, clears the terminal scroll buffer,
 * and launches the progress-monitoring thread when silent mode is
 * disabled.
 *
 * The progress thread monitors the supplied atomic counters and
 * completion flag while updating the conversion progress display.
 * The verbose-output flag is shared with the caller so that the
 * progress interface can communicate whether detailed results
 * should be displayed after conversion.
 *
 * In silent mode, no terminal output or background thread is created.
 *
 * @param args          Parsed command-line arguments controlling output.
 * @param completedBytes Atomic counter tracking completed output bytes.
 * @param totalBytes     Estimated total output size in bytes.
 * @param completedTasks Atomic counter tracking successful conversions.
 * @param failedTasks    Atomic counter tracking failed conversions.
 * @param totalTasks     Total number of conversion tasks.
 * @param isComplete     Atomic flag indicating completion of all workers.
 * @param showVerbose    Shared flag indicating whether detailed results
 *                       should be displayed.
 *
 * @return A joinable progress-monitoring thread when output is enabled,
 *         or a non-joinable thread in silent mode.
 */
static std::thread startConversionProgress(
    const ParsedArgs& args,
    std::atomic<std::size_t>& completedBytes,
    std::size_t totalBytes,
    std::atomic<std::size_t>& completedTasks,
    std::atomic<std::size_t>& failedTasks,
    std::size_t totalTasks,
    std::atomic<bool>& isComplete,
    bool& showVerbose) {

	if (args.silentMode)
	    return {};

	const std::string coloredProcess =
	    std::string(UI::Palette::Orange) + "c2iso" +
	    std::string(UI::Palette::BoldReset);

	std::ostringstream hdr;
	hdr << "\n" << color << " Processing "
	    << (totalTasks == 1 ? "task for " : "tasks for ")
	    << UI::Palette::Orange << "c2iso" << color << "... ("
	    << UI::Palette::Red << "Ctrl+c"
	    << color << ":cancel)\n";

	const std::string header = hdr.str();

	clearScrollBuffer();
	std::cout << header;

	return launchProgressBar(
	    &completedBytes, totalBytes,
	    &completedTasks, &failedTasks, totalTasks,
	    &isComplete, &showVerbose,
	    coloredProcess, header);

}

/**
 * @brief Constructs conversion tasks in deterministic processing order.
 *
 * Converts the format-specific image collections into a unified task
 * list, preserving the established backend ordering and the alphabetical
 * ordering of files within each backend.
 *
 * Reserves sufficient capacity before appending tasks to avoid
 * unnecessary vector reallocations.
 *
 * @param binImg BIN/IMG image paths.
 * @param mdf    MDF image paths.
 * @param nrg    NRG image paths.
 * @param chd    CHD image paths.
 * @param daa    DAA image paths.
 *
 * @return A vector containing all conversion tasks in processing order.
 */
static std::vector<ConversionTask> buildConversionTasks(
    const std::vector<std::string>& binImg,
    const std::vector<std::string>& mdf,
    const std::vector<std::string>& nrg,
    const std::vector<std::string>& chd,
    const std::vector<std::string>& daa) {

	std::vector<ConversionTask> tasks;

	const std::size_t totalTasks =
	    binImg.size() + mdf.size() + nrg.size() +
	    chd.size() + daa.size();

	tasks.reserve(totalTasks);

	auto appendTasks = [&](const std::vector<std::string>& files,
	                       ImgKind kind) {

		for (const auto& file : files)
		    tasks.push_back({file, kind});

	};

	appendTasks(binImg, ImgKind::BinImg);
	appendTasks(mdf,    ImgKind::Mdf);
	appendTasks(nrg,    ImgKind::Nrg);
	appendTasks(chd,    ImgKind::Chd);
	appendTasks(daa,    ImgKind::Daa);

	return tasks;

}

/**
 * @brief Displays detailed results of the conversion operation.
 *
 * Outputs completed, skipped, and failed operation messages, followed
 * by a summary of successful and failed conversions.
 *
 * Detailed output is displayed only when silent mode is disabled and
 * the progress interface has explicitly requested verbose results.
 *
 * Synchronizes terminal output through SyncOutput to prevent
 * interference with other terminal-writing components.
 *
 * @param silentMode     Whether terminal output is suppressed.
 * @param showVerbose    Whether detailed operation results were requested.
 * @param completedTasks Atomic counter containing successful conversions.
 * @param failedTasks    Atomic counter containing failed conversions.
 */
static void displayConversionResults(
    bool silentMode,
    bool showVerbose,
    const std::atomic<std::size_t>& completedTasks,
    const std::atomic<std::size_t>& failedTasks) {

	if (silentMode || !showVerbose)
	    return;

	SyncOutput sync;
	clearScrollBuffer();
	std::cout << "\n";

	for (const auto& msg : verboseSets.operationCompleted)
	    std::cout << formatVerboseLine("c2iso", msg) << UI::Palette::BoldReset << "\n";

	for (const auto& msg : verboseSets.operationSkipped)
	    std::cout << formatVerboseLine("c2iso", msg) << UI::Palette::BoldReset << "\n";

	for (const auto& msg : verboseSets.operationFailed)
	    std::cout << formatVerboseLine("c2iso", msg) << UI::Palette::BoldReset << "\n";

	std::cout << "\nConversion summary:\n"
	          << " Successful: " << completedTasks.load() << "\n"
	          << " Failed:     " << failedTasks.load()
	          << UI::Palette::Reset << "\n\n";

}

/**
 * @brief Determines the final exit status of the conversion operation.
 *
 * Treats the operation as unsuccessful if any conversion failed,
 * input-path validation reported errors, or cancellation was requested.
 *
 * @param failedTasks Number of failed conversion tasks.
 * @param hasErrors   Whether input-path validation encountered errors.
 * @param cancelled   Whether the operation was cancelled.
 *
 * @return 0 if the operation completed successfully; otherwise 1.
 */
static int getConversionExitStatus(
    std::size_t failedTasks,
    bool hasErrors,
    bool cancelled) {

	return (failedTasks == 0 && !hasErrors && !cancelled) ? 0 : 1;

}

// ------------------------------------------------------------------------- //
// handleConvert                                                              //
// ------------------------------------------------------------------------- //

/**
 * @brief Converts supported disk images into ISO files.
 *
 * Returns immediately with a notice if no paths were supplied.
 * Otherwise validates the supplied input paths, recursively discovers
 * supported disk images according to the configured scan depth, and
 * prepares conversion tasks grouped by their respective backends.
 *
 * Calculates the estimated total output size and initializes the
 * progress display when silent mode is disabled. Conversion tasks
 * are executed concurrently through the shared worker infrastructure,
 * which manages task distribution, conversion progress, and failures.
 *
 * Waits for all conversion workers and the progress-monitoring thread
 * to finish before optionally displaying detailed operation results.
 *
 * @param args Parsed command-line arguments containing input paths,
 *             maximum scan depth, and silent-mode settings.
 *
 * @return 0 if all conversions completed successfully, or if no paths
 *         were given, or if no convertible images were found and no
 *         path errors occurred.
 *         Returns 1 if any input path was invalid, any conversion
 *         failed, or the operation was cancelled.
 */
int handleConvert(const ParsedArgs& args) {

	std::unordered_set<std::string> images;
	bool hasErrors = false;

	if (args.paths.empty()) {
		verboseWarn(args.silentMode, "No files to c2iso.");
		return 0;
	}

	if (collectAndValidateImages(args, images, hasErrors))
	    return 1;

	if (GlobalState::g_operationCancelled.load())
	    verboseWarn(args.silentMode, "Conversion cancelled by user.");

	if (images.empty()) {
		if (GlobalState::g_operationCancelled.load())
		    return 1;

		verboseWarn(args.silentMode,
		            "No convertible image files found.");

		return hasErrors ? 1 : 0;
	}

	// Group and sort images by conversion backend.
	std::vector<std::string> binImg, mdf, nrg, chd, daa;

	groupConversionImages(
	    images, binImg, mdf, nrg, chd, daa);

	verboseInfo(args.silentMode,
	            std::string("Located ")
	            .append(std::to_string(images.size()))
	            .append(" image file")
	            .append(images.size() == 1 ? "" : "s")
	            .append("; Attempting to convert...\n"));

	std::atomic<std::size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
	std::atomic<bool> isComplete{false};

	std::vector<std::string> successfulOutputs;
	std::mutex outMutex;
	bool showVerbose = false;

	// Calculate estimated output size.
	const std::size_t totalBytes = calculateConversionTotalBytes(
	    binImg, mdf, nrg, chd, daa);

	// Start progress display.
	std::thread barThread = startConversionProgress(
	    args,
	    completedBytes,
	    totalBytes,
	    completedTasks,
	    failedTasks,
	    images.size(),
	    isComplete,
	    showVerbose);

	// Build and execute conversion tasks.
	const auto tasks = buildConversionTasks(
	    binImg, mdf, nrg, chd, daa);

	runConvertWorkers(
	    tasks,
	    completedBytes,
	    completedTasks,
	    failedTasks,
	    successfulOutputs,
	    outMutex);

	isComplete.store(true, std::memory_order_release);

	if (barThread.joinable())
	    barThread.join();

	// Display detailed results if requested.
	displayConversionResults(
	    args.silentMode,
	    showVerbose,
	    completedTasks,
	    failedTasks);

	return getConversionExitStatus(
	    failedTasks.load(),
	    hasErrors,
	    GlobalState::g_operationCancelled.load());

}
