// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "isocmd/ops/cpMvRm.h"
#include "isocmd/ui/display.h"
#include "isocmd/core/globalMutexes.h"
#include "isocmd/input/history.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/ops/process.h"
#include "isocmd/input/readline.h"
#include "isocmd/util/sort.h"
#include "isocmd/core/state.h"
#include "isocmd/util/stringManipulation.h"
#include "isocmd/ui/themes.h"
#include "isocmd/ui/verbose.h"

/**
 * @brief Constructs a list of formatted strings representing ISO files for display.
 * @details Extracts filenames and directories, applying theme-specific colors and
 * formatting based on global display configurations.
 *
 * @param indexChunks Chunks of indices pointing to selected files.
 * @param isoFiles The full list of available ISO file paths.
 * @return A vector of formatted strings ready for console output.
 */
static std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
    std::vector<std::string> entries;

    const CpMvRmColors colors = getCpMvRmColors();

    for (const auto& chunk : indexChunks) {
        for (int index : chunk) {
            if (index <= 0 || static_cast<size_t>(index) > isoFiles.size()) continue;

            auto [isoDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");

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
 * @brief Validates a Linux filesystem path for absolute format, control characters,
 *        blank content, existence, directory type, and read/execute accessibility.
 * @details Rejects paths that are not absolute, contain control characters, are
 * whitespace-only, do not exist, exist but are not directories, or cannot be
 * read+executed by the current user.
 *
 * @param path The string representing the filesystem path to check.
 * @return An empty string if valid, or a color-coded error message if invalid.
 */
static std::string validateLinuxPath(const std::string& path) {
    const CpMvRmColors colors = getCpMvRmColors();
    auto makeError = [&](const std::string& msg) -> std::string {
        return msg + std::string(UI::Palette::RL_BoldAlt);
    };

    if (path.empty() || path[0] != '/')
        return makeError(std::string(colors.error_label) + "Error: Path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + " must be absolute (start with '/').");

    for (char c : path)
        if (iscntrl(static_cast<unsigned char>(c)))
            return makeError(std::string(colors.error_label) + "Error: Control characters in path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + ".");

    if (path.find_first_not_of(" \t") == std::string::npos)
        return makeError(std::string(colors.error_label) + "Error: Path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + " is blank.");

    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0) {
        if (errno == EACCES || errno == EPERM)
            return makeError(std::string(colors.error_label) + "Error: Permission denied accessing path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + ".");
        return makeError(std::string(colors.error_label) + "Error: Path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + " does not exist.");
    }

    if (!S_ISDIR(pathStat.st_mode))
        return makeError(std::string(colors.error_label) + "Error: " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + " is not a directory.");

    // Verify the directory is actually accessible (readable + executable)
    if (access(path.c_str(), R_OK | X_OK) != 0)
        return makeError(std::string(colors.error_label) + "Error: Permission denied accessing path " + std::string(colors.error_path) + "'" + path + "'" + std::string(colors.error_label) + ".");

    return "";
}

/**
 * @brief Renders a paginated list of entries and captures a single line of user input.
 * @details Defined in a separate translation unit. The caller supplies a setup
 * callback invoked once per prompt to configure the Readline environment
 * (key bindings, buffers, signal handlers). Pagination turns set @p isPageTurn
 * so the caller can distinguish a page flip from a real input.
 *
 * @param entries               Pre-formatted display lines to paginate.
 * @param promptPrefix          Text printed before the prompt.
 * @param promptSuffix          Text printed after the prompt.
 * @param setupEnvironmentFn    Callback to configure the Readline environment.
 * @param isPageTurn            [out] True when the returned input was a page turn.
 * @param currentPage           [in/out] Current page index; updated on page turns.
 * @param disableTabCompletion  If true, tab completion is suppressed.
 * @return The captured input, or "EOF_SIGNAL" on EOF.
 * @note This is a forward declaration; the definition (and its canonical
 *       documentation) lives elsewhere.
 */
std::string handlePaginatedDisplay(const std::vector<std::string>& entries,
                                   const std::string& promptPrefix,
                                   const std::string& promptSuffix,
                                   const std::function<void()>& setupEnvironmentFn,
                                   bool& isPageTurn,
                                   size_t& currentPage,
                                   bool disableTabCompletion = false);

/**
 * @brief Orchestrates user confirmation for permanent file deletion.
 *
 * Renders the paginated ISO list via handlePaginatedDisplay and captures a Y/N
 * confirmation. Configures the Readline environment for the delete prompt
 * (Ctrl+L redraw, ESC exit handler). The '\f' key binding is restored
 * automatically via RlKeyBindGuard on every exit path, including early returns
 * and EOF. Pagination turns loop silently without treating the input as a
 * confirmation.
 *
 * @param isoFiles        The full list of ISO file paths to be shown and deleted.
 * @param indexChunks     Chunks used for paginated list display formatting.
 *                        Not modified by this function.
 * @param umountMvRmBreak [out] Set to true when the user confirms ('Y'/'y');
 *                        set to false on any cancel or EOF path.
 * @param abortDel        [out] Set to true when the user explicitly cancels
 *                        (non-Y input) or sends EOF; left unchanged on 'Y'.
 * @return True if the user confirmed with 'Y' or 'y', false on any other input,
 *         cancellation, or EOF.
 */
static bool handleDeleteOperation(const std::vector<std::string>& isoFiles,
                           const std::vector<std::vector<int>>& indexChunks,
                           bool& umountMvRmBreak,
                           bool& abortDel) {
    PendingReadlineTextClearGuard pendingTextGuard;

    rl_attempted_completion_function = nullptr;

    reset_custom_keybindingsForRm();
    rl_bind_keyseq("\\e", exit_handler);
    const CpMvRmColors colors = getCpMvRmColors();
    std::string green = "\001" + std::string(colors.prompt_green) + "\002";
    std::string blue  = "\001" + std::string(colors.prompt_blue) + "\002";
    std::string red   = "\001" + std::string(UI::Palette::Red) + "\002";
    std::string reset = "\001" + std::string(UI::Palette::BoldReset) + "\002";
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);

    std::string promptPrefix = "\n";
    std::string promptSuffix = std::format(
        "{0}The selected {1}ISO{0} will be {2}*PERMANENTLY DELETED*{0}. Proceed? (y/n): {3}",
        blue,           // {0}
        green,          // {1}
        red,            // {2}
        reset           // {3}
    );

    bool isPageTurn = false;
    auto setupEnv = [&]() {
        RestoreReadlineBuffer();

        rl_bind_key('\f', [](int count, int key) -> int {
            clear_screen_and_buffer(count, key);
            rl_on_new_line();
            rl_replace_line("", 0);
            rl_done = 1;
            return 0;
        });
    };
    size_t currentPage = 0;
    while (true) {
        std::string userInput;
        {
            RlKeyBindGuard keyGuard; // restores '\f', '\t' and Esc bindings on scope exit
            userInput = handlePaginatedDisplay(
                entries, promptPrefix, promptSuffix, setupEnv, isPageTurn, currentPage, true
            );
        }
        if (userInput == "EOF_SIGNAL") {
            umountMvRmBreak = false;
            abortDel = true;
            return false;
        }
        if (!isPageTurn && !userInput.empty()) {
            std::string choice = trimWhitespace(userInput);
            if (choice == "Y" || choice == "y") {
                umountMvRmBreak = true;
                return true;
            } else {
                umountMvRmBreak = false;
                abortDel = true;
                return false;
            }
        }
    }
}

/**
 * @brief Displays context-sensitive help for search / CP-MV flows.
 * @details Forward declaration; defined elsewhere.
 *
 * @param isCpMv     True when the help is for the copy/move flow.
 * @param import2ISO True when the help is for the import-to-ISO flow.
 * @param isCopy     True for copy (vs move).
 */
void helpSearches(bool isCpMv, bool import2ISO, bool isCopy);

/**
 * @brief Prompts for destination directories or routes to deletion confirmation.
 *
 * In Copy/Move mode: Validates Linux paths, handles pagination, processes
 * overwrite flags (-o), and manages Readline history. Readline state
 * (key bindings, completion mode, startup hook) is restored automatically
 * via RAII guards on every exit path.
 * In Delete mode: Proxies the request to handleDeleteOperation.
 *
 * @param ctx  Prompt context. Relevant fields:
 *             - `indexChunks`         chunks used for paginated list display formatting
 *             - `isoFiles`            list of source ISO files
 *             - `isDelete`            if true, routes to handleDeleteOperation
 *             - `isCopy`              distinguishes copy (true) from move (false)
 *             - `operationColor`      ANSI color applied to the operation label
 *             - `operationDescription` human-readable verb ("copy", "move", "delete")
 *             - `filterHistory`       [in/out] Readline history filter flag
 *             - `umountMvRmBreak`     [in/out] controls parent-loop unmount/break
 *             - `abortDel`            [out] set true when deletion is canceled
 *             - `overwriteExisting`   [out] set true when "-o" flag was supplied
 *             - `userDestDir`         [out] validated destination path(s); empty if canceled
 * @return `ctx.userDestDir` after the operation; empty string if canceled, or
 *         empty on the delete path (result conveyed via `ctx.abortDel`).
 */
std::string userDestDirCpMv(CpMvPromptContext& ctx) {
    PendingReadlineTextClearGuard pendingTextGuard;
    rl_attempted_completion_function = nullptr;
    std::vector<std::string> entries = generateIsoEntries(ctx.indexChunks, ctx.isoFiles);
    sortFilesCaseInsensitive(entries);
    clearScrollBuffer();

    reset_custom_keybindingsForCpMvWrite2Usb();

    // Restore g_rl_complete_mode to whatever it was when we return, throw, or
    // break out of the loop — regardless of which exit path is taken.
    RlCompleteModeGuard completeModeGuard;

    bool shouldContinue = true;
    std::string userInput;

    size_t currentPage = 0;

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

                if (!ctx.isCopy) ctx.umountMvRmBreak = true;
                if (!isPageTurn) {
                    clear_history();
                    ctx.filterHistory = false;
                    loadHistory(ctx.filterHistory);
                }
            };

            const CpMvRmColors colors = getCpMvRmColors();

            std::string green = "\001" + std::string(colors.prompt_green) + "\002";
            std::string blue  = "\001" + std::string(colors.prompt_blue)  + "\002";
            std::string reset = "\001" + std::string(UI::Palette::BoldReset) + "\002";

            std::string promptPrefix = "\n";
            std::string promptSuffix = std::format(
                "{0}FolderPaths{1} ↵ for selected {0}ISO{1} to be {2}{3}{1} into, ? for help:\n{4}",
                green,                      // {0}
                blue,                       // {1}
                ctx.operationColor,         // {2}
                ctx.operationDescription,   // {3}
                reset                       // {4}
            );

            {
                RlKeyBindGuard     keyGuard;
                RlStartupHookGuard hookGuard;

                userInput = handlePaginatedDisplay(
                    entries, promptPrefix, promptSuffix, setupEnv, isPageTurn, currentPage, false
                );
            }

            if (userInput == "EOF_SIGNAL") { shouldContinue = false; break; }
            if (userInput == "?") {
                bool import2ISO = false, isCpMv = true;
                helpSearches(isCpMv, import2ISO, ctx.isCopy);
                ctx.userDestDir = "";
                continue;
            }
            if (userInput == "\x1b") {
                ctx.umountMvRmBreak = false;
                ctx.userDestDir = "";
                clear_history();
                shouldContinue = false;
                continue;
            }
            if (userInput.empty()) continue;

            bool hasOverwriteFlag = (userInput.size() >= 3 && userInput.substr(userInput.size() - 3) == " -o");
            std::string pathsInput = hasOverwriteFlag ? userInput.substr(0, userInput.size() - 3) : userInput;

            bool pathsValid = true;
            std::string invalidPathError;

            std::istringstream iss(pathsInput);
            std::string token;
            while (std::getline(iss, token, ';')) {
                token.erase(0, token.find_first_not_of(" \t"));
                token.erase(token.find_last_not_of(" \t") + 1);
                std::string err = validateLinuxPath(token);
                if (!err.empty()) {
                    pathsValid = false;
                    invalidPathError = err;
                    break;
                }
            }

            if (!pathsValid) {
                verboseSets.uniqueErrorTokenMessages.insert(invalidPathError);
                ctx.userDestDir = "";
                resetReadlinePagination();
                continue;
            }

            ctx.overwriteExisting = hasOverwriteFlag;
            ctx.userDestDir = pathsInput;
            add_history(pathsInput.c_str());
            saveHistory(ctx.filterHistory);
            shouldContinue = false;
        } else {
            bool proceedWithDelete = handleDeleteOperation(
                ctx.isoFiles, ctx.indexChunks, ctx.umountMvRmBreak, ctx.abortDel
            );
            if (!proceedWithDelete) {
                ctx.userDestDir = "";
                shouldContinue = false;
                continue;
            }
            shouldContinue = false;
        }
    }
    resetReadlinePagination();
    return ctx.userDestDir;
}

namespace fs = std::filesystem;

/**
 * @brief Resolves the real (invoking) user and group IDs and names.
 * @details Forward declaration; defined elsewhere. Used to restore ownership
 * of files created while running under sudo/setuid.
 *
 * @param real_uid        [out] Real user ID.
 * @param real_gid        [out] Real group ID.
 * @param real_username   [out] Real username.
 * @param real_groupname  [out] Real group name.
 */
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

/**
 * @brief Builds a color‑coded error message, adds it to the reporter,
 *        increments failedTasks and sets operationSuccessful = false.
 *
 * Constructs a human-readable error string for the given @p errorType, using
 * the configured verbose theme for colour codes. The message is appended to
 * the batch reporter, the failed-task counter is incremented with acquire-release
 * semantics, and the operation-successful flag is stored as false with release
 * semantics.
 *
 * @details **Error types and formatting:**
 * - @c "same_file"        — red; always uses full @c srcDir/srcFile path regardless
 *                           of @c displayConfig::toggleNamesOnly.
 * - @c "source_missing"   — red; uses @c displaySrc.
 * - @c "overwrite_failed" — red; uses @c destDir + @c srcFile (no separator) + @p errorDetail.
 * - @c "file_exists"      — red; uses @c displaySrc and @c destDir; hints at @c -o flag.
 * - @c "remove_after_move"— red; uses @c displaySrc + @p errorDetail.
 * - @c "missing_file"     — purple (distinct from all other branches); uses @c displaySrc.
 * - default               — red; emits @p errorDetail directly.
 *
 * **Display path:** @c displaySrc is @c srcDir/srcFile when
 * @c displayConfig::toggleNamesOnly is false, or @c srcFile alone when true.
 * This toggle is applied in all branches except @c "same_file".
 *
 * All original colour codes and message templates are preserved exactly.
 *
 * @param ctx         Operation context providing reporter, counters, and success flag.
 * @param errorType   Category of failure (see detailed list above).
 * @param srcDir      Path view of the source directory.
 * @param srcFile     Filename view of the source file.
 * @param destDir     Path view of the destination directory.
 * @param errorDetail System error description; used by several branches.
 * @param operation   Operation label ("move", "copy", "moving", "copying").
 */
static void reportErrorCpMvRm(OperationContext& ctx,
                       std::string_view errorType,
                       std::string_view srcDir,
                       std::string_view srcFile,
                       std::string_view destDir,
                       std::string_view errorDetail,
                       std::string_view operation)
{
    const auto vt = getVerboseTheme();
    std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

    std::string errorMsg;
    errorMsg.reserve(256 + displaySrc.size() + errorDetail.size());

    if (errorType == "same_file") {
        // always use full srcDir/srcFile regardless of toggleNamesOnly
        errorMsg.append(vt.red).append("Cannot ").append(operation)
                .append(" file to itself: ")
                .append(vt.red).append("'").append(vt.yellow)
                .append(srcDir).append("/").append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "source_missing") {
        errorMsg.append(vt.red).append("Source file no longer exists: ")
                .append(vt.red).append("'").append(vt.yellow)
                .append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(".")
                .append(vt.reset);
    }
    else if (errorType == "overwrite_failed") {
        errorMsg.append(vt.red).append("Failed to overwrite: ")
                .append(vt.red).append("'").append(vt.yellow)
                .append(destDir).append(srcFile).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ")
                .append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "file_exists") {
        errorMsg.append(vt.red).append("Error ").append(operation).append(": '")
                .append(vt.red).append(vt.yellow)
                .append(displaySrc).append(vt.red)
                .append(vt.reset).append(vt.red).append(" → ")
                .append(vt.yellow).append(destDir).append(srcFile).append(vt.red).append("': File exists ")
                .append("(overwrite with -o")
                .append(vt.reset).append(vt.red).append(").")
                .append(vt.reset);
    }
    else if (errorType == "remove_after_move") {
        errorMsg.append(vt.red).append("Move completed but failed to remove source file: ")
                .append(vt.red).append("'").append(vt.yellow)
                .append(displaySrc).append(vt.red).append("'")
                .append(vt.reset).append(vt.red).append(" - ")
                .append(errorDetail).append(".")
                .append(vt.reset);
    }
    else if (errorType == "missing_file") {
        errorMsg.append(vt.purple).append("Missing: ")
                .append(vt.purple).append("'")
                .append(vt.yellow).append(displaySrc)
                .append(vt.purple).append("'.");
    }
    else {
        errorMsg.append(vt.red).append("Error: ").append(errorDetail)
                .append(vt.reset);
    }

    ctx.reporter.addError(std::move(errorMsg));
    if (ctx.failedTasks)
        ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
    ctx.operationSuccessful.store(false, std::memory_order_release);
}

/**
 * @brief Performs a binary file copy with real-time progress tracking and
 *        cancellation support.
 *
 * Uses an 8 MiB buffered stream approach to transfer data between two paths.
 * Data is written to a temporary file (@p dst plus a ".part" suffix) and
 * renamed over @p dst only after every byte has been written and the stream
 * has been flushed and closed successfully. The destination is therefore
 * never touched until the copy is complete: if @p dst already exists it is
 * replaced atomically by the rename, and it is left intact on failure or
 * cancellation.
 *
 * An atomic counter is updated incrementally so the UI can display progress.
 * The operation can be aborted at any time via GlobalState::g_operationCancelled.
 * On cancellation or any failure after the temporary file was created, the
 * temporary file is removed. Its cleanup uses a separate error code, so the
 * failure reason stored in @p ec is preserved.
 *
 * @param src            Source file path.
 * @param dst            Destination file path.
 * @param completedBytes Optional (may be null) atomic counter for tracking bytes
 *                       written (updated with
 *                       memory_order_relaxed).
 * @param ec             Error code object to capture system failures.
 *                       Set to operation_canceled on user abort,
 *                       no_such_file_or_directory if the source is missing,
 *                       permission_denied if the temporary file cannot be
 *                       opened, io_error on write or flush failures, or the
 *                       error from the final rename.
 * @return True if the copy completed successfully, false if cancelled or failed.
 */
static bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst,
                              std::atomic<size_t>* completedBytes,
                              std::error_code& ec) {
    const size_t bufferSize = 8 * 1024 * 1024;
    std::vector<char> buffer(bufferSize);

    if (GlobalState::g_operationCancelled.load()) return false;

    std::ifstream input(src, std::ios::binary);
    if (!input) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    // Write to a temp file so a failed or cancelled copy never leaves a
    // partial file at dst, and never touches an existing dst until success.
    fs::path tmp = dst;
    tmp += ".part";

    std::ofstream output(tmp, std::ios::binary);
    if (!output) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    // Closes and removes the temp file; uses its own error_code so the
    // caller's ec (operation_canceled / io_error) is never overwritten.
    auto discardTemp = [&]() {
        output.close();
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
    };

    while (!GlobalState::g_operationCancelled.load()) {
        input.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = input.gcount();
        if (bytesRead == 0) break;

        output.write(buffer.data(), bytesRead);
        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            discardTemp();
            return false;
        }

        if (completedBytes)
            completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
    }

    if (GlobalState::g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        discardTemp();
        return false;
    }

    output.close();                       // flush; catches deferred write errors
    if (!output) {
        ec = std::make_error_code(std::errc::io_error);
        discardTemp();
        return false;
    }

    std::error_code renameEc;
    fs::rename(tmp, dst, renameEc);       // atomically replaces dst if it exists
    if (renameEc) {
        ec = renameEc;
        discardTemp();
        return false;
    }

    return true;
}

/**
 * @brief Logs the final result of a Move or Copy operation to the batch reporter.
 *
 * Formats and dispatches either a success message (to the "isos" set) or an
 * error message (to the "errors" set) based on the operation outcome. Updates
 * the operation‑context counters accordingly:
 * - On failure or error: increments @c failedTasks, sets @c operationSuccessful
 *   to false.
 * - On success: increments @c completedTasks.
 *
 * Batching is handled automatically by the reporter; no explicit flush call
 * is needed here.
 *
 * @param ctx              Operation context providing reporter and counters.
 * @param success          Whether the operation succeeded at the filesystem level.
 * @param cancelled        Whether the operation was aborted by the user.
 * @param ec               Error code (empty on success).
 * @param verb             Present‑tense verb for error messages (e.g. "moving", "copying").
 * @param srcDir           Source directory for display formatting.
 * @param srcFile          Source filename for display formatting.
 * @param destDirProcessed Destination directory for display.
 * @param destFile         Destination filename for display.
 */
static void logOperationResult(OperationContext& ctx,
                               bool success, bool cancelled,
                               const std::error_code& ec,
                               std::string_view verb,
                               std::string_view srcDir, std::string_view srcFile,
                               std::string_view destDirProcessed, std::string_view destFile)
{
    const CpMvRmColors colors = getCpMvRmColors();
    std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

    if (!success || ec) {
        std::string errorDetail = cancelled ? "Cancelled" : ec.message();
        std::string msg;
        msg.reserve(128 + displaySrc.size() + destDirProcessed.size() + errorDetail.size());

        msg.append(colors.error_label).append("Error ").append(verb).append(": '")
           .append(colors.error_path).append(displaySrc)
           .append(UI::Palette::BoldReset).append(colors.error_label)
           .append(" → ").append(colors.error_path)
           .append(destDirProcessed).append(destFile)
           .append(colors.error_label).append("': ")
           .append(errorDetail).append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addError(std::move(msg));
        if (ctx.failedTasks)
            ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
        ctx.operationSuccessful.store(false, std::memory_order_release);
    } else {
        std::string_view pastVerb = (verb == "moving") ? "Moved" : "Copied";
        std::string msg;
        // Exact size (plus a little slack) instead of a generous guess: these
        // strings are kept until the verbose report is printed.
        msg.reserve(3 * colors.success_label.size() + pastVerb.size() + colors.success_path.size()
                    + displaySrc.size() + UI::Palette::BoldReset.size() + colors.dest_path.size()
                    + destDirProcessed.size() + destFile.size() + 16);

        msg.append(colors.success_label).append(pastVerb).append(": '")
           .append(colors.success_path).append(displaySrc)
           .append(UI::Palette::BoldReset).append(colors.success_label)
           .append(" → ")
           .append(colors.dest_path).append(destDirProcessed)
           .append(destFile)
           .append(colors.success_label).append("'.");

        ctx.reporter.addSuccess(std::move(msg));
        if (ctx.completedTasks)
            ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    // Batching happens automatically inside reporter.addSuccess/addError
}

/**
 * @brief Removes a single file from disk and logs the outcome.
 *
 * Attempts to delete the file at @p srcPath. Respects the global cancellation
 * flag; if set, the operation is aborted before touching the filesystem.
 * On success, the file's byte count is added to @c ctx.completedBytes and a
 * success message is emitted. On failure, the error is captured and reported.
 * All counters are taken from @p ctx; any that are null are skipped.
 *
 * @param ctx      Operation context for reporting and counters.
 * @param srcPath  Full path to the file to delete.
 * @param srcDir   Source directory (for display).
 * @param srcFile  Source filename (for display).
 * @param fileSize Size of the file in bytes (added to completedBytes on success).
 */
static void performDeleteOperation(OperationContext& ctx,
                            const fs::path& srcPath,
                            std::string_view srcDir, std::string_view srcFile,
                            size_t fileSize)
{
    const CpMvRmColors colors = getCpMvRmColors();
    std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        std::string msg;
        msg.reserve(128 + displaySrc.size());
        msg.append(colors.error_label).append("Error deleting: ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label)
           .append(": Cancelled.")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addError(std::move(msg));
        if (ctx.failedTasks)
            ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
        ctx.operationSuccessful.store(false, std::memory_order_release);
        return;
    }

    std::error_code ec;
    if (fs::remove(srcPath, ec)) {
        if (ctx.completedBytes)
            ctx.completedBytes->fetch_add(fileSize, std::memory_order_relaxed);

        std::string msg;
        msg.reserve(2 * colors.success_label.size() + colors.success_path.size() + displaySrc.size()
                    + 2 * UI::Palette::BoldReset.size() + 16);
        msg.append(colors.success_label).append("Deleted: ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addSuccess(std::move(msg));
        if (ctx.completedTasks)
            ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
    } else {
        std::string msg;
        msg.reserve(128 + displaySrc.size() + (ec ? ec.message().size() : 20));
        msg.append(colors.error_label).append("Error deleting: ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(": ")
           .append(ec ? ec.message() : "File not found")
           .append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addError(std::move(msg));
        if (ctx.failedTasks)
            ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
        ctx.operationSuccessful.store(false, std::memory_order_release);
    }
}

/**
 * @brief Executes a file Move operation (single destination).
 *
 * Tries a fast filesystem rename first (same‑device move). If that fails
 * (typically because the destination is on a different device), falls back to
 * a manual copy‑then‑delete procedure. The source is only removed after a
 * successful copy.
 *
 * Ownership of the destination file is changed to the real user (via the
 * changeOwnership callback in @p ctx) after a successful operation. Byte and
 * task counters are taken from @p ctx; any that are null are skipped.
 *
 * @param ctx      Operation context for reporting, counters, and ownership.
 * @param p        Source/destination paths and display strings.
 * @param fileSize Source file size in bytes, added to @c ctx.completedBytes
 *                 after a successful rename.
 * @return True if the move succeeded, false otherwise.
 */
static bool performMoveOperation(OperationContext& ctx, const TransferPaths& p, size_t fileSize) {
    const bool cancelled = GlobalState::g_operationCancelled.load(std::memory_order_acquire);
    if (cancelled) {
        logOperationResult(ctx, false, true, {}, "moving",
                           p.srcDir, p.srcFile, p.destDirProcessed, p.destFile);
        return false;
    }

    std::error_code ec;
    fs::rename(p.srcPath, p.destPath, ec);

    if (!ec) {
        if (ctx.completedBytes) {
            ctx.completedBytes->fetch_add(fileSize, std::memory_order_relaxed);
        }
        if (ctx.changeOwnership) {
            ctx.changeOwnership(p.destPath);
        }
        logOperationResult(ctx, true, false, {}, "moving",
                           p.srcDir, p.srcFile, p.destDirProcessed, p.destFile);
        return true;
    }

    // Cross-device fallback
    ec.clear();
    const bool success = bufferedCopyWithProgress(
        p.srcPath, p.destPath, ctx.completedBytes, ec);

    if (success) {
        std::error_code deleteEc;
        if (!fs::remove(p.srcPath, deleteEc)) {
            const MainTheme* theme = getActiveTheme();
            const bool isOriginal = (globalTheme == "original");
            const std::string_view errLabel = isOriginal ? UI::Palette::Red    : theme->secondary;
            const std::string_view errPath  = isOriginal ? UI::Palette::Yellow : theme->warning;

            const std::string displaySrc = buildDisplaySrc(p.srcDir, p.srcFile);
            std::string msg;
            msg.reserve(160 + displaySrc.size() + deleteEc.message().size());
            msg.append(errLabel).append("Move completed but failed to remove source file: ")
               .append(errPath).append("'").append(displaySrc).append("'")
               .append(UI::Palette::BoldReset).append(errLabel).append(" - ")
               .append(deleteEc.message()).append(UI::Palette::BoldReset);

            ctx.reporter.addError(std::move(msg));
            if (ctx.completedTasks) {
                ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
            }
            return true;  // Move itself succeeded
        }

        if (ctx.changeOwnership) {
            ctx.changeOwnership(p.destPath);
        }
        logOperationResult(ctx, true, false, {}, "moving",
                           p.srcDir, p.srcFile, p.destDirProcessed, p.destFile);
        return true;
    }

    logOperationResult(ctx, false,
                       GlobalState::g_operationCancelled.load(std::memory_order_acquire),
                       ec, "moving",
                       p.srcDir, p.srcFile, p.destDirProcessed, p.destFile);
    return false;
}

/**
 * @brief Buffered copy to a single destination, with reporting and ownership.
 *
 * Performs a buffered copy from @p p.srcPath to @p p.destPath. Used for plain
 * copies ("copying") and for each destination of a multi-destination move
 * ("moving"). It does **not** attempt a fast rename and does **not** delete
 * the source — source cleanup for multi‑dest moves is handled by the caller
 * once at least one copy succeeds. Ownership of the destination file is
 * changed to the real user on success.
 *
 * @param ctx  Operation context for reporting, counters, and ownership.
 * @param p    Source/destination paths and display strings.
 * @param verb Present‑tense verb used in log messages ("copying" or "moving").
 * @return True if the copy succeeded, false otherwise.
 */
static bool performBufferedCopyOperation(OperationContext& ctx,
                                         const TransferPaths& p,
                                         std::string_view verb)
{
    std::error_code ec;
    const bool success = bufferedCopyWithProgress(p.srcPath, p.destPath, ctx.completedBytes, ec);
    if (success && ctx.changeOwnership)
        ctx.changeOwnership(p.destPath);

    logOperationResult(ctx, success,
                       GlobalState::g_operationCancelled.load(), ec, verb,
                       p.srcDir, p.srcFile, p.destDirProcessed, p.destFile);
    return success;
}

namespace {

    /**
     * @brief Parse a semicolon-separated list of destination directories.
     *
     * Splits @p userDestDir on ';' and normalizes each entry through
     * `fs::path(...).string()`. Empty entries produced by a trailing ';' are
     * preserved as-is (matching the original inline parsing behaviour).
     *
     * @param userDestDir Semicolon-separated destination directory string.
     *
     * @return Vector of normalized destination directory strings, in input order.
     *
     * @note An empty @p userDestDir yields an empty vector (no iterations).
     */
    std::vector<std::string> parseDestinationDirs(const std::string& userDestDir)
    {
        std::vector<std::string> destDirs;
        std::istringstream iss(userDestDir);
        std::string destDir;
        while (std::getline(iss, destDir, ';'))
            destDirs.push_back(fs::path(destDir).string());
        return destDirs;
    }

    /**
     * @brief Validate that a single (source, destination) pair is safe to copy/move.
     *
     * Applies the pre-dispatch checks previously inlined in
     * handleIsoFileOperation(), with the same reporting, except that an existing
     * destination is no longer deleted up front. Checks run in this order:
     *  1. Same-file check (source and destination resolve to the same path).
     *  2. Source-missing check (source disappeared mid-run).
     *  3. Destination-exists check: if the destination does not exist, the pair
     *     is ready. If it exists and @c params.overwriteExisting is false, a
     *     "file_exists" error is reported. If it exists and
     *     @c params.overwriteExisting is true, the pair is ready; the existing
     *     file is not touched here.
     *
     * @param ctx    Operation context used for error reporting.
     * @param params Destination-check parameters (see DestinationCheckParams).
     *               @c params.state.validDestinations is incremented here.
     *
     * @return True if the operation may proceed to dispatch; false otherwise
     *         (a corresponding error message has already been reported).
     *
     * @note @c params.state.validDestinations is incremented for every
     *       non-same-file destination, even if a later check rejects it. This
     *       preserves the original multi-destination cleanup semantics.
     */
    bool isDestinationReadyForCopyMove(OperationContext& ctx, DestinationCheckParams& params) {
        const TransferPaths& p = params.paths;

        if (fs::absolute(p.srcPath) == fs::absolute(p.destPath)) {
            const std::string_view opName = (params.op == TransferOp::Move) ? "move" : "copy";
            reportErrorCpMvRm(ctx, "same_file", p.srcDir, p.srcFile, "", "", opName);
            return false;
        }

        ++params.state.validDestinations;

        if (!fs::exists(p.srcPath)) {
            reportErrorCpMvRm(ctx, "source_missing", p.srcDir, p.srcFile, "", "", "");
            return false;
        }

        if (!fs::exists(p.destPath)) {
            return true;
        }

        if (!params.overwriteExisting) {
            const std::string_view opName = (params.op == TransferOp::Copy) ? "copying" : "moving";
            reportErrorCpMvRm(ctx, "file_exists", p.srcDir, p.srcFile,
                              p.destDirProcessed, "", opName);
            return false;
        }

        // Overwrite: the existing file is replaced atomically by the rename at
        // the end of the copy/move, so it survives if the copy fails.
        return true;
    }

    /**
     * @brief Validate and dispatch one copy/move operation for a single destination.
     *
     * Performs destination validation via isDestinationReadyForCopyMove(), then
     * dispatches to the appropriate operation function:
     *  - performBufferedCopyOperation ("moving") — move with more than one
     *                                    destination (source removed by the caller).
     *  - performMoveOperation          — single-destination move.
     *  - performBufferedCopyOperation ("copying") — copy (any destination count).
     *
     * @param ctx    Operation context for reporting, counters, and ownership.
     * @param params Destination-check/dispatch parameters (see
     *               DestinationCheckParams). Updated fields:
     *               - @c state.validDestinations       (incremented during validation)
     *               - @c state.atLeastOneCopySucceeded (set true on successful copy
     *                 in multi-destination move mode)
     *
     * @note For multi-destination moves, @c state.atLeastOneCopySucceeded is
     *       what drives the caller's source-removal logic.
     */
    void processCopyMoveDestination(OperationContext& ctx,
                                    DestinationCheckParams& params)
    {
        if (!isDestinationReadyForCopyMove(ctx, params)) {
            return;
        }

        const TransferPaths& p = params.paths;

        if (params.op == TransferOp::Move && params.destDirCount > 1) {
            if (performBufferedCopyOperation(ctx, p, "moving")) {
                params.state.atLeastOneCopySucceeded = true;
            }
        } else if (params.op == TransferOp::Move) {
            performMoveOperation(ctx, p, params.fileSize);
        } else {
            performBufferedCopyOperation(ctx, p, "copying");
        }
    }

    /**
     * @brief Remove the source file after a multi-destination move.
     *
     * Removes @p srcPath only when all of the following hold:
     *  - the operation is a move (@p op == TransferOp::Move),
     *  - there is more than one destination (@p destDirCount > 1),
     *  - at least one destination was valid (@c state.validDestinations > 0),
     *  - at least one copy succeeded (@c state.atLeastOneCopySucceeded).
     *
     * If the removal fails, a "remove_after_move" error is reported.
     *
     * @param ctx           Operation context used for error reporting.
     * @param srcPath       Source path to remove.
     * @param srcDir        Source directory (for reporting).
     * @param srcFile       Source filename (for reporting).
     * @param op            Copy or move.
     * @param destDirCount  Number of destination directories.
     * @param state         Per-file multi-destination tracking state.
     *
     * @note This intentionally does not remove the source if no copy succeeded,
     *       matching the documented "remove once at least one copy succeeds"
     *       contract.
     */
    void removeSourceAfterMultiDestMove(OperationContext& ctx,
                                        const fs::path& srcPath,
                                        std::string_view srcDir,
                                        std::string_view srcFile,
                                        TransferOp op,
                                        size_t destDirCount,
                                        const MultiDestState& state)
    {
        if (!(op == TransferOp::Move && destDirCount > 1 &&
              state.validDestinations > 0 && state.atLeastOneCopySucceeded)) {
            return;
        }

        std::error_code deleteEc;
        if (!fs::remove(srcPath, deleteEc)) {
            reportErrorCpMvRm(ctx, "remove_after_move", srcDir, srcFile, "",
                              deleteEc.message(), "");
        }
    }

} // namespace

/**
 * @brief High-level handler that iterates through ISO files to perform CP, MV, or RM.
 *
 * Manages thread‑safe updates to reporting sets and handles ownership changes
 * for newly created files.
 *
 * Processing flow:
 * - Parses semicolon‑separated destination directories from
 *   @c options.userDestDir.
 * - Creates a thread‑local BatchReporter and OperationContext.
 * - For each 1-based index in @p chunk (resolved against @c options.isoFiles):
 *   - Validates the file still exists on disk.
 *   - For delete: calls performDeleteOperation.
 *   - For move/copy: iterates over all destinations, checking for same‑file,
 *     source‑missing, overwrite, and file‑exists conditions before dispatching
 *     to the appropriate operation function.
 *   - For multi‑destination moves: the source file is removed once at least
 *     one copy succeeds.
 * - Ownership of newly created files is restored to the real user (via chown).
 * - Batched messages are flushed on exit.
 *
 * @param options   File-operation options. Relevant fields:
 *                  - `isoFiles`          master file list (indexed by @p chunk)
 *                  - `userDestDir`       semicolon-separated destination directories
 *                  - `isMove`            true for move operation
 *                  - `isCopy`            true for copy operation
 *                  - `isDelete`          true for delete operation
 *                  - `overwriteExisting` if true, replace existing files at destination
 * @param chunk     1-based indices into @c options.isoFiles to operate on in
 *                  this chunk.
 * @param progress  Progress/accounting object. Relevant fields:
 *                  - `completedBytes`  atomic counter for bytes processed
 *                  - `completedTasks`  atomic counter for successful operations
 *                  - `failedTasks`     atomic counter for failed operations
 *
 * @note For multi-destination moves, the source file is removed once at least
 *       one copy succeeds — not necessarily all copies.
 * @note Ownership is restored using the real user ID from sudo/setuid context.
 */
void handleIsoFileOperation(const FileOperationOptions& options,
                            const std::vector<int>& chunk,
                            FileOperationProgress& progress)
{
    // ----- Thread-local batched reporter -----
    BatchReporter reporter(verboseSets.operationCompleted,
                           verboseSets.operationFailed,
                           GlobalMutexes::globalSetsMutex,
                           50);  // batch size

    std::atomic<bool> operationSuccessful(true);

    // ----- Resolve real user for ownership changes -----
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    OperationContext ctx{
        .reporter            = reporter,
        .completedTasks      = &progress.completedTasks,
        .failedTasks         = &progress.failedTasks,
        .operationSuccessful = operationSuccessful,
        .changeOwnership     = [&](const fs::path& path) {
                                   chown(path.c_str(), real_uid, real_gid);
                               },
        .completedBytes      = &progress.completedBytes,
    };

    // ----- Parse destinations -----
    const std::vector<std::string> destDirs = parseDestinationDirs(options.userDestDir);
    const TransferOp op = options.isMove ? TransferOp::Move : TransferOp::Copy;

    // ----- Process each file -----
    for (const int chunkIdx : chunk) {
        const std::string& iso = options.isoFiles[chunkIdx - 1];
        fs::path srcPath(iso);
        if (!fs::exists(srcPath)) {
            auto [isoDir, isoFile] =
                extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");
            reportErrorCpMvRm(ctx, "missing_file", isoDir, isoFile, "", "", "");
            continue;
        }

        auto [srcDir, srcFile] =
            extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");

        struct stat st;
        size_t fileSize = (stat(srcPath.c_str(), &st) == 0) ? st.st_size : 0;

        if (options.isDelete) {
            performDeleteOperation(ctx, srcPath, srcDir, srcFile, fileSize);
            continue;
        }

        MultiDestState state;

        for (const auto& dst : destDirs) {
            fs::path destPath = fs::path(dst) / srcPath.filename();

            auto [_, destFile] =
                extractDirectoryAndFilename(destPath.native(), "cp_mv_rm");

            DestinationCheckParams destParams{
                .paths = {
                    .srcPath          = srcPath,
                    .destPath         = destPath,
                    .srcDir           = srcDir,
                    .srcFile          = srcFile,
                    .destDirProcessed = dst,
                    .destFile         = destFile,
                },
                .op                = op,
                .overwriteExisting = options.overwriteExisting,
                .fileSize          = fileSize,
                .destDirCount      = destDirs.size(),
                .state             = state,
            };
            processCopyMoveDestination(ctx, destParams);
        }

        // Multi-dest move cleanup
        removeSourceAfterMultiDestMove(ctx, srcPath, srcDir, srcFile,
                                       op, destDirs.size(), state);
    }

    // ----- Flush any remaining batched messages -----
    reporter.flush();
}
