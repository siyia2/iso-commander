// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <filesystem>
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
#include "../cpMvRm.h"
#include "../display.h"
#include "../globalMutexes.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../sort.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Constructs a list of formatted strings representing ISO files for display.
 * @details Extracts filenames and directories, applying theme-specific colors and
 * formatting based on global display configurations.
 * * @param indexChunks Chunks of indices pointing to selected files.
 * @param isoFiles The full list of available ISO file paths.
 * @return A vector of formatted strings ready for console output.
 */
std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
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
 * @brief Validates a Linux filesystem path for absolute format, control characters, and existence.
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

std::string handlePaginatedDisplay(const std::vector<std::string>& entries, const std::string& promptPrefix,
const std::string& promptSuffix, const std::function<void()>& setupEnvironmentFn, bool& isPageTurnm, size_t& currentPage);

/**
 * @brief Orchestrates user confirmation for permanent file deletion.
 *
 * Renders the paginated ISO list via handlePaginatedDisplay and captures a Y/N
 * confirmation. Configures the Readline environment for the delete prompt
 * (Ctrl+L redraw, ESC exit handler). The '\f' key binding is restored
 * automatically via RlFormFeedGuard on every exit path, including early returns
 * and EOF. Pagination turns loop silently without treating the input as a
 * confirmation.
 *
 * @param isoFiles        The full list of ISO file paths to be shown and deleted.
 * @param indexChunks     Chunks used for paginated list display formatting.
 * @param umountMvRmBreak [out] Set to true when the user confirms ('Y'/'y');
 *                        set to false on any cancel or EOF path.
 * @param abortDel        [out] Set to true when the user explicitly cancels
 *                        (non-Y input) or sends EOF; left unchanged on 'Y'.
 * @return True if the user confirmed with 'Y' or 'y', false on any other input,
 *         cancellation, or EOF.
 */
bool handleDeleteOperation(const std::vector<std::string>& isoFiles,
                           std::vector<std::vector<int>>& indexChunks,
                           bool& umountMvRmBreak,
                           bool& abortDel) {
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
    std::string promptSuffix =
        blue + "The selected " +
        green + "ISO" +
        blue + " will be " +
        red + "*PERMANENTLY DELETED FROM DISK*" +
        blue + ". Proceed? (y/n): " +
        reset;
    bool isPageTurn = false;
    auto setupEnv = [&]() {
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
            RlFormFeedGuard ffGuard; // restores '\f' → prevent_readline_keybindings on scope exit
            userInput = handlePaginatedDisplay(
                entries, promptPrefix, promptSuffix, setupEnv, isPageTurn, currentPage
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

void helpSearches(bool isCpMv, bool import2ISO);

/**
 * @brief Prompts for a destination directory or routes to deletion confirmation.
 *
 * In Copy/Move mode: Validates Linux paths, handles pagination, processes
 * overwrite flags (-o), and manages Readline history. Readline state
 * (key bindings, completion mode, startup hook) is restored automatically
 * via RAII guards on every exit path.
 * In Delete mode: Proxies the request to handleDeleteOperation.
 *
 * @param isoFiles           List of source ISO files.
 * @param indexChunks        Chunks used for paginated list display formatting.
 * @param userDestDir        [out] The resulting validated destination path(s);
 *                           empty if the operation was canceled.
 * @param operationColor     ANSI color escape string applied to the operation
 *                           label in the prompt.
 * @param operationDescription  Human-readable verb shown in the prompt
 *                              ("copy", "move", or "delete").
 * @param umountMvRmBreak    [in/out] Controls whether the parent loop should
 *                           unmount/break after the operation; set to false on
 *                           ESC or Move cancel, true when entering Move mode.
 * @param filterHistory      [in/out] Readline history filter flag; passed to
 *                           loadHistory/saveHistory.
 * @param isDelete           If true, routes to handleDeleteOperation instead
 *                           of the path-input prompt.
 * @param isCopy             Distinguishes Copy (true) from Move (false);
 *                           affects umountMvRmBreak behaviour.
 * @param abortDel           [out] Set to true by handleDeleteOperation when
 *                           the deletion is canceled by the user.
 * @param overwriteExisting  [out] Set to true when the user appends the "-o"
 *                           flag to their input.
 * @return The validated destination path string; empty string if canceled;
 *         empty string on delete path (result conveyed via abortDel).
 */
std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks,
std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel,
bool& overwriteExisting) {
    rl_attempted_completion_function = nullptr;
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
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
        if (!isDelete) {
            bool isPageTurn = false;

            // Persist the current input line and cursor position across Ctrl+L screen clears.
            static std::string saved_line;

            auto setupEnv = [&]() {
                enable_ctrl_d();
                setupSignalHandlerCancellations();

                // Ctrl+L: save current input, clear the screen.
                // The saved input is restored on the next prompt.
                rl_bind_key('\f', [](int count, int key) -> int {
                    static std::string saved_line = rl_copy_text(0, rl_end);
                    clear_screen_and_buffer(count, key);
                    return 0;
                });

                rl_bind_key('\t', my_rl_complete);

                // RlStartupHookGuard is scoped to setupEnv's enclosing loop
                // iteration (see below), so the hook is cleared whether
                // handlePaginatedDisplay returns normally or early.

                // Restore the saved prompt
                if (!RetainAndRestoreReadlineBuffer::g_rl_pending_text.empty()) {
                    rl_startup_hook = []() -> int {
                        if (!RetainAndRestoreReadlineBuffer::g_rl_pending_text.empty()) {
                            rl_insert_text(RetainAndRestoreReadlineBuffer::g_rl_pending_text.c_str());
                            rl_point = rl_end;
                            RetainAndRestoreReadlineBuffer::g_rl_pending_text = "";
                        }
                        return 0;
                    };
                } else {
                    rl_startup_hook = nullptr;
                }

                if (!isCopy) umountMvRmBreak = true;
                if (!isPageTurn) {
                    clear_history();
                    filterHistory = false;
                    loadHistory(filterHistory);
                }
            };

            const CpMvRmColors colors = getCpMvRmColors();

            std::string green = "\001" + std::string(colors.prompt_green) + "\002";
            std::string blue  = "\001" + std::string(colors.prompt_blue)  + "\002";
            std::string reset = "\001" + std::string(UI::Palette::BoldReset) + "\002";

            std::string promptPrefix = "\n";
            std::string promptSuffix =
                green + "FolderPaths" +
                blue + " ↵ for selected " +
                green + "ISO" +
                blue + " to be " +
                operationColor + operationDescription +
                blue + " into, ? for help:\n" +
                reset;

            // Guard scoped around handlePaginatedDisplay: the two rl_bind_key
            // resets in its destructor always fire, even if the call exits via
            // EOF_SIGNAL or any other early path. Replaces the two manual
            // rl_bind_key calls that previously appeared only on the happy path.
            // RlStartupHookGuard is also placed here so the hook set inside
            // setupEnv is always cleared after each iteration.
            {
                RlKeyBindGuard     keyGuard;
                RlStartupHookGuard hookGuard;

                userInput = handlePaginatedDisplay(
                    entries, promptPrefix, promptSuffix, setupEnv, isPageTurn, currentPage
                );
            } // ← rl_bind_key resets and rl_startup_hook = nullptr fire here

            if (userInput == "EOF_SIGNAL") { shouldContinue = false; break; }
            if (userInput == "?") {
                bool import2ISO = false, isCpMv = true;
                helpSearches(isCpMv, import2ISO);
                userDestDir = "";
                continue;
            }
            if (userInput == "\x1b") {
                umountMvRmBreak = false;
                userDestDir = "";
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
                userDestDir = "";
                resetReadlinePagination();
                continue;
            }

            overwriteExisting = hasOverwriteFlag;
            userDestDir = pathsInput;
            add_history(pathsInput.c_str());
            saveHistory(filterHistory);
            shouldContinue = false;
        } else {
            bool proceedWithDelete = handleDeleteOperation(isoFiles, indexChunks, umountMvRmBreak, abortDel);
            if (!proceedWithDelete) {
                userDestDir = "";
                shouldContinue = false;
                continue;
            }
            shouldContinue = false;
        }
    }
    resetReadlinePagination();
    return userDestDir;
}

namespace fs = std::filesystem;

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
void reportErrorCpMvRm(OperationContext& ctx,
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
                .append(vt.yellow).append(destDir).append(vt.red).append("': File exists ")
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
 * An atomic counter is updated incrementally so the UI can display progress.
 * The operation can be aborted at any time via GlobalState::g_operationCancelled;
 * if cancelled, the partially‑written destination file is removed.
 *
 * @param src            Source file path.
 * @param dst            Destination file path.
 * @param completedBytes Atomic counter for tracking bytes written (updated with
 *                       memory_order_relaxed).
 * @param ec             Error code object to capture system failures.
 *                       Set to operation_canceled on user abort,
 *                       no_such_file_or_directory if the source is missing,
 *                       permission_denied if the destination cannot be opened,
 *                       or io_error on write failures.
 * @return True if the copy completed successfully, false if cancelled or failed.
 */
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst,
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

    std::ofstream output(dst, std::ios::binary);
    if (!output) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    while (!GlobalState::g_operationCancelled.load()) {
        input.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = input.gcount();
        if (bytesRead == 0) break;

        output.write(buffer.data(), bytesRead);
        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }

        completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
    }

    if (GlobalState::g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        output.close();
        fs::remove(dst, ec);
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
           .append(" → ").append(destDirProcessed).append("': ")
           .append(errorDetail).append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addError(std::move(msg));
        if (ctx.failedTasks)
            ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
        ctx.operationSuccessful.store(false, std::memory_order_release);
    } else {
        std::string_view pastVerb = (verb == "moving") ? "Moved" : "Copied";
        std::string msg;
        msg.reserve(128 + displaySrc.size() + destDirProcessed.size() + destFile.size());

        msg.append(colors.success_label).append(pastVerb).append(": '")
           .append(colors.success_path).append(displaySrc)
           .append(UI::Palette::BoldReset).append(colors.success_label)
           .append(" → ")
           .append(colors.dest_path).append(destDirProcessed)
           .append(destFile)
           .append(UI::Palette::BoldReset).append("'.");

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
 * On success, the file's byte count is added to @p completedBytes and a
 * success message is emitted. On failure, the error is captured and reported.
 *
 * @param ctx            Operation context for reporting and counters.
 * @param srcPath        Full path to the file to delete.
 * @param srcDir         Source directory (for display).
 * @param srcFile        Source filename (for display).
 * @param fileSize       Size of the file in bytes (added to completedBytes on success).
 * @param completedBytes Atomic counter tracking total bytes deleted.
 * @param completedTasks Atomic counter incremented on successful deletion.
 * @param failedTasks    Atomic counter incremented on failure or cancellation.
 */
void performDeleteOperation(OperationContext& ctx,
                            const fs::path& srcPath,
                            std::string_view srcDir, std::string_view srcFile,
                            size_t fileSize,
                            std::atomic<size_t>* completedBytes,
                            std::atomic<size_t>* completedTasks,
                            std::atomic<size_t>* failedTasks)
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
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        ctx.operationSuccessful.store(false, std::memory_order_release);
        return;
    }

    std::error_code ec;
    if (fs::remove(srcPath, ec)) {
        completedBytes->fetch_add(fileSize, std::memory_order_relaxed);

        std::string msg;
        msg.reserve(128 + displaySrc.size());
        msg.append(colors.success_label).append("Deleted: ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(".")
           .append(UI::Palette::BoldReset);

        ctx.reporter.addSuccess(std::move(msg));
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
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
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
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
 * changeOwnership callback in @p ctx) after a successful operation.
 *
 * @param ctx              Operation context for reporting, counters, and ownership.
 * @param srcPath          Full source path.
 * @param destPath         Full destination path.
 * @param srcDir           Source directory (for display).
 * @param srcFile          Source filename (for display).
 * @param destDirProcessed Destination directory (for display).
 * @param destFile         Destination filename (for display).
 * @param fileSize         Size in bytes (used for progress tracking on fast rename).
 * @param completedBytes   Atomic counter tracking bytes moved.
 * @param completedTasks   Atomic counter incremented on successful move.
 * @return True if the move succeeded, false otherwise.
 */
bool performMoveOperation(OperationContext& ctx,
                          const fs::path& srcPath, const fs::path& destPath,
                          std::string_view srcDir, std::string_view srcFile,
                          std::string_view destDirProcessed, std::string_view destFile,
                          size_t fileSize,
                          std::atomic<size_t>* completedBytes,
                          std::atomic<size_t>* completedTasks)
{
    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        logOperationResult(ctx, false, true, {}, "moving",
                           srcDir, srcFile, destDirProcessed, destFile);
        return false;
    }

    std::error_code ec;
    fs::rename(srcPath, destPath, ec);

    if (!ec) {
        completedBytes->fetch_add(fileSize, std::memory_order_relaxed);
        if (ctx.changeOwnership)
            ctx.changeOwnership(destPath);
        logOperationResult(ctx, true, false, {}, "moving",
                           srcDir, srcFile, destDirProcessed, destFile);
        return true;
    }

    // Cross‑device fallback
    ec.clear();
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) {
        std::error_code deleteEc;
        if (!fs::remove(srcPath, deleteEc)) {
            const MainTheme* theme = getActiveTheme();
            const bool isOriginal  = (globalTheme == "original");
            const std::string_view errLabel = isOriginal ? UI::Palette::Red    : theme->secondary;
            const std::string_view errPath  = isOriginal ? UI::Palette::Yellow : theme->warning;

            std::string displaySrc = buildDisplaySrc(srcDir, srcFile);
            std::string msg;
            msg.reserve(160 + displaySrc.size() + deleteEc.message().size());
            msg.append(errLabel).append("Move completed but failed to remove source file: ")
               .append(errPath).append("'").append(displaySrc).append("'")
               .append(UI::Palette::BoldReset).append(errLabel).append(" - ")
               .append(deleteEc.message()).append(UI::Palette::BoldReset);

            ctx.reporter.addError(std::move(msg));
            if (completedTasks)
                completedTasks->fetch_add(1, std::memory_order_acq_rel);
            return true;   // move itself succeeded
        }
        if (ctx.changeOwnership)
            ctx.changeOwnership(destPath);
        logOperationResult(ctx, true, false, {}, "moving",
                           srcDir, srcFile, destDirProcessed, destFile);
    } else {
        logOperationResult(ctx, false,
                           GlobalState::g_operationCancelled.load(std::memory_order_acquire),
                           ec, "moving",
                           srcDir, srcFile, destDirProcessed, destFile);
    }
    return success;
}

/**
 * @brief Specialised move operation for multiple destinations (copy only).
 *
 * Performs a buffered copy from @p srcPath to @p destPath. Unlike
 * performMoveOperation, this does **not** attempt a fast rename and does
 * **not** delete the source — source cleanup for multi‑dest moves is handled
 * by the caller once at least one copy succeeds.
 *
 * @param ctx              Operation context for reporting, counters, and ownership.
 * @param srcPath          Full source path.
 * @param destPath         Full destination path.
 * @param srcDir           Source directory (for display).
 * @param srcFile          Source filename (for display).
 * @param destDirProcessed Destination directory (for display).
 * @param destFile         Destination filename (for display).
 * @param completedBytes   Atomic counter tracking bytes copied.
 * @return True if the copy succeeded, false otherwise.
 */
bool performMultiDestMoveOperation(OperationContext& ctx,
                                   const fs::path& srcPath, const fs::path& destPath,
                                   std::string_view srcDir, std::string_view srcFile,
                                   std::string_view destDirProcessed, std::string_view destFile,
                                   std::atomic<size_t>* completedBytes)
{
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success && ctx.changeOwnership)
        ctx.changeOwnership(destPath);

    logOperationResult(ctx, success,
                       GlobalState::g_operationCancelled.load(), ec, "moving",
                       srcDir, srcFile, destDirProcessed, destFile);
    return success;
}

/**
 * @brief Specialised copy operation.
 *
 * Performs a buffered copy from @p srcPath to @p destPath. Ownership of the
 * destination file is changed to the real user on success.
 *
 * @param ctx              Operation context for reporting, counters, and ownership.
 * @param srcPath          Full source path.
 * @param destPath         Full destination path.
 * @param srcDir           Source directory (for display).
 * @param srcFile          Source filename (for display).
 * @param destDirProcessed Destination directory (for display).
 * @param destFile         Destination filename (for display).
 * @param completedBytes   Atomic counter tracking bytes copied.
 * @return True if the copy succeeded, false otherwise.
 */
bool performCopyOperation(OperationContext& ctx,
                          const fs::path& srcPath, const fs::path& destPath,
                          std::string_view srcDir, std::string_view srcFile,
                          std::string_view destDirProcessed, std::string_view destFile,
                          std::atomic<size_t>* completedBytes)
{
    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success && ctx.changeOwnership)
        ctx.changeOwnership(destPath);

    logOperationResult(ctx, success,
                       GlobalState::g_operationCancelled.load(), ec, "copying",
                       srcDir, srcFile, destDirProcessed, destFile);
    return success;
}

/**
 * @brief High-level handler that iterates through ISO files to perform CP, MV, or RM.
 *
 * Manages thread‑safe updates to reporting sets and handles ownership changes
 * for newly created files.
 *
 * Processing flow:
 * - Parses semicolon‑separated destination directories from @p userDestDir.
 * - Creates a thread‑local BatchReporter and OperationContext.
 * - For each file in @p isoFiles (that also exists in @p isoFilesCopy):
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
 * @param isoFiles            Files to operate on in this chunk.
 * @param isoFilesCopy        Master file list (for existence validation).
 * @param userDestDir         Semicolon‑separated destination directories.
 * @param isMove              True for move operation.
 * @param isCopy              True for copy operation.
 * @param isDelete            True for delete operation.
 * @param completedBytes      Atomic counter for progress tracking (bytes processed).
 * @param completedTasks      Atomic counter for successful operations.
 * @param failedTasks         Atomic counter for failed operations.
 * @param overwriteExisting   If true, replace existing files at destination.
 * @param successfulDestPaths Vector to collect successful destination paths (optional).
 * @param destPathsMutex      Mutex protecting @p successfulDestPaths.
 *
 * @note For multi-destination moves, the source file is removed once at least
 *       one copy succeeds — not necessarily all copies.
 * @note Ownership is restored using the real user ID from sudo/setuid context.
 */
void handleIsoFileOperation(const std::vector<std::string>& isoFiles,
                            const std::vector<std::string>& isoFilesCopy,
                            const std::string& userDestDir,
                            bool isMove, bool isCopy, bool isDelete,
                            std::atomic<size_t>* completedBytes,
                            std::atomic<size_t>* completedTasks,
                            std::atomic<size_t>* failedTasks,
                            bool overwriteExisting,
                            std::vector<std::string>* successfulDestPaths,
                            std::mutex* destPathsMutex)
{
    // ----- Thread‑local batched reporter -----
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

    OperationContext ctx{ reporter, completedTasks, failedTasks,
                          operationSuccessful,
                          [&](const fs::path& path) {
                              chown(path.c_str(), real_uid, real_gid);
                          } };

    // ----- Parse destinations -----
    std::vector<std::string> destDirs;
    {
        std::istringstream iss(userDestDir);
        std::string destDir;
        while (std::getline(iss, destDir, ';'))
            destDirs.push_back(fs::path(destDir).string());
    }

    const std::unordered_set<std::string> isoFilesCopySet(
        isoFilesCopy.begin(), isoFilesCopy.end());

    // ----- Process each file -----
    for (const auto& iso : isoFiles) {
        if (!isoFilesCopySet.count(iso)) continue;

        fs::path srcPath(iso);
        if (!fs::exists(srcPath)) {
            auto [isoDir, isoFile] = extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");
            reportErrorCpMvRm(ctx, "missing_file", isoDir, isoFile, "", "", "");
            continue;
        }

        auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");
        struct stat st;
        size_t fileSize = (stat(srcPath.c_str(), &st) == 0) ? st.st_size : 0;

        if (isDelete) {
            performDeleteOperation(ctx, srcPath, srcDir, srcFile, fileSize,
                                   completedBytes, completedTasks, failedTasks);
            continue;
        }

        bool atLeastOneCopySucceeded = false;
        int validDestinations = 0;

        for (const auto& dst : destDirs) {
            fs::path destPath = fs::path(dst) / srcPath.filename();
            const std::string& destDirProcessed = dst;
            auto [_, destFile] = extractDirectoryAndFilename(destPath.native(), "cp_mv_rm");

            if (fs::absolute(srcPath) == fs::absolute(destPath)) {
                std::string op = isMove ? "move" : "copy";
                reportErrorCpMvRm(ctx, "same_file", srcDir, srcFile, "", "", op);
                continue;
            }
            ++validDestinations;

            if (!fs::exists(srcPath)) {
                reportErrorCpMvRm(ctx, "source_missing", srcDir, srcFile, "", "", "");
                continue;
            }

            if (fs::exists(destPath)) {
                if (overwriteExisting) {
                    std::error_code ec;
                    if (!fs::remove(destPath, ec)) {
                        reportErrorCpMvRm(ctx, "overwrite_failed", "", "",
                                          destDirProcessed, ec.message(), "");
                        continue;
                    }
                } else {
                    std::string op = isCopy ? "copying" : "moving";
                    reportErrorCpMvRm(ctx, "file_exists", srcDir, srcFile,
                                      destDirProcessed, "", op);
                    continue;
                }
            }

            bool success = false;
            if (isMove && destDirs.size() > 1) {
                success = performMultiDestMoveOperation(
                    ctx, srcPath, destPath, srcDir, srcFile,
                    destDirProcessed, destFile, completedBytes);
                if (success) {
                    atLeastOneCopySucceeded = true;
                    if (successfulDestPaths && destPathsMutex) {
                        std::lock_guard<std::mutex> lock(*destPathsMutex);
                        successfulDestPaths->push_back(destPath.string());
                    }
                }
            } else if (isMove) {
                success = performMoveOperation(
                    ctx, srcPath, destPath, srcDir, srcFile,
                    destDirProcessed, destFile, fileSize,
                    completedBytes, completedTasks);
                if (success && successfulDestPaths && destPathsMutex) {
                    std::lock_guard<std::mutex> lock(*destPathsMutex);
                    successfulDestPaths->push_back(destPath.string());
                }
            } else { // isCopy
                success = performCopyOperation(
                    ctx, srcPath, destPath, srcDir, srcFile,
                    destDirProcessed, destFile, completedBytes);
                if (success && successfulDestPaths && destPathsMutex) {
                    std::lock_guard<std::mutex> lock(*destPathsMutex);
                    successfulDestPaths->push_back(destPath.string());
                }
            }
        }

        // Multi‑dest move cleanup
        if (isMove && destDirs.size() > 1 && validDestinations > 0 && atLeastOneCopySucceeded) {
            std::error_code deleteEc;
            if (!fs::remove(srcPath, deleteEc)) {
                reportErrorCpMvRm(ctx, "remove_after_move", srcDir, srcFile, "",
                                  deleteEc.message(), "");
            }
        }
    }

    // ----- Flush any remaining batched messages -----
    reporter.flush();
}
