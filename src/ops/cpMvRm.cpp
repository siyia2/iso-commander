// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

import std;
import isocmd.input.lineEditor;
import isocmd.core.globalMutexes;
import isocmd.input.history;
import isocmd.input.inputHandling;
import isocmd.input.terminalControl;
import isocmd.input.promptCompletion;
import isocmd.ops.cpMvRm;
import isocmd.proc.process;
import isocmd.search.search;
import isocmd.ui.display.display;
import isocmd.ui.interface.pagination;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;
import isocmd.util.sort;
import isocmd.util.stringManipulation;

// Declarations used only by this file (moved out of the module interface).
void reportErrorCpMvRm(
    OperationContext& ctx,
    std::string_view errorType,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDir,
    std::string_view errorDetail,
    std::string_view operation
);

namespace fs = std::filesystem;

/**
 * @brief Constructs formatted display entries for selected ISO files.
 *
 * @param indexChunks Chunks of indices pointing to selected files.
 * @param isoFiles Full list of available ISO file paths.
 * @return Formatted strings ready for console output.
 */
std::vector<std::string> generateIsoEntries(
    const std::vector<std::vector<int>>& indexChunks,
    const std::vector<std::string>& isoFiles
)
{

	std::vector<std::string> entries;

	const CpMvRmColors colors = getCpMvRmColors();

	for (const auto& chunk : indexChunks) {
		for (int index : chunk) {
			if (index <= 0 || static_cast<std::size_t>(index) > isoFiles.size()) {
				continue;
			}

			auto [isoDir, filename] =
			    extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");

			std::string entry;
			entry.reserve(isoDir.length() + filename.length() + 64);

			entry += colors.arrow;
			entry += "-> ";

			if (!displayConfig::toggleNamesOnly) {
				entry += colors.dir;
				entry.append(isoDir);
				entry += "/";
			}

			entry += colors.iso;
			entry.append(filename);
			entry += UI::Palette::BoldReset;
			entry += '\n';

			entries.push_back(std::move(entry));
		}
	}

	return entries;

}

/**
 * @brief Validates a Linux filesystem path.
 *
 * Checks absolute format, control characters, blank content, existence,
 * directory type, and read/execute accessibility.
 *
 * @param path Filesystem path to validate.
 * @return Empty if valid, otherwise a color-coded error message.
 */
static std::string validateLinuxPath(const std::string& path)
{

	const CpMvRmColors colors = getCpMvRmColors();

	auto makeError = [&](const std::string& msg) -> std::string {

		return msg + std::string(UI::Palette::PromptBoldAlt);

	};

	if (path.empty() || path[0] != '/') {
		return makeError(
		    std::string(colors.error_label) +
		    "Error: Path " +
		    std::string(colors.error_path) +
		    "'" + path + "'" +
		    std::string(colors.error_label) +
		    " must be absolute (start with '/')."
		);
	}

	for (char c : path) {
		if (std::iscntrl(static_cast<unsigned char>(c))) {
			return makeError(
			    std::string(colors.error_label) +
			    "Error: Control characters in path " +
			    std::string(colors.error_path) +
			    "'" + path + "'" +
			    std::string(colors.error_label) + "."
			);
		}
	}

	if (path.find_first_not_of(" \t") == std::string::npos) {
		return makeError(
		    std::string(colors.error_label) +
		    "Error: Path " +
		    std::string(colors.error_path) +
		    "'" + path + "'" +
		    std::string(colors.error_label) +
		    " is blank."
		);
	}

	struct stat pathStat {};

	if (stat(path.c_str(), &pathStat) != 0) {
		if (errno == EACCES || errno == EPERM) {
			return makeError(
			    std::string(colors.error_label) +
			    "Error: Permission denied accessing path " +
			    std::string(colors.error_path) +
			    "'" + path + "'" +
			    std::string(colors.error_label) + "."
			);
		}

		return makeError(
		    std::string(colors.error_label) +
		    "Error: Path " +
		    std::string(colors.error_path) +
		    "'" + path + "'" +
		    std::string(colors.error_label) +
		    " does not exist."
		);
	}

	if (!S_ISDIR(pathStat.st_mode)) {
		return makeError(
		    std::string(colors.error_label) +
		    "Error: " +
		    std::string(colors.error_path) +
		    "'" + path + "'" +
		    std::string(colors.error_label) +
		    " is not a directory."
		);
	}

	// Verify the directory is actually accessible (readable + executable).
	if (access(path.c_str(), R_OK | X_OK) != 0) {
		return makeError(
		    std::string(colors.error_label) +
		    "Error: Permission denied accessing path " +
		    std::string(colors.error_path) +
		    "'" + path + "'" +
		    std::string(colors.error_label) + "."
		);
	}

	return {};

}

/**
 * @brief Configures line-editor keybindings, signal handlers, and history for a prompt iteration.
 *
 * @param ctx Prompt context containing operation flags and history settings.
 * @param isPageTurn Indicates whether the current line editor invocation is a page turn.
 */
static void configureCpMvLineEditorEnv(CpMvPromptContext& ctx, const bool& isPageTurn)
{

	enable_ctrl_d();
	setupSignalHandlerCancellations();

	RestoreLineEditorBuffer();
	editorBindKeySequence("\033", exit_handler);

	if (!ctx.isCopy) {
		ctx.umountMvRmBreak = true;
	}

	if (!isPageTurn) {
		editorClearHistory();
		ctx.filterHistory = false;
		loadHistory(ctx.filterHistory);
	}

}

/**
 * @brief Builds the formatted line editor prompt suffix for Copy/Move destination selection.
 *
 * @param ctx Prompt context providing operation color and description.
 * @return Formatted prompt string with line editor color escape sequences.
 */
static std::string buildCpMvPromptSuffix(const CpMvPromptContext& ctx)
{

	const CpMvRmColors colors = getCpMvRmColors();

	const std::string green =
	    "\001" + std::string(colors.prompt_green) + "\002";

	const std::string blue =
	    "\001" + std::string(colors.prompt_blue) + "\002";

	const std::string reset =
	    "\001" + std::string(UI::Palette::BoldReset) + "\002";

	std::string prompt(green);
	prompt
	    .append("FolderPaths")
	    .append(blue)
	    .append(" ↵ for selected ")
	    .append(green)
	    .append("ISO")
	    .append(blue)
	    .append(" to be ")
	    .append(ctx.operationColor)
	    .append(ctx.operationDescription)
	    .append(blue)
	    .append(" into, ? for help:\n")
	    .append(reset);
	return prompt;

}

/**
 * @brief Trims leading and trailing horizontal whitespace from a path token in place.
 *
 * @param token Path token string to trim.
 */
static void trimPathToken(std::string& token)
{

	token.erase(0, token.find_first_not_of(" \t"));
	token.erase(token.find_last_not_of(" \t") + 1);

}

/**
 * @brief Parses and validates semicolon-delimited Linux destination paths.
 *
 * Trims each semicolon-separated token, validates it with validateLinuxPath(),
 * and rebuilds a normalized semicolon-joined path string. On the first invalid
 * token, records the error in @p invalidPathError and returns false.
 *
 * @param rawPathsInput Raw path list with any trailing overwrite flag stripped.
 * @param normalizedPaths Output string receiving trimmed, valid paths joined by ';'.
 * @param invalidPathError Output error message when validation fails.
 * @return true if all tokens are valid, false otherwise.
 */
static bool parseAndValidateDestPaths(
    const std::string& rawPathsInput,
    std::string& normalizedPaths,
    std::string& invalidPathError)
{

	normalizedPaths.clear();
	invalidPathError.clear();

	std::istringstream iss(rawPathsInput);
	std::string token;

	while (std::getline(iss, token, ';')) {
		trimPathToken(token);

		std::string err = validateLinuxPath(token);
		if (!err.empty()) {
			invalidPathError = std::move(err);
			return false;
		}

		if (!normalizedPaths.empty()) {
			normalizedPaths += ';';
		}

		normalizedPaths += token;
	}

	return true;

}

/**
 * @brief Handles non-control Copy/Move input, path validation, and history persistence.
 *
 * @param ctx Prompt context updated with validated paths and overwrite flag.
 * @param userInput Raw line returned by paginated line editor display.
 * @param shouldContinue Loop control flag cleared when a valid destination is accepted.
 */
static void processCpMvInput(
    CpMvPromptContext& ctx,
    const std::string& userInput,
    bool& shouldContinue)
{

	if (userInput.empty()) {
		return;
	}

	const bool hasOverwriteFlag =
	    userInput.size() >= 3 &&
	    userInput.substr(userInput.size() - 3) == " -o";

	const std::string rawPathsInput = hasOverwriteFlag
	    ? userInput.substr(0, userInput.size() - 3)
	    : userInput;

	std::string pathsInput;
	std::string invalidPathError;

	if (!parseAndValidateDestPaths(rawPathsInput, pathsInput, invalidPathError)) {
		verboseSets.uniqueErrorTokenMessages.insert(invalidPathError);
		ctx.userDestDir.clear();
		resetLineEditorPagination();
		return;
	}

	// Input such as " -o" leaves no paths at all.
	if (pathsInput.empty()) {
		ctx.userDestDir.clear();
		return;
	}

	ctx.overwriteExisting = hasOverwriteFlag;
	ctx.userDestDir = pathsInput;

	editorAddHistory(pathsInput);
	saveHistory(ctx.filterHistory);

	shouldContinue = false;

}

/**
 * @brief Runs a single Copy/Move prompt iteration and updates loop state.
 *
 * @param ctx Prompt context.
 * @param entries Sorted ISO entry list for display.
 * @param currentPage Current pagination page index.
 * @param shouldContinue Loop control flag updated on exit, cancel, or completion.
 */
static void handleCpMvIteration(
    CpMvPromptContext& ctx,
    const std::vector<std::string>& entries,
    std::size_t& currentPage,
    bool& shouldContinue)
{

	bool isPageTurn = false;
	auto setupEnv = [&]() {

		configureCpMvLineEditorEnv(ctx, isPageTurn);

	};

	const std::string promptPrefix = "\n";
	const std::string promptSuffix = buildCpMvPromptSuffix(ctx);

	std::string userInput;
	{
		EditorKeyBindGuard keyGuard;
		EditorStartupHookGuard hookGuard;

		userInput = handlePaginatedDisplay(
		    entries,
		    promptPrefix,
		    promptSuffix,
		    setupEnv,
		    isPageTurn,
		    currentPage,
		    false
		);
	}

	if (userInput == "EOF_SIGNAL") {
		shouldContinue = false;
		return;
	}

	if (userInput == "?") {
		constexpr bool import2ISO = false;
		constexpr bool isCpMv = true;

		helpSearches(isCpMv, import2ISO, ctx.isCopy);
		ctx.userDestDir.clear();
		return;
	}

	if (userInput == "\x1b") {
		ctx.umountMvRmBreak = false;
		ctx.userDestDir.clear();
		editorClearHistory();
		shouldContinue = false;
		return;
	}

	processCpMvInput(ctx, userInput, shouldContinue);

}

/**
 * @brief Prompts for destination directories or routes to deletion confirmation.
 *
 * In Copy/Move mode, validates Linux paths, handles pagination, processes
 * overwrite flags (-o), and manages line-editor history.
 *
 * In Delete mode, delegates to handleDeleteOperation().
 *
 * @param ctx Prompt context.
 * @return Validated destination path(s), or an empty string if canceled.
 */
std::string userDestDirCpMv(CpMvPromptContext& ctx)
{

	PendingLineEditorTextClearGuard pendingTextGuard;

	editorCompletionHook = nullptr;

	std::vector<std::string> entries =
	    generateIsoEntries(ctx.indexChunks, ctx.isoFiles);

	sortFilesCaseInsensitive(entries);
	clearScrollBuffer();

	reset_custom_keybindingsForCpMvWrite2Usb();

	// Restore the previous line editor completion mode on every exit path.
	EditorCompletionModeGuard completeModeGuard;

	bool shouldContinue = true;
	std::size_t currentPage = 0;

	while (shouldContinue) {
		resetLineEditorPagination();
		RetainAndRestoreLineEditorBuffer::g_editorCompletionMode = 1;

		if (!ctx.isDelete) {
			handleCpMvIteration(ctx, entries, currentPage, shouldContinue);
		} else {
			const bool proceedWithDelete = handleDeleteOperation(
			    ctx.isoFiles,
			    ctx.indexChunks,
			    ctx.umountMvRmBreak,
			    ctx.abortDel
			);

			if (!proceedWithDelete) {
				ctx.userDestDir.clear();
			}

			shouldContinue = false;
		}
	}

	resetLineEditorPagination();
	return ctx.userDestDir;

}

/**
 * @brief Reports a categorized copy/move/delete error.
 *
 * Builds a color-coded error message, adds it to the reporter, increments
 * failedTasks, and marks the operation unsuccessful.
 */
void reportErrorCpMvRm(
    OperationContext& ctx,
    std::string_view errorType,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDir,
    std::string_view errorDetail,
    std::string_view operation
)
{

	const auto vt = getVerboseTheme();
	const std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

	std::string errorMsg;
	errorMsg.reserve(256 + displaySrc.size() + errorDetail.size());

	errorMsg.append(vt.red).append("✗ ");
	if (errorType == "same_file") {
		errorMsg.append(vt.yellow).append(srcDir).append("/").append(srcFile)
		        .append(vt.red).append(": Cannot ").append(operation).append(" file to itself");
	} else if (errorType == "source_missing") {
		errorMsg.append(vt.yellow).append(displaySrc)
		        .append(vt.red).append(": Source file no longer exists");
	} else if (errorType == "overwrite_failed") {
		errorMsg.append(vt.yellow).append(destDir).append(srcFile)
		        .append(vt.red).append(": Failed to overwrite: ").append(errorDetail);
	} else if (errorType == "file_exists") {
		errorMsg.append(vt.yellow).append(displaySrc)
		        .append(vt.reset).append(" → ")
		        .append(vt.yellow).append(destDir).append(srcFile)
		        .append(vt.red).append(": File exists (overwrite with -o)");
	} else if (errorType == "remove_after_move") {
		errorMsg.append(vt.yellow).append(displaySrc)
		        .append(vt.red).append(": Move completed but failed to remove source file: ")
		        .append(errorDetail);
	} else if (errorType == "missing_file") {
		errorMsg.append(vt.yellow).append(displaySrc)
		        .append(vt.purple).append(": Missing");
	} else {
		errorMsg.append(errorDetail);
	}
	errorMsg.append(vt.reset);

	ctx.reporter.addError(std::move(errorMsg));

	if (ctx.failedTasks) {
		ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
	}

	ctx.operationSuccessful.store(false, std::memory_order_release);

}

/**
 * @brief Logs the final result of a move or copy operation.
 *
 * Updates the reporter and the operation-context counters.
 */
void logOperationResult(
    OperationContext& ctx,
    bool success,
    bool cancelled,
    const std::error_code& ec,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDirProcessed,
    std::string_view destFile
)
{

	const CpMvRmColors colors = getCpMvRmColors();
	const std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

	if (!success || ec) {
		const std::string errorDetail =
		    cancelled ? "Cancelled" : ec.message();

		std::string msg;
		msg.reserve(
		    128 + displaySrc.size() +
		    destDirProcessed.size() +
		    errorDetail.size()
		);

		msg.append(colors.error_label).append("✗ ")
		   .append(colors.error_path).append(displaySrc)
		   .append(colors.arrow).append(" → ")
		   .append(colors.error_path).append(destDirProcessed).append(destFile)
		   .append(colors.error_label).append(": ").append(errorDetail)
		   .append(UI::Palette::BoldReset);

		ctx.reporter.addError(std::move(msg));

		if (ctx.failedTasks) {
			ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
		}

		ctx.operationSuccessful.store(false, std::memory_order_release);
	} else {
		std::string msg;
		msg.reserve(128 + displaySrc.size() + destDirProcessed.size() + destFile.size());

		msg.append(colors.success_path).append("✓ ").append(displaySrc)
		   .append(colors.success_label).append(" → ")
		   .append(colors.dest_path).append(destDirProcessed).append(destFile)
		   .append(UI::Palette::BoldReset);

		ctx.reporter.addSuccess(std::move(msg));

		if (ctx.completedTasks) {
			ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
		}
	}

	// Batching happens automatically inside reporter.addSuccess/addError.

}

namespace {

	/**
	 * @brief Parses a semicolon-separated list of destination directories.
	 *
	 * Empty entries produced by a trailing semicolon are preserved, matching
	 * the original inline parsing behaviour.
	 *
	 * @param userDestDir Semicolon-separated destination directory string.
	 * @return Normalized destination directory strings in input order.
	 */
	std::vector<std::string> parseDestinationDirs(
	    const std::string& userDestDir
	)
	{

		std::vector<std::string> destDirs;

		std::istringstream iss(userDestDir);
		std::string destDir;

		while (std::getline(iss, destDir, ';')) {
			destDirs.push_back(fs::path(destDir).string());
		}

		return destDirs;

	}

	/**
	 * @brief Checks whether a source/destination pair may proceed.
	 *
	 * Checks same-file, source-missing, and destination-exists conditions.
	 * Existing destinations are not removed here; overwrite is performed by
	 * the eventual copy/rename operation.
	 *
	 * @param ctx Operation context for error reporting.
	 * @param params Destination-check parameters.
	 * @return True if the operation may proceed.
	 */
	bool isDestinationReadyForCopyMove(
	    OperationContext& ctx,
	    DestinationCheckParams& params
	)
	{

		const TransferPaths& paths = params.paths;

		if (fs::absolute(paths.srcPath) == fs::absolute(paths.destPath)) {
			const std::string_view opName =
			    (params.op == TransferOp::Move) ? "move" : "copy";

			reportErrorCpMvRm(
			    ctx, "same_file",
			    paths.srcDir, paths.srcFile,
			    "", "", opName
			);

			return false;
		}

		++params.state.validDestinations;

		if (!fs::exists(paths.srcPath)) {
			reportErrorCpMvRm(
			    ctx, "source_missing",
			    paths.srcDir, paths.srcFile,
			    "", "", ""
			);

			return false;
		}

		if (!fs::exists(paths.destPath)) {
			return true;
		}

		if (!params.overwriteExisting) {
			const std::string_view opName =
			    (params.op == TransferOp::Copy) ? "copying" : "moving";

			reportErrorCpMvRm(
			    ctx, "file_exists",
			    paths.srcDir, paths.srcFile,
			    paths.destDirProcessed, "", opName
			);

			return false;
		}

		// The existing destination is replaced only after a successful copy.
		return true;

	}

	/**
	 * @brief Validates and dispatches one copy/move operation.
	 *
	 * Multi-destination moves use buffered copies; the caller removes the source
	 * once at least one destination copy succeeds.
	 */
	void processCopyMoveDestination(
	    OperationContext& ctx,
	    DestinationCheckParams& params
	)
	{

		if (!isDestinationReadyForCopyMove(ctx, params)) {
			return;
		}

		const TransferPaths& paths = params.paths;

		bool success = false;

		if (params.op == TransferOp::Move && params.destDirCount > 1) {
			success = performBufferedCopyOperation(ctx, paths);

			if (success) {
				params.state.atLeastOneCopySucceeded = true;
			}
		} else if (params.op == TransferOp::Move) {
			success = performMoveOperation(ctx, paths, params.fileSize);
		} else {
			success = performBufferedCopyOperation(ctx, paths);
		}

		if (success && params.successfulDestPaths && params.destPathsMutex) {
			std::lock_guard<std::mutex> lock(*params.destPathsMutex);
			params.successfulDestPaths->push_back(paths.destPath.string());
		}

	}

	/**
	 * @brief Removes the source after a successful multi-destination move.
	 *
	 * The source is removed only when at least one destination was valid and at
	 * least one copy succeeded.
	 */
	void removeSourceAfterMultiDestMove(
	    OperationContext& ctx,
	    const fs::path& srcPath,
	    std::string_view srcDir,
	    std::string_view srcFile,
	    TransferOp op,
	    std::size_t destDirCount,
	    const MultiDestState& state
	)
	{

		if (!(op == TransferOp::Move &&
		      destDirCount > 1 &&
		      state.validDestinations > 0 &&
		      state.atLeastOneCopySucceeded)) {
			return;
		}

		std::error_code deleteEc;

		if (!fs::remove(srcPath, deleteEc)) {
			reportErrorCpMvRm(
			    ctx,
			    "remove_after_move",
			    srcDir,
			    srcFile,
			    "",
			    deleteEc.message(),
			    ""
			);
		}

	}

} // namespace

/**
 * @brief Handles copy, move, or delete operations for a chunk of ISO files.
 *
 * Creates a batch reporter and operation context, processes each selected
 * file, restores ownership of newly created files to the invoking user,
 * and flushes pending report messages before returning.
 *
 * @param options File-operation options.
 * @param chunk 1-based indices into options.isoFiles.
 * @param progress Progress and accounting object.
 */
void handleIsoFileOperation(
    const FileOperationOptions& options,
    const std::vector<int>& chunk,
    FileOperationProgress& progress
)
{

	// Thread-local batched reporter.
	BatchReporter reporter(
	    verboseSets.operationCompleted,
	    verboseSets.operationFailed,
	    GlobalMutexes::globalSetsMutex,
	    50
	);

	std::atomic<bool> operationSuccessful(true);

	// Resolve the real user for ownership changes.
	uid_t real_uid;
	gid_t real_gid;

	std::string real_username;
	std::string real_groupname;

	getRealUserId(
	    real_uid,
	    real_gid,
	    real_username,
	    real_groupname
	);

	OperationContext ctx{
		.reporter = reporter,
		.completedTasks = &progress.completedTasks,
		.failedTasks = &progress.failedTasks,
		.operationSuccessful = operationSuccessful,
		.changeOwnership = [&](const fs::path& path) {

			chown(path.c_str(), real_uid, real_gid);

		},
		.completedBytes = &progress.completedBytes,
	};

	// Parse destinations.
	const std::vector<std::string> destDirs =
	    parseDestinationDirs(options.userDestDir);

	const TransferOp op =
	    options.isMove ? TransferOp::Move : TransferOp::Copy;

	// Process each selected file.
	for (const int chunkIdx : chunk) {
		const std::string& iso = options.isoFiles[chunkIdx - 1];

		fs::path srcPath(iso);

		if (!fs::exists(srcPath)) {
			auto [isoDir, isoFile] =
			    extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");

			reportErrorCpMvRm(
			    ctx, "missing_file",
			    isoDir, isoFile,
			    "", "", ""
			);

			continue;
		}

		auto [srcDir, srcFile] =
		    extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");

		struct stat st {};

		const std::size_t fileSize =
		    (stat(srcPath.c_str(), &st) == 0)
		        ? static_cast<std::size_t>(st.st_size)
		        : 0;

		if (options.isDelete) {
			performDeleteOperation(
			    ctx,
			    srcPath,
			    srcDir,
			    srcFile,
			    fileSize
			);

			continue;
		}

		MultiDestState state;

		for (const auto& dst : destDirs) {
			fs::path destPath = fs::path(dst) / srcPath.filename();

			auto [_, destFile] =
			    extractDirectoryAndFilename(destPath.native(), "cp_mv_rm");

			DestinationCheckParams destParams{
				.paths = {
					.srcPath = srcPath,
					.destPath = destPath,
					.srcDir = srcDir,
					.srcFile = srcFile,
					.destDirProcessed = dst,
					.destFile = destFile,
				},
				.op = op,
				.overwriteExisting = options.overwriteExisting,
				.fileSize = fileSize,
				.destDirCount = destDirs.size(),
				.state = state,
				.successfulDestPaths = &progress.successfulDestPaths,
				.destPathsMutex = &progress.destPathsMutex,
			};

			processCopyMoveDestination(ctx, destParams);
		}

		// Multi-destination move cleanup.
		removeSourceAfterMultiDestMove(
		    ctx,
		    srcPath,
		    srcDir,
		    srcFile,
		    op,
		    destDirs.size(),
		    state
		);
	}

	// Flush any remaining batched messages.
	reporter.flush();

}

std::string buildDisplaySrc(std::string_view srcDir, std::string_view srcFile) {

	if (!displayConfig::toggleNamesOnly) {
		std::string s;
		s.reserve(srcDir.size() + srcFile.size() + 1);
		s.append(srcDir).append("/").append(srcFile);
		return s;
	}
	return std::string(srcFile);

}

EditorFormFeedGuard::~EditorFormFeedGuard() {

	editorBindKey('\f', ignoreEditorKeybinding);

}

EditorKeyBindGuard::~EditorKeyBindGuard() {

	editorBindKey('\f', ignoreEditorKeybinding);
	editorBindKey('\t', ignoreEditorKeybinding);
	editorBindKeySequence("\033", exit_handler);

}

EditorCompletionModeGuard::EditorCompletionModeGuard() : saved(RetainAndRestoreLineEditorBuffer::g_editorCompletionMode) {}

EditorCompletionModeGuard::~EditorCompletionModeGuard() {

	RetainAndRestoreLineEditorBuffer::g_editorCompletionMode = saved;

}

EditorStartupHookGuard::~EditorStartupHookGuard() {

	editorStartupHook = nullptr;

}

BatchReporter::BatchReporter(std::unordered_set<std::string>& globalIsos,
              std::unordered_set<std::string>& globalErrors,
              std::mutex& mutex,
              std::size_t batchSize) : globalIsos_(globalIsos), globalErrors_(globalErrors),
      mutex_(mutex), batchSize_(batchSize) {}

void BatchReporter::addSuccess(std::string msg) {

	localIsos_.push_back(std::move(msg));
	tryFlush();

}

void BatchReporter::addError(std::string msg) {

	localErrors_.push_back(std::move(msg));
	tryFlush();

}

void BatchReporter::flush() {

	if (!localIsos_.empty() || !localErrors_.empty()) {
		std::lock_guard<std::mutex> lock(mutex_);
		globalIsos_.insert(std::make_move_iterator(localIsos_.begin()),
		                  std::make_move_iterator(localIsos_.end()));
		globalErrors_.insert(std::make_move_iterator(localErrors_.begin()),
		                     std::make_move_iterator(localErrors_.end()));
		localIsos_.clear();
		localErrors_.clear();
	}

}

void BatchReporter::tryFlush() {

	if (localIsos_.size() >= batchSize_ || localErrors_.size() >= batchSize_)
	    flush();

}
