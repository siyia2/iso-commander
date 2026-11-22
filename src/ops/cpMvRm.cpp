// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "isocmd/ops/cpMvRm.h"
#include "isocmd/core/globalMutexes.h"
#include "isocmd/core/state.h"
#include "isocmd/input/history.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/input/readline.h"
#include "isocmd/ops/process.h"
#include "isocmd/ui/themes.h"
#include "isocmd/ui/verbose.h"
#include "isocmd/util/sort.h"
#include "isocmd/util/stringManipulation.h"

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
        return msg + std::string(UI::Palette::RL_BoldAlt);
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
        if (iscntrl(static_cast<unsigned char>(c))) {
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
 * @brief Prompts for destination directories or routes to deletion confirmation.
 *
 * In Copy/Move mode, validates Linux paths, handles pagination, processes
 * overwrite flags (-o), and manages Readline history.
 *
 * In Delete mode, delegates to handleDeleteOperation().
 *
 * @param ctx Prompt context.
 * @return Validated destination path(s), or an empty string if canceled.
 */
std::string userDestDirCpMv(CpMvPromptContext& ctx)
{
    PendingReadlineTextClearGuard pendingTextGuard;

    rl_attempted_completion_function = nullptr;

    std::vector<std::string> entries =
        generateIsoEntries(ctx.indexChunks, ctx.isoFiles);

    sortFilesCaseInsensitive(entries);
    clearScrollBuffer();

    reset_custom_keybindingsForCpMvWrite2Usb();

    // Restore the previous Readline completion mode on every exit path.
    RlCompleteModeGuard completeModeGuard;

    bool shouldContinue = true;
    std::string userInput;

    std::size_t currentPage = 0;

    while (shouldContinue) {
        resetReadlinePagination();
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 1;

        if (!ctx.isDelete) {
            bool isPageTurn = false;

            auto setupEnv = [&]() {
                enable_ctrl_d();
                setupSignalHandlerCancellations();

                RestoreReadlineBuffer();
                rl_bind_keyseq("\\e", exit_handler);

                if (!ctx.isCopy) {
                    ctx.umountMvRmBreak = true;
                }

                if (!isPageTurn) {
                    clear_history();
                    ctx.filterHistory = false;
                    loadHistory(ctx.filterHistory);
                }
            };

            const CpMvRmColors colors = getCpMvRmColors();

            const std::string green =
                "\001" + std::string(colors.prompt_green) + "\002";

            const std::string blue =
                "\001" + std::string(colors.prompt_blue) + "\002";

            const std::string reset =
                "\001" + std::string(UI::Palette::BoldReset) + "\002";

            const std::string promptPrefix = "\n";

            const std::string promptSuffix = std::format(
                "{0}FolderPaths{1} ↵ for selected {0}ISO{1} to be {2}{3}{1} into, ? for help:\n{4}",
                green,
                blue,
                ctx.operationColor,
                ctx.operationDescription,
                reset
            );

            {
                RlKeyBindGuard keyGuard;
                RlStartupHookGuard hookGuard;

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
                break;
            }

            if (userInput == "?") {
                constexpr bool import2ISO = false;
                constexpr bool isCpMv = true;

                helpSearches(isCpMv, import2ISO, ctx.isCopy);

                ctx.userDestDir.clear();
                continue;
            }

            if (userInput == "\x1b") {
                ctx.umountMvRmBreak = false;
                ctx.userDestDir.clear();
                clear_history();
                shouldContinue = false;
                continue;
            }

            if (userInput.empty()) {
                continue;
            }

            const bool hasOverwriteFlag =
                userInput.size() >= 3 &&
                userInput.substr(userInput.size() - 3) == " -o";

            const std::string rawPathsInput = hasOverwriteFlag
                ? userInput.substr(0, userInput.size() - 3)
                : userInput;

            bool pathsValid = true;
            std::string invalidPathError;

            // Trimmed tokens are what get validated, so they are also what
            // gets stored; otherwise "/a; /b" would validate but later be
            // used as " /b".
            std::string pathsInput;

            std::istringstream iss(rawPathsInput);
            std::string token;

            while (std::getline(iss, token, ';')) {
                token.erase(0, token.find_first_not_of(" \t"));
                token.erase(token.find_last_not_of(" \t") + 1);

                std::string err = validateLinuxPath(token);

                if (!err.empty()) {
                    pathsValid = false;
                    invalidPathError = std::move(err);
                    break;
                }

                if (!pathsInput.empty()) {
                    pathsInput += ';';
                }

                pathsInput += token;
            }

            if (!pathsValid) {
                verboseSets.uniqueErrorTokenMessages.insert(invalidPathError);
                ctx.userDestDir.clear();
                resetReadlinePagination();
                continue;
            }

            // Input such as " -o" leaves no paths at all.
            if (pathsInput.empty()) {
                ctx.userDestDir.clear();
                continue;
            }

            ctx.overwriteExisting = hasOverwriteFlag;
            ctx.userDestDir = pathsInput;

            add_history(pathsInput.c_str());
            saveHistory(ctx.filterHistory);

            shouldContinue = false;
        } else {
            const bool proceedWithDelete = handleDeleteOperation(
                ctx.isoFiles,
                ctx.indexChunks,
                ctx.umountMvRmBreak,
                ctx.abortDel
            );

            if (!proceedWithDelete) {
                ctx.userDestDir.clear();
                shouldContinue = false;
                continue;
            }

            shouldContinue = false;
        }
    }

    resetReadlinePagination();
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

    if (errorType == "same_file") {
        errorMsg.append(vt.red)
                .append("Cannot ")
                .append(operation)
                .append(" file to itself: ")
                .append(vt.red)
                .append("'")
                .append(vt.yellow)
                .append(srcDir)
                .append("/")
                .append(srcFile)
                .append(vt.red)
                .append("'")
                .append(vt.reset)
                .append(vt.red)
                .append(".")
                .append(vt.reset);
    } else if (errorType == "source_missing") {
        errorMsg.append(vt.red)
                .append("Source file no longer exists: ")
                .append(vt.red)
                .append("'")
                .append(vt.yellow)
                .append(displaySrc)
                .append(vt.red)
                .append("'")
                .append(vt.reset)
                .append(vt.red)
                .append(".")
                .append(vt.reset);
    } else if (errorType == "overwrite_failed") {
        errorMsg.append(vt.red)
                .append("Failed to overwrite: ")
                .append(vt.red)
                .append("'")
                .append(vt.yellow)
                .append(destDir)
                .append(srcFile)
                .append(vt.red)
                .append("'")
                .append(vt.reset)
                .append(vt.red)
                .append(" - ")
                .append(errorDetail)
                .append(".")
                .append(vt.reset);
    } else if (errorType == "file_exists") {
        errorMsg.append(vt.red)
                .append("Error ")
                .append(operation)
                .append(": '")
                .append(vt.red)
                .append(vt.yellow)
                .append(displaySrc)
                .append(vt.red)
                .append(vt.reset)
                .append(vt.red)
                .append(" → ")
                .append(vt.yellow)
                .append(destDir)
                .append(srcFile)
                .append(vt.red)
                .append("': File exists ")
                .append("(overwrite with -o")
                .append(vt.reset)
                .append(vt.red)
                .append(").")
                .append(vt.reset);
    } else if (errorType == "remove_after_move") {
        errorMsg.append(vt.red)
                .append("Move completed but failed to remove source file: ")
                .append(vt.red)
                .append("'")
                .append(vt.yellow)
                .append(displaySrc)
                .append(vt.red)
                .append("'")
                .append(vt.reset)
                .append(vt.red)
                .append(" - ")
                .append(errorDetail)
                .append(".")
                .append(vt.reset);
    } else if (errorType == "missing_file") {
        errorMsg.append(vt.purple)
                .append("Missing: ")
                .append(vt.purple)
                .append("'")
                .append(vt.yellow)
                .append(displaySrc)
                .append(vt.purple)
                .append("'.");
    } else {
        errorMsg.append(vt.red)
                .append("Error: ")
                .append(errorDetail)
                .append(vt.reset);
    }

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
    std::string_view verb,
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

        msg.append(colors.error_label)
           .append("Error ")
           .append(verb)
           .append(": '")
           .append(colors.error_path)
           .append(displaySrc)
           .append(UI::Palette::BoldReset)
           .append(colors.error_label)
           .append(" → ")
           .append(colors.error_path)
           .append(destDirProcessed)
           .append(destFile)
           .append(colors.error_label)
           .append("': ")
           .append(errorDetail)
           .append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addError(std::move(msg));

        if (ctx.failedTasks) {
            ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }

        ctx.operationSuccessful.store(false, std::memory_order_release);
    } else {
        const std::string_view pastVerb =
            (verb == "moving") ? "Moved" : "Copied";

        std::string msg;

        msg.reserve(
            3 * colors.success_label.size() +
            pastVerb.size() +
            colors.success_path.size() +
            displaySrc.size() +
            UI::Palette::BoldReset.size() +
            colors.dest_path.size() +
            destDirProcessed.size() +
            destFile.size() +
            16
        );

        msg.append(colors.success_label)
           .append(pastVerb)
           .append(": '")
           .append(colors.success_path)
           .append(displaySrc)
           .append(UI::Palette::BoldReset)
           .append(colors.success_label)
           .append(" → ")
           .append(colors.dest_path)
           .append(destDirProcessed)
           .append(destFile)
           .append(colors.success_label)
           .append("'.");

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
        success = performBufferedCopyOperation(ctx, paths, "moving");

        if (success) {
            params.state.atLeastOneCopySucceeded = true;
        }
    } else if (params.op == TransferOp::Move) {
        success = performMoveOperation(ctx, paths, params.fileSize);
    } else {
        success = performBufferedCopyOperation(ctx, paths, "copying");
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
