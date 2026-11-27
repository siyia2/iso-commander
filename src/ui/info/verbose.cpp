// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>

import std;
import isocmd.input.lineEditor;
import isocmd.core.cancellation;
import isocmd.core.lists;
import isocmd.database.databaseOps;
import isocmd.input.inputHandling;
import isocmd.select.select;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

VerboseSets verboseSets;

void clearGlobalVerboseSets() {

	verboseSets.operationCompleted.clear();
	verboseSets.operationFailed.clear();
	verboseSets.operationSkipped.clear();
	verboseSets.uniqueErrorTokenMessages.clear();
	std::vector<MountRecord>().swap(verboseSets.mountRecords); // also release capacity
	std::vector<UmountRecord>().swap(verboseSets.umountRecords);

}

std::int64_t monotonicNowNs() {

	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();

}

/**
 * @brief Inserts the fixed-color operation label after a result's status symbol.
 *
 * Keeps the original status and body colors, including diagnostics on continuation
 * lines. Input-validation errors without a status receive a failure symbol.
 * Formatting is deferred until display so stored messages and their ordering
 * remain unchanged.
 */
std::string formatVerboseLine(std::string_view operation, std::string_view line) {

	if (line.empty()) return {};

	// Result producers prefix their text with theme SGR sequences.
	std::size_t offset = 0;
	while (line.substr(offset).starts_with("\033[")) {
		const auto end = line.find('m', offset + 2);
		if (end == std::string_view::npos) break;
		offset = end + 1;
	}

	const auto theme = resolveVerboseTheme();
	const auto operationColor = getOperationColor(operation);
	std::string result;
	result.reserve(line.size() + operationColor.size() + operation.size() +
	               theme.label.size() + offset + 8);

	const auto body = line.substr(offset);
	for (std::string_view status : {"✓ ", "– ", "✗ "}) {
		if (!body.starts_with(status)) continue;
		result.append(line.substr(0, offset + status.size()))
		      .append(operationColor).append(operation)
		      .append(theme.label).append(": ")
		      .append(offset ? line.substr(0, offset) : theme.reset)
		      .append(body.substr(status.size()));
		return result;
	}

	// Token and destination validation messages have no preexisting status.
	result.append(theme.error).append("✗ ")
	      .append(operationColor).append(operation)
	      .append(theme.label).append(": ")
	      .append(theme.error).append(line);
	if (!offset) result.append(theme.reset);
	return result;

}

/**
 * @brief Prints categorized operation results, sorted case-insensitively.
 *
 * Manages terminal state and prints sorted batches of success, warning and
 * error messages.
 *
 * - **Signal safety:** ignores SIGINT and disables Ctrl+D so results are
 *   read before returning to the main menu.
 * - **No copies:** sorts and prints std::string_view views of the input sets.
 * - **Case-insensitive:** std::lexicographical_compare with std::tolower.
 * - **Stream routing:** error-type items go to std::cerr with colors from
 *   VerboseAndDatabaseTheme.
 * - **Resize:** the whole print body is a render lambda handed to
 *   pressEnterToContinueWithRepaint(), so a resize during the wait repaints
 *   the results instead of clearing the screen and leaving only the prompt.
 *
 * @param primarySet   Main data set (e.g. successful unmounts or conversions).
 * @param secondarySet Supporting data (e.g. already unmounted or skipped
 *                     items).
 * @param tertiarySet  Additional context (e.g. specific warning categories).
 * @param errorSet     Explicit failure strings.
 * @param operation    Short operation label on each result.
 * @param printType    Layout mode: 0 (Umount), 1 (Basic Ops), 2 (Mount),
 *                     3 (Conversion).
 */
void verbosePrint(std::unordered_set<std::string>& primarySet,
            std::unordered_set<std::string>& secondarySet,
            std::unordered_set<std::string>& tertiarySet,
            std::unordered_set<std::string>& errorSet,
            int printType, std::string_view operation) {

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();

	auto render = [&] {

		const VerboseAndDatabaseTheme vt = getVerboseTheme();
		SyncOutput sync;
		clearScrollBuffer();
		std::cout << "\n";

		// Stored mount/unmount records: how many there are, and how to walk them as
		// formatted lines (one line in memory at a time).
		struct RecordSource {
			std::function<std::size_t()> count;
			std::function<void(const std::function<void(std::string_view)>&)> each;
		};

		bool printedGroup = false;

		// Prints @p set sorted, then the stored records of @p records (mount and
		// unmount results), formatted one at a time while printing, so no second copy
		// of all lines is built (see forEachMountLine(), forEachUmountLine()).
		auto printSortedSet = [&](std::unordered_set<std::string>& set, bool isError = false,
		                          const RecordSource* records = nullptr) {

			const bool hasRecords = records && records->count() > 0;
			if (set.empty() && !hasRecords) return;

			auto emit = [&](std::string_view item) {

				const auto line = formatVerboseLine(operation, item);
				if (isError)
				    std::cerr << vt.red << line << vt.reset << "\n";
				else
				    std::cout << line << "\n";

			};

			if (printedGroup) std::cout << "\n";
			printedGroup = true;

			if (!set.empty()) {
				std::vector<std::string_view> views;
				views.reserve(set.size());
				for (const auto& s : set)
				    views.emplace_back(s);

				std::sort(views.begin(), views.end(), [](std::string_view a, std::string_view b) {

					return std::lexicographical_compare(
					    a.begin(), a.end(), b.begin(), b.end(),
					    [](char x, char y) {

							return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);

						}
					);

				});

				for (std::string_view item : views) emit(item);
			}

			if (hasRecords) records->each(emit);

		};

		auto mountSource = [](MountRecKind kind) {

			return RecordSource{
				[kind] { return countMountRecords(kind); },
				[kind](const std::function<void(std::string_view)>& fn) { forEachMountLine(kind, fn); }};

		};
		auto umountSource = [](bool completed) {

			return RecordSource{
				[completed] { return countUmountRecords(completed); },
				[completed](const std::function<void(std::string_view)>& fn) { forEachUmountLine(completed, fn); }};

		};
		const RecordSource mountCompleted = mountSource(MountRecKind::Completed);
		const RecordSource mountSkipped   = mountSource(MountRecKind::Skipped);
		const RecordSource mountFailed    = mountSource(MountRecKind::Failed);
		const RecordSource umountCompleted = umountSource(true);
		const RecordSource umountFailed    = umountSource(false);

		switch (printType) {
			case 0:
			    printSortedSet(primarySet);
				printSortedSet(secondarySet);
				printSortedSet(errorSet, true);
				break;
			case 1: // everything but mount; unmount results also come from umount records
			    printSortedSet(primarySet, false, &umountCompleted);
				printSortedSet(secondarySet, false, &umountFailed);
				printSortedSet(errorSet);
				break;
			case 2: // mount: completed / skipped / failed also come from mount records
			    printSortedSet(primarySet, false, &mountCompleted);
				printSortedSet(tertiarySet, true, &mountSkipped);
				printSortedSet(secondarySet, true, &mountFailed);
				printSortedSet(errorSet, true);
				break;
			case 3:
			    printSortedSet(secondarySet);
				printSortedSet(tertiarySet);
				printSortedSet(errorSet);
				printSortedSet(primarySet);
				break;
		}

	};

	pressEnterToContinueWithRepaint(render);

}

/**
 * @brief Prints and clears any unique error messages from tokenization.
 *
 * Used to surface verbose input errors produced by dry-runs (cp/mv/rm).
 */
void displayErrors() {

	if (!verboseSets.uniqueErrorTokenMessages.empty()) {
		std::cout << "\n";
		for (const auto& err : verboseSets.uniqueErrorTokenMessages) {
			std::cout << err << "\n";
		}
		verboseSets.uniqueErrorTokenMessages.clear();
	}

}

/**
 * @brief Checks whether verboseSets contains error tokens with no other
 *        operation outcomes or mount/unmount records.
 *
 * @return true if uniqueErrorTokenMessages is non-empty and
 *         operationFailed, operationSkipped, operationCompleted,
 *         mountRecords, and umountRecords are all empty; false otherwise.
 */
static bool hasNoValidInput() {

	return !verboseSets.uniqueErrorTokenMessages.empty() &&
	       verboseSets.operationFailed.empty() &&
	       verboseSets.operationSkipped.empty() &&
	       verboseSets.operationCompleted.empty() &&
	       verboseSets.mountRecords.empty() &&
	       verboseSets.umountRecords.empty();

}

/**
 * @brief Clears the scroll buffer, prints the "No valid input provided."
 *        notice, and waits for the user to press Enter before repainting.
 */
static void renderNoValidInputNotice() {

	const auto c = getVerboseTheme();

	auto render = [&] {

		SyncOutput sync;
		clearScrollBuffer();
		std::cout << "\n" << (c.red)
		          << "No valid input provided."
		          << UI::Palette::BoldReset << "\n";

	};

	pressEnterToReturnWithRepaint(render);

}

/**
 * @brief Clears the scroll buffer and prints detailed operation results via
 *        verbosePrint.
 *
 * Mount operations pass verboseSets.operationSkipped and use layout mode 2;
 * all other operations pass an empty set and use layout mode 1.
 *
 * @param operation Action string ("mount", "mv", "rm", "umount", ...) used to
 *                  select the skipped-items set and layout mode.
 */
static void printVerboseResults(const std::string& operation) {

	const bool isMount = (operation == "mount");
	std::unordered_set<std::string> emptySet;

	clearScrollBuffer();
	verbosePrint(
	    verboseSets.operationCompleted,
	    verboseSets.operationFailed,
	    isMount ? verboseSets.operationSkipped : emptySet,
	    verboseSets.uniqueErrorTokenMessages,
	    isMount ? 2 : 1, operation
	);

}

/**
 * @brief Determines whether line-editor history should be cleared after a
 *        destructive operation on filtered results.
 *
 * @param operation       Action string ("mv", "rm", "umount", ...).
 * @param isFiltered      Whether a search filter is active.
 * @param umountMvRmBreak Whether a destructive operation occurred prior to this
 *                        results pass.
 * @return true if operation is "mv", "rm", or "umount" and both isFiltered
 *         and umountMvRmBreak are true; false otherwise.
 */
static bool shouldClearLineEditorHistory(const std::string& operation,
                                       bool isFiltered,
                                       bool umountMvRmBreak) {

	const bool isDestructive = (operation == "mv" ||
	                            operation == "rm" ||
	                            operation == "umount");
	return isDestructive && isFiltered && umountMvRmBreak;

}

/**
 * @brief Reports the results of an ISO file-selection operation.
 *
 * Examines verboseSets: prints a "no valid input" notice, or delegates to
 * verbosePrint when @p verbose is set.
 *
 * - **"No valid input" check:** if uniqueErrorTokenMessages is non-empty and
 *   operationFailed, operationSkipped, operationCompleted, mountRecords, and
 *   umountRecords are all empty, clears the scroll buffer, prints the notice,
 *   and waits for Enter.
 * - **Verbose output:** if @p verbose is true, clears the scroll buffer and
 *   calls verbosePrint. Mount operations pass operationSkipped and use layout
 *   mode 2; all others pass an empty set and use mode 1.
 * - **History clear:** after a destructive operation (mv, rm, umount), if
 *   @p isFiltered and @p umountMvRmBreak are both true, line-editor history is
 *   cleared so stale indices cannot be re-run.
 *
 * @param operation       Action string ("mount", "mv", "rm", "umount", ...);
 *                        drives the verbosePrint layout mode and the
 *                        history-clear gate.
 * @param verbose         If true, triggers the detailed results screen.
 * @param isFiltered      Whether a search filter is active; gates the
 *                        history-clear path with @p umountMvRmBreak.
 * @param umountMvRmBreak True after a destructive operation; triggers the
 *                        history clear on the next results pass.
 */
void handleSelectIsoFilesResults(const std::string& operation, bool& verbose,
                                 bool& isFiltered, bool& umountMvRmBreak) {

	if (hasNoValidInput()) {
		renderNoValidInputNotice();
	} else if (verbose) {
		printVerboseResults(operation);
	}

	if (shouldClearLineEditorHistory(operation, isFiltered, umountMvRmBreak)) {
		editorClearHistory();
	}

}

/**
 * @brief Counts entries in @p allIsoFiles that are not in
 * @p globalIsoFileList.
 *
 * @param allIsoFiles      Newly discovered ISO file list to test against the cache.
 * @param globalIsoFileList Current cached ISO file list (previous database state).
 * @return Number of new entries in @p allIsoFiles not present in
 *         @p globalIsoFileList.
 */
static int countDifferentEntries(const std::vector<std::string>& allIsoFiles, const std::vector<std::string>& globalIsoFileList) {

	std::unordered_set<std::string_view> globalSet;
	globalSet.reserve(globalIsoFileList.size());

	for (const auto& file : globalIsoFileList) {
		globalSet.insert(file);
	}

	int count = 0;
	for (const auto& file : allIsoFiles) {
		if (globalSet.find(file) == globalSet.end()) {
			count++;
		}
	}

	return count;

}

namespace {

	/**
	 * @brief Prints skipped invalid search paths with color-coded styling.
	 *
	 * Handles printing the "Total files processed: 0" line if no files were
	 * scanned and no valid paths were present, followed by the formatted
	 * list of invalid directories.
	 *
	 * @param invalidPaths     Directories skipped due to permissions or existence errors.
	 * @param totalProcessed   Atomic count of files successfully scanned.
	 * @param hasValidPaths    Flag indicating whether valid base directories existed.
	 * @param vt               Theme structure providing color codes.
	 */
	void printInvalidPathsReport(const std::unordered_set<std::string>& invalidPaths,
	                             std::size_t totalProcessed,
	                             bool hasValidPaths,
	                             const VerboseAndDatabaseTheme& vt) {

		if (invalidPaths.empty()) return;

		if (totalProcessed == 0 && !hasValidPaths) {
			std::cout << "\r" << color << "Total files processed: 0\n" << std::flush;
		}
		std::cout << "\n" << color << "Invalid paths omitted from search: " << vt.red;
		for (auto it = invalidPaths.begin(); it != invalidPaths.end();) {
			std::cout << "'" << *it << "'" << (++it != invalidPaths.end() ? " " : "");
		}
		std::cout << color << "\n";

	}

	/**
	 * @brief Prints deduplicated system-level I/O error messages.
	 *
	 * @param uniqueErrorMessages Deduplicated system-level I/O errors encountered during scanning.
	 */
	void printUniqueErrorMessages(const std::unordered_set<std::string>& uniqueErrorMessages) {

		if (uniqueErrorMessages.empty()) return;

		for (const auto& error : uniqueErrorMessages) {
			std::cout << error;
		}
		std::cout << "\n";

	}

	/**
	 * @brief Evaluates state and prints the color-coded outcome of the database refresh.
	 *
	 * Evaluates the outcome across the mutually exclusive status branches (Cancelled,
	 * database access failure, lack of valid paths, no new ISO found, no ISO found,
	 * or successful import with delta count).
	 *
	 * @param isCancelled   Flag indicating whether the global operation was cancelled.
	 * @param hasIsoFiles   Flag indicating whether the discovered ISO file list is non-empty.
	 * @param hasValidPaths Flag indicating whether valid base directories were traversed.
	 * @param newISOFound   Flag indicating whether the scan produced delta changes.
	 * @param saveSuccess   Flag indicating whether the database persistence step succeeded.
	 * @param deltaCount    Number of newly imported ISO entries.
	 * @param vt            Theme structure providing color codes.
	 */
	void printDatabaseRefreshOutcome(bool isCancelled,
	                                 bool hasIsoFiles,
	                                 bool hasValidPaths,
	                                 bool newISOFound,
	                                 bool saveSuccess,
	                                 int deltaCount,
	                                 const VerboseAndDatabaseTheme& vt) {

		if (isCancelled) {
			std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "Cancelled" << vt.green << "]" << vt.bold << "\n";
		} else if (hasIsoFiles && newISOFound && !saveSuccess) {
			std::cout << "\n" << vt.red << "Database Refresh failed: [" << vt.yellow << "Unable to access the database file" << vt.red << "]" << vt.bold << "\n";
		} else if (!hasValidPaths) {
			std::cout << "\n" << vt.red << "Database refresh failed: [" << vt.yellow << "Lack of valid paths" << vt.red << "]" << vt.bold << "\n";
		} else if (hasIsoFiles && !newISOFound && !saveSuccess) {
			std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No new ISO found" << vt.green << "]" << vt.bold << "\n";
		} else if (!hasIsoFiles) {
			std::cout << "\n" << vt.green << "Database Refresh: [" << vt.yellow << "No ISO found" << vt.green << "]" << vt.bold << "\n";
		} else if (hasIsoFiles && saveSuccess && newISOFound) {
			std::cout << "\n" << vt.green << "Database Refresh: [" << vt.magenta << deltaCount << " ISO imported" << vt.green << "]" << vt.bold << "\n";
		}

	}

	/**
	 * @brief Renders the complete final results screen block.
	 *
	 * Handles screen clearing, buffer management, header repainting on resize,
	 * processing metrics, error/invalid path reporting, and status outcome printing.
	 *
	 * @param report              Snapshot of all values needed to render the
	 *                            database refresh report (see struct docs).
	 * @param vt                  Theme structure providing color codes.
	 * @param repaintCallerHeader Optional callback to reprint caller screen header on resize.
	 */
	void renderDatabaseRefreshReport(const DatabaseRefreshReport& report,
	                                 const VerboseAndDatabaseTheme& vt,
	                                 const std::function<void()>& repaintCallerHeader = {}) {

		SyncOutput sync;
		clearScrollBuffer();

		if (repaintCallerHeader) {
			repaintCallerHeader();
		}

		std::cout << "\n";

		if (report.hasValidPaths) {
			std::cout << "\r" << color << "Total files processed: " << report.totalProcessed << "\n";
		}

		printInvalidPathsReport(report.invalidPaths,
		                        report.totalProcessed,
		                        report.hasValidPaths,
		                        vt);
		printUniqueErrorMessages(report.uniqueErrorMessages);

		std::cout << color << "\nTime Elapsed: " << std::fixed << std::setprecision(1)
		          << report.totalElapsed << "s\n";

		printDatabaseRefreshOutcome(report.cancelled,
		                            report.hasIsoFiles,
		                            report.hasValidPaths,
		                            report.newISOFound,
		                            report.saveSuccess,
		                            report.deltaCount,
		                            vt);

	}

} // namespace

/**
 * @brief Finalizes an ISO database refresh: reports results and syncs global
 * state.
 *
 * See the header declaration for the full behavioral contract; this
 * definition documents only implementation-level sequencing.
 *
 * Sequencing:
 *   1. Ignore SIGINT and disable Ctrl+D (never restored).
 *   2. If `result.newISOFound`, reconcile `GlobalState::globalIsoFileList`
 *      via `loadFromDatabase`.
 *   3. Call `saveToDatabase(result.allIsoFiles, &result.newISOFound)` unless
 *      cancelled. The save may mutate `result.newISOFound`.
 *   4. Capture elapsed time from `result.start_time`.
 *   5. Compute `deltaCount` only on a successful save with `newISOFound`.
 *   6. Snapshot `totalProcessed` / `hasValidPaths` / `hasIsoFiles`.
 *   7. Hand a render lambda to `pressEnterToContinueWithRepaint`. The
 *      lambda is the sole producer of on-screen output; the caller must not
 *      print the block itself.
 *
 * @param result              Refresh inputs and scan outputs (see struct docs).
 * @param repaintCallerHeader Optional callback that reprints the caller's
 *                            screen header on a resize during the wait.
 *                            When empty, the block starts from a cleared
 *                            screen with no caller context.
 */
void saveAndReportResultsForDatabase(DatabaseRefreshResult& result,
                                     const std::function<void()>& repaintCallerHeader) {

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();

	if (result.newISOFound) {
		loadFromDatabase(GlobalState::globalIsoFileList);
	}

	const VerboseAndDatabaseTheme vt = getVerboseTheme();

	const bool saveSuccess = GlobalState::g_operationCancelled
	                             ? false
	                             : saveToDatabase(result.allIsoFiles, &result.newISOFound);
	const double total_elapsed = static_cast<double>(monotonicNowNs() - result.start_time_ns) / 1e9;

	const int deltaCount =
	    (!result.allIsoFiles.empty() && saveSuccess && result.newISOFound)
	        ? countDifferentEntries(result.allIsoFiles, GlobalState::globalIsoFileList)
	        : 0;

	const std::size_t totalProcessed = result.totalFiles.load();
	const bool hasValidPaths = !result.validPaths.empty();
	const bool hasIsoFiles = !result.allIsoFiles.empty();

	pressEnterToContinueWithRepaint([&] {

		renderDatabaseRefreshReport(
		    DatabaseRefreshReport{
				.totalProcessed = totalProcessed,
				.deltaCount = deltaCount,
				.totalElapsed = total_elapsed,
				.hasValidPaths = hasValidPaths,
				.hasIsoFiles = hasIsoFiles,
				.newISOFound = result.newISOFound,
				.saveSuccess = saveSuccess,
				.cancelled = GlobalState::g_operationCancelled,
				.invalidPaths = result.invalidPaths,
				.uniqueErrorMessages = result.uniqueErrorMessages,
			},
		    vt,
		    repaintCallerHeader);

	});

}

/**
 * @brief Logs deduplicated filesystem errors and invalid search paths.
 *
 * Formats and displays issues found during a crawl (invalid paths, I/O
 * errors), then clears both input sets for the next search cycle.
 *
 * - **Signal suppression:** SIGINT is ignored and Ctrl+D disabled at entry;
 *   neither is restored before return.
 * - **"Total files processed: 0":** printed to std::cout only when
 *   @p directoryPaths is empty and @p invalidDirectoryPaths is non-empty.
 * - **Invalid path output:** the header "Invalid paths omitted from search: "
 *   goes to std::cout; the path values and trailing "." go to std::cerr.
 *   Paths are stored uncolored; the red set by the header applies to them.
 * - **Error message output:** entries in @p processedErrorsFind go to
 *   std::cout, separated by newlines (no trailing newline after the last
 *   entry).
 * - **Cleanup:** both @p processedErrorsFind and @p invalidDirectoryPaths are
 *   cleared unconditionally before return.
 *
 * @param invalidDirectoryPaths Inaccessible or invalid paths; cleared on exit.
 * @param directoryPaths        Source directories from the crawl; only
 *                              .empty() is checked, to gate the
 *                              "Total files processed: 0" line.
 * @param processedErrorsFind   Deduplicated formatted error strings; cleared on
 *                              exit.
 */
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind) {

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();

	const VerboseAndDatabaseTheme vt = getVerboseTheme();
	std::cout << "\n";

	if (directoryPaths.empty() && !invalidDirectoryPaths.empty()) {
		std::cout << "\r" << color << "Total files processed: 0" << std::flush;
	}

	if (!invalidDirectoryPaths.empty()) {
		std::cout << "\n\n" << color << "Invalid paths omitted from search: " << vt.red;
		for (auto it = invalidDirectoryPaths.begin(); it != invalidDirectoryPaths.end(); ++it) {
			std::cerr << "'" << *it << "'" << (std::next(it) != invalidDirectoryPaths.end() ? " " : "");
		}
		std::cerr << color;
	}

	if (!processedErrorsFind.empty()) {
		std::cout << "\n\n";
		auto it = processedErrorsFind.begin();
		while (it != processedErrorsFind.end()) {
			std::cout << *it;
			++it;
			if (it != processedErrorsFind.end()) {
				std::cout << "\n";
			}
		}
	}

	processedErrorsFind.clear();
	invalidDirectoryPaths.clear();

}

/**
 * @brief Executes `verboseFind` while capturing `std::cout` and `std::cerr`.
 *
 * Restores both stream buffers before returning and trims a single leading
 * newline when valid directories were scanned and the captured buffer starts
 * with three newlines.
 *
 * @param req            Search inputs and in/out sets forwarded to `verboseFind`.
 * @param hadDirectories Whether `req.directoryPaths` was non-empty before search.
 * @return Captured and normalized diagnostic output from `verboseFind`.
 */
static std::string captureVerboseFindOutput(ImageSearchRequest& req,
                                            const bool hadDirectories) {

	std::ostringstream verboseFindBuf;
	{
		std::streambuf* oldOut = std::cout.rdbuf(verboseFindBuf.rdbuf());
		std::streambuf* oldErr = std::cerr.rdbuf(verboseFindBuf.rdbuf());
		verboseFind(req.invalidDirectoryPaths, req.directoryPaths, req.processedErrorsFind);
		std::cout.rdbuf(oldOut);
		std::cerr.rdbuf(oldErr);
	}

	std::string output = verboseFindBuf.str();
	if (hadDirectories && output.rfind("\n\n\n", 0) == 0) {
		output.erase(0, 1);
	}
	return output;

}

/**
 * @brief Writes color-coded file discovery and cache summary blocks to `std::cout`.
 *
 * Does nothing when cancellation has been requested via `GlobalState::g_operationCancelled`.
 *
 * @param req            Search results and cache counts to format.
 * @param vt             Theme color and style tokens.
 * @param hasFindContent Whether `verboseFind` produced more than a single character.
 */
static void printDiscoveryStatusBlocks(const ImageSearchRequest& req,
                                       const VerboseAndDatabaseTheme& vt,
                                       const bool hasFindContent) {

	if (GlobalState::g_operationCancelled.load()) {
		return;
	}

	const char* const sectionPrefix = hasFindContent ? "\n\n" : "\n";

	if (!req.fileNames.empty()) {
		std::cout << sectionPrefix
		          << vt.green << req.fileNames.size() << " "
		          << vt.orange << "{" << req.fileExtension << "} "
		          << vt.green << "files found" << vt.yellow << "\n"
		          << req.currentCacheOld << " "
		          << vt.orange << "{" << req.fileExtension << "} "
		          << vt.yellow << "cached entries" << vt.reset << vt.bold << "\n\n";
	}

	if (!req.newFilesFound && !req.files.empty() && !req.list) {
		std::cout << sectionPrefix
		          << vt.red << "0 "
		          << vt.orange << "{" << req.fileExtension << "} "
		          << vt.red << "files found " << vt.yellow << "\n"
		          << req.files.size() << " "
		          << vt.orange << "{" << req.fileExtension << "} "
		          << vt.yellow << "cached entries | "
		          << vt.blue << "ls "
		          << vt.yellow << "↵ to list" << vt.reset << vt.bold << "\n\n";
	}

	if (req.files.empty() && !req.list) {
		std::cout << sectionPrefix
		          << vt.red << "0" << vt.orange << " {" << req.fileExtension << "} "
		          << vt.red << "files found\n"
		          << vt.yellow << "0" << vt.orange << " {" << req.fileExtension << "} "
		          << vt.yellow << "cached entries\n"
		          << vt.reset << vt.bold << "\n";
	}

}

/**
 * @brief Renders the full search summary screen block for resize replay.
 *
 * @param req                 Search metadata and extension/cache state.
 * @param vt                  Theme color and style tokens.
 * @param repaintCallerHeader Optional callback to redraw the caller header first.
 * @param verboseFindOutput   Captured output from `verboseFind`.
 * @param totalProcessed      Snapshotted count of processed files.
 * @param totalElapsedTime    Snapshotted elapsed duration in seconds.
 * @param hadDirectories      Whether valid directories were scanned.
 * @param hasFindContent      Whether `verboseFindOutput` has substantial content.
 */
static void renderImageSearchSummary(const ImageSearchRequest& req,
                                     const VerboseAndDatabaseTheme& vt,
                                     const std::function<void()>& repaintCallerHeader,
                                     const std::string& verboseFindOutput,
                                     const std::size_t totalProcessed,
                                     const double totalElapsedTime,
                                     const bool hadDirectories,
                                     const bool hasFindContent) {

	SyncOutput sync;
	clearScrollBuffer();

	// Reprint the caller's screen header (the FolderPaths prompt line)
	// before the block, so a resize during the confirmation wait doesn't
	// lose the question the user just answered.
	if (repaintCallerHeader) {
		repaintCallerHeader();
	}

	if (hadDirectories) {
		std::cout << "\n" << "\r" << color << "Total files processed: " << totalProcessed << vt.reset;
	}

	std::cout << verboseFindOutput;

	printDiscoveryStatusBlocks(req, vt, hasFindContent);

	std::cout << color << "Time Elapsed: " << std::fixed << std::setprecision(1)
	          << totalElapsedTime << "s" << vt.bold << "\n";

}

/**
 * @brief Renders a color-coded summary of image discovery results and cache
 * deltas.
 *
 * See the header declaration for the full behavioral contract; this
 * definition documents only implementation-level sequencing.
 *
 * Sequencing:
 *   1. Ignore SIGINT and disable Ctrl+D (never restored).
 *   2. Capture whether any valid directory was scanned.
 *   3. Run `verboseFind` exactly once with stdout/stderr redirected into an
 *      `ostringstream`, since it clears its set arguments and we need to
 *      replay its text on resize.
 *   4. Trim one leading newline when the captured text begins with "\n\n\n".
 *   5. Snapshot `totalProcessed`, the elapsed time, and the captured text.
 *   6. Hand a render lambda to `pressEnterToContinueWithRepaint`. The lambda
 *      calls `repaintCallerHeader` first when set, then writes the block.
 *
 * @param req                 Search inputs and in/out sets (see struct docs).
 * @param repaintCallerHeader Optional callback that reprints the caller's
 *                            screen header on a resize during the wait.
 *                            When empty, the block starts from a cleared
 *                            screen with no caller context.
 *
 * @warning `req.invalidDirectoryPaths` and `req.processedErrorsFind` are
 *          cleared unconditionally before this function returns.
 */
void verboseImageSearchResults(ImageSearchRequest& req,
                               const std::function<void()>& repaintCallerHeader) {

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();

	const VerboseAndDatabaseTheme vt = getVerboseTheme();
	const bool hadDirectories = !req.directoryPaths.empty();
	const std::string verboseFindOutput = captureVerboseFindOutput(req, hadDirectories);
	const bool hasFindContent = verboseFindOutput.size() > 1;
	const std::size_t totalProcessed = req.totalFiles.load();

	const double total_elapsed_time =
	    static_cast<double>(monotonicNowNs() - req.start_time_ns) / 1e9;

	auto render = [&] {

		renderImageSearchSummary(req,
		                         vt,
		                         repaintCallerHeader,
		                         verboseFindOutput,
		                         totalProcessed,
		                         total_elapsed_time,
		                         hadDirectories,
		                         hasFindContent);

	};

	pressEnterToContinueWithRepaint(render);
	clearScrollBuffer();

}
