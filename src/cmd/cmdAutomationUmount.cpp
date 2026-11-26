// SPDX-License-Identifier: GPL-3.0-or-later

#include <unistd.h>

import std;
import isocmd.cmd.cmdAutomation;
import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.inputHandling;
import isocmd.ops.umount;
import isocmd.proc.process;
import isocmd.threadpool.threadpool;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

namespace fs = std::filesystem;

using namespace IsoCli;

// ─── Umount branch ──────────────────────────────────────────────────────────

/// Executes concurrent unmount workers across sorted mount paths.
static void runUnmountWorkers(const std::vector<std::string>& paths,
                              bool silentMode,
                              std::atomic<std::size_t>& completedBytes,
                              std::atomic<std::size_t>& completedTasks,
                              std::atomic<std::size_t>& failedTasks,
                              std::atomic<std::size_t>& nextTask) {

	ThreadPool& pool = getStaticThreadPool();
	const std::size_t poolSize = pool.threadCount();
	const std::size_t numThreads = std::min({
		paths.size(),
		GlobalThreadCaps::UMOUNT_THREAD_CAP,
		poolSize
	});

	std::vector<std::future<void>> futures;
	futures.reserve(numThreads);

	for (std::size_t t = 0; t < numThreads; ++t) {
		futures.emplace_back(pool.enqueue(
		    [&paths, &nextTask,
		     &completedBytes, &completedTasks, &failedTasks,
		     silentMode]() {

				// Stop claiming new items on Ctrl+C so the bar ends at once;
				// unclaimed items are marked as cancelled after the bar.
				while (!GlobalState::g_operationCancelled.load()) {
					const std::size_t i = nextTask.fetch_add(1);

					if (i >= paths.size())
					    break;

					unmountISO(std::vector<std::string>{paths[i]},
					           &completedTasks, &failedTasks,
					           silentMode);

					completedBytes.fetch_add(1);
				}

			}));
	}

	for (auto& future : futures)
	    future.wait();

}

/// Holds the aggregated results and execution state from collecting and validating ISO mount points.
struct CollectionResult {
	std::unordered_set<std::string> mountPoints;
	bool hasErrors = false;
	bool cancelled = false;
	bool abortImmediately = false;
};

/**
 * @brief Checks if the global operation cancellation flag has been set.
 *
 * @param[in] args Parsed command-line arguments for silent mode configuration.
 * @param[out] res Collection result structure to update cancellation metadata.
 * @param[in] message Custom warning message to display if cancelled.
 * @return true if the operation was cancelled, false otherwise.
 */
static bool checkCancellation(const ParsedArgs& args,
                              CollectionResult& res,
                              const char* message = "Operation cancelled by user.") {

	if (GlobalState::g_operationCancelled.load()) {
		verboseWarn(args.silentMode, message);
		res.cancelled = true;
		res.abortImmediately = true;
		return true;
	}
	return false;

}

/**
 * @brief Performs a surface scan of the specified mount directory to collect ISO mount points.
 *
 * @param[in] mntPath The directory path to scan.
 * @param[in] args Parsed command-line arguments containing silent mode configuration.
 * @param[out] res Collection result structure where discovered mount points and error flags are recorded.
 */
static void collectFromMnt(const fs::path& mntPath,
                           const ParsedArgs& args,
                           CollectionResult& res) {

	disableInput();
	verboseInfo(args.silentMode,
	            std::string("Scanning ")
	            .append(mntPath.string())
	            .append(" for ISO mount points (surface scan)..."));

	try {
		for (const auto& entry : fs::directory_iterator(mntPath)) {
			if (GlobalState::g_operationCancelled.load())
			    return;

			if (entry.is_directory())
			    res.mountPoints.insert(fs::canonical(entry.path()).string());
		}
	} catch (const fs::filesystem_error& e) {
		if (e.code() != std::errc::no_such_file_or_directory)
		    res.hasErrors = true;
	}

}

/**
 * @brief Validates and processes a single user-specified input path.
 *
 * @param[in] rawPath The raw path string from command-line arguments.
 * @param[in] args Parsed command-line arguments containing silent mode configuration.
 * @param[out] res Collection result structure to update mount points and error flags.
 */
static void processUserPath(const std::string& rawPath,
                            const ParsedArgs& args,
                            CollectionResult& res) {

	fs::path path(rawPath);
	try {
		if (fs::exists(path) && fs::is_directory(path)) {
			const std::string canonical = fs::canonical(path).string();

			if (canonical == "/mnt/ISOs") {
				collectFromMnt(path, args, res);
			} else if (canonical.rfind("/mnt/ISOs/", 0) == 0) {
				res.mountPoints.insert(canonical);
			} else {
				warnMsg(args.silentMode,
				        "is not allowed. Only /mnt/ISOs or /mnt/ISOs/isoFolder are valid.",
				        rawPath);
				res.hasErrors = true;
			}
		} else {
			fs::path candidate = path.is_relative()
			    ? fs::path("/mnt/ISOs") / path.filename()
			    : path;

			if (fs::exists(candidate) && fs::is_directory(candidate)) {
				res.mountPoints.insert(fs::canonical(candidate).string());
			} else {
				warnMsg(args.silentMode,
				        "does not exist or is not a valid mount point, skipping.",
				        rawPath);
				res.hasErrors = true;
			}
		}
	} catch (const fs::filesystem_error& e) {
		warnMsg(args.silentMode,
		        std::string("Error processing path: ") + e.what(), rawPath);
		res.hasErrors = true;
	}

}

/**
 * @brief Collects, sanitizes, and validates ISO mount points based on command-line arguments or surface scans.
 *
 * @details This function examines the provided `ParsedArgs`:
 * - If no paths or the `"all"` argument is specified, it performs a surface scan of `/mnt/ISOs`.
 * - If specific paths are provided, it validates each path, enforcing that they strictly reside within
 *   the permitted `/mnt/ISOs` directory hierarchy.
 * - It monitors `GlobalState::g_operationCancelled` asynchronously during directory iterations to support
 *   responsive user cancellation.
 *
 * @param[in] args Parsed command-line arguments containing target input paths and silent-mode configurations.
 * @return CollectionResult A structure containing the validated set of mount points, error flags, and cancellation metadata.
 *
 * @throws fs::filesystem_error If underlying filesystem canonicalization or iteration fails unrecoverably.
 */
static CollectionResult collectMountPoints(const ParsedArgs& args) {

	CollectionResult res;

	const bool scanAllMnt = args.paths.empty() ||
	    (args.paths.size() == 1 && args.paths[0] == "all");

	if (scanAllMnt) {
		collectFromMnt("/mnt/ISOs", args, res);
		checkCancellation(args, res);
		return res;
	}

	for (const auto& rawPath : args.paths) {
		if (checkCancellation(args, res)) {
			break;
		}
		processUserPath(rawPath, args, res);
	}

	if (GlobalState::g_operationCancelled.load() && !res.cancelled) {
		verboseWarn(args.silentMode, "Umount operation cancelled by user.");
		res.cancelled = true;
	}

	return res;

}

/**
 * @brief Starts the progress reporting thread when output is not silent.
 * @param[in]  totalTasks     Total number of unmount tasks to report on.
 * @param[in]  coloredProcess Formatted process label passed to the progress bar.
 * @param[in]  completedBytes Atomic counter of completed byte progress.
 * @param[in]  completedTasks Atomic counter of completed unmount tasks.
 * @param[in]  failedTasks    Atomic counter of failed unmount tasks.
 * @param[out] isComplete     Atomic signal set when worker execution finishes.
 * @param[out] showVerbose    Flag toggled if verbose summary output is requested.
 * @return std::thread Joinable progress bar thread, or a default-constructed thread in silent mode.
 */
static std::thread startProgressThread(
    std::size_t totalTasks,
    const std::string& coloredProcess,
    std::atomic<std::size_t>& completedBytes,
    std::atomic<std::size_t>& completedTasks,
    std::atomic<std::size_t>& failedTasks,
    std::atomic<bool>& isComplete,
    bool& showVerbose
) {

	std::ostringstream hdr;
	hdr << "\n" << color << " Processing "
	    << (totalTasks == 1 ? "task for " : "tasks for ")
	    << UI::Palette::Yellow << "umount" << color << "... ("
	    << UI::Palette::Red << "Ctrl+c"
	    << color << ":cancel)\n";
	const std::string header = hdr.str();

	clearScrollBuffer();
	std::cout << header;

	return launchProgressBar(
	    &completedBytes, totalTasks,
	    &completedTasks, &failedTasks, totalTasks,
	    &isComplete, &showVerbose,
	    coloredProcess, header);

}

/**
 * @brief Processes any remaining mount points left unclaimed after a cancellation.
 * @param[in]     paths          Sorted mount point paths across all tasks.
 * @param[in]     nextTask       Atomic index of the next unclaimed task.
 * @param[in,out] completedTasks Atomic counter of completed unmount tasks.
 * @param[in,out] failedTasks    Atomic counter of failed unmount tasks.
 * @param[in]     silentMode     Suppresses non-essential output when true.
 */
static void handleCancelledRemainder(
    const std::vector<std::string>& paths,
    const std::atomic<std::size_t>& nextTask,
    std::atomic<std::size_t>& completedTasks,
    std::atomic<std::size_t>& failedTasks,
    bool silentMode
) {

	const std::size_t first = std::min(nextTask.load(), paths.size());
	if (first < paths.size()) {
		unmountISO(
		    std::vector<std::string>(paths.begin() + first, paths.end()),
		    &completedTasks, &failedTasks, silentMode);
	}

}

/**
 * @brief Renders detailed per-item results and final task counts in verbose mode.
 * @param[in] completedTasks Atomic counter of completed unmount tasks.
 * @param[in] failedTasks Atomic counter of failed unmount tasks.
 */
static void printVerboseUnmountSummary(
    const std::atomic<std::size_t>& completedTasks,
    const std::atomic<std::size_t>& failedTasks
) {

	SyncOutput sync;
	clearScrollBuffer();
	std::cout << "\n";

	auto printUmountLine = [](std::string_view msg) {

		std::cout << msg << UI::Palette::BoldReset << "\n";

	};
	for (const auto& msg : verboseSets.operationCompleted) printUmountLine(msg);
	forEachUmountLine(true, printUmountLine);

	for (const auto& msg : verboseSets.operationFailed) printUmountLine(msg);
	forEachUmountLine(false, printUmountLine);

	std::cout << "\nUnmount summary:\n"
	          << " Successful: " << completedTasks.load() << "\n"
	          << " Failed:     " << failedTasks.load()
	          << UI::Palette::Reset << "\n\n";

}

/**
 * @brief Computes the exit status from task failure counts, prior errors, and cancellation state.
 * @param[in] failedTasks Number of unmount tasks that failed.
 * @param[in] hasErrors Flag indicating prior path validation errors.
 * @return int Returns `0` when no failures, validation errors, or cancellations occurred; `1` otherwise.
 */
static int computeUnmountExitCode(std::size_t failedTasks, bool hasErrors) {

	return (failedTasks == 0 && !hasErrors &&
	        !GlobalState::g_operationCancelled.load()) ? 0 : 1;

}

/**
 * @brief Executes concurrent unmounting of collected ISO mount points with real-time progress reporting.
 *
 * @details This function orchestrates the unmount lifecycle:
 * - Sorts the target mount point paths alphabetically for deterministic execution order.
 * - Spawns and manages a dedicated progress bar reporting thread (unless running in silent mode).
 * - Dispatches worker threads via `runUnmountWorkers` to claim tasks from a shared atomic index.
 * - Handles post-cancellation catch-up tasks for any unclaimed mount points.
 * - Performs cleanup of the parent `/mnt/ISOs` directory upon worker completion.
 * - Renders detailed operation success/failure summaries if verbose mode was requested.
 *
 * @param[in] mountPoints Unordered set of canonicalized absolute mount point paths to be unmounted.
 * @param[in] args Parsed command-line arguments controlling verbosity and silent settings.
 * @param[in] hasErrors Flag indicating whether path validation errors were encountered prior to execution.
 * @return int Returns `0` if all tasks succeed without errors or cancellation; returns `1` otherwise.
 */
static int executeUnmounts(const std::unordered_set<std::string>& mountPoints,
                           const ParsedArgs& args,
                           bool hasErrors) {

	if (mountPoints.empty()) {
		if (GlobalState::g_operationCancelled.load())
		    return 1;
		verboseWarn(args.silentMode, "No ISO mount points found to unmount.");
		return hasErrors ? 1 : 0;
	}

	verboseInfo(args.silentMode,
	            std::string("Located ")
	            .append(std::to_string(mountPoints.size()))
	            .append(" mount point")
	            .append(mountPoints.size() == 1 ? "" : "s")
	            .append("; Attempting to unmount...\n"));

	std::atomic<std::size_t> completedBytes{0}, completedTasks{0}, failedTasks{0};
	std::atomic<bool> isComplete{false};
	bool showVerbose = false;

	// Sort for deterministic processing order.
	std::vector<std::string> paths(mountPoints.begin(), mountPoints.end());
	std::sort(paths.begin(), paths.end());
	const std::size_t totalTasks = paths.size();

	// Start the progress bar unless output is silent.
	std::thread barThread;
	const std::string coloredProcess =
	        std::string(UI::Palette::Yellow) + "umount" + std::string(UI::Palette::BoldReset);

	if (!args.silentMode) {
		barThread = startProgressThread(
		    totalTasks, coloredProcess,
		    completedBytes, completedTasks, failedTasks,
		    isComplete, showVerbose);
	}

	std::atomic<std::size_t> nextTask{0};
	runUnmountWorkers(paths, args.silentMode, completedBytes, completedTasks, failedTasks, nextTask);

	isComplete.store(true, std::memory_order_release);

	if (barThread.joinable())
	    barThread.join();

	// After a cancel, mark the mount points no worker claimed as cancelled.
	if (GlobalState::g_operationCancelled.load()) {
		handleCancelledRemainder(paths, nextTask, completedTasks, failedTasks, args.silentMode);
	}

	// Clean up the parent directory after all unmount workers finish.
	{
		std::error_code ec;
		fs::remove("/mnt/ISOs", ec);
	}

	// Show detailed output only if requested through the progress prompt.
	if (!args.silentMode && showVerbose) {
		printVerboseUnmountSummary(completedTasks, failedTasks);
	}

	return computeUnmountExitCode(failedTasks.load(), hasErrors);

}

/**
 * @brief Top-level control handler for unmounting ISO mount points supplied via command-line arguments.
 *
 * @details Performs necessary privilege verification (`geteuid() == 0`), delegates path collection
 * and boundary validation to `collectMountPoints()`, and hands off validated targets to `executeUnmounts()`
 * for concurrent processing.
 *
 * @param[in] args Parsed command-line arguments structure containing input paths and execution flags.
 * @return int Exit status code:
 *             - `0` on complete success (including when no targets were found and no errors occurred).
 *             - `1` if root privileges are missing, path validation fails, unmount operations error out,
 *               or the task is cancelled by the user.
 */
int handleUmount(const ParsedArgs& args) {

	if (geteuid() != 0) {
		errMsg("Root privileges required for unmounting ISOs.");
		return 1;
	}

	CollectionResult res = collectMountPoints(args);
	if (res.abortImmediately) {
		return 1;
	}

	return executeUnmounts(res.mountPoints, args, res.hasErrors);

}
