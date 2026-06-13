// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
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
#include "../concurrency.h"
#include "../cpMvRm.h"
#include "../display.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../pausePrompt.h"
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
        GlobalState::g_rl_complete_mode = 1;
        if (!isDelete) {
            bool isPageTurn = false;

            // Persist the current input line and cursor position across Ctrl+L screen clears.
            static std::string saved_line;
            static int saved_point = 0;

            auto setupEnv = [&]() {
                enable_ctrl_d();
                setupSignalHandlerCancellations();

                // Ctrl+L: save current input, clear the screen, then submit an empty line
                // so the main loop handles the redraw. The saved input is restored on the
                // next prompt via rl_pre_input_hook.
                rl_bind_key('\f', [](int count, int key) -> int {
                    saved_line = rl_copy_text(0, rl_end);
                    saved_point = rl_point;
                    clear_screen_and_buffer(count, key);
                    rl_on_new_line();
                    rl_replace_line("", 0);
                    rl_done = 1;
                    return 0;
                });

                // Before the next prompt accepts input, restore whatever the user had typed
                // before the screen clear, including the cursor position, then redisplay.
                rl_pre_input_hook = []() -> int {
                    if (!saved_line.empty()) {
                        rl_insert_text(saved_line.c_str());
                        rl_point = saved_point;
                        saved_line.clear();
                        rl_redisplay();
                    }
                    rl_pre_input_hook = nullptr;
                    return 0;
                };

                rl_bind_key('\t', my_rl_complete);

                // RlStartupHookGuard is scoped to setupEnv's enclosing loop
                // iteration (see below), so the hook is cleared whether
                // handlePaginatedDisplay returns normally or early.
                if (!GlobalState::g_rl_pending_text.empty()) {
                    rl_startup_hook = []() -> int {
                        if (!GlobalState::g_rl_pending_text.empty()) {
                            rl_insert_text(GlobalState::g_rl_pending_text.c_str());
                            rl_point = rl_end;
                            GlobalState::g_rl_pending_text = "";
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

/**
 * @brief Performs a binary file copy with real-time progress tracking and cancellation support.
 * @details Uses a buffered stream approach to transfer data and updates an atomic byte counter.
 * * @param src Source path.
 * @param dst Destination path.
 * @param completedBytes Atomic counter for tracking bytes written.
 * @param ec Error code object to capture system failures.
 * @return True if copy completed successfully, false if cancelled or failed.
 */
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
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
 * @brief Logs the final result of a Move or Copy operation into verbose reporting sets.
 */
static void logOperationResult(bool success, bool cancelled, const std::error_code& ec,
                               std::string_view verb, std::string_view srcDir, std::string_view srcFile,
                               std::string_view destDirProcessed, std::string_view destFile,
                               std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors,
                               std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
                               std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {

    const CpMvRmColors colors = getCpMvRmColors();

    std::string displaySrc;
    if (!displayConfig::toggleNamesOnly) {
        displaySrc.reserve(srcDir.size() + srcFile.size() + 1);
        displaySrc.append(srcDir).append("/").append(srcFile);
    } else {
        displaySrc = std::string(srcFile);
    }

    if (!success || ec) {
        // If this were string_view, it would point to a deleted temporary string.
        std::string errorDetail = cancelled ? "Cancelled" : ec.message();

        std::string msg;
        msg.reserve(128 + displaySrc.size() + destDirProcessed.size() + errorDetail.size());

        msg.append(colors.error_label).append("Error ").append(verb).append(": ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(" to '").append(destDirProcessed).append("': ")
           .append(errorDetail).append(".")
           .append(UI::Palette::BoldReset);

        verboseErrors.push_back(std::move(msg));

        if (failedTasks) {
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        operationSuccessful.store(false, std::memory_order_release);

    } else {
        // Success case: pastVerb can stay string_view because literals ("Moved") live forever
        std::string_view pastVerb = (verb == "moving") ? "Moved" : "Copied";

        std::string msg;
        msg.reserve(128 + displaySrc.size() + destDirProcessed.size() + destFile.size());

        msg.append(colors.success_label).append(pastVerb).append(": ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(" to ")
           .append(colors.dest_path).append("'").append(destDirProcessed).append(destFile).append("'")
           .append(UI::Palette::BoldReset).append(".");

        verboseIsos.push_back(std::move(msg));

        if (completedTasks) {
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (batchInsertMessages) {
        batchInsertMessages();
    }
}

/**
 * @brief Removes a single ISO file from the disk and logs the outcome.
 */
void performDeleteOperation(const fs::path& srcPath, std::string_view srcDir, std::string_view srcFile,
                            size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks,
                            std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos,
                            std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful,
                            const std::function<void()>& batchInsertMessages) {

    const CpMvRmColors colors = getCpMvRmColors();

    // Optimize display path construction
    std::string displaySrc;
    if (!displayConfig::toggleNamesOnly) {
        displaySrc.reserve(srcDir.size() + srcFile.size() + 1);
        displaySrc.append(srcDir).append("/").append(srcFile);
    } else {
        displaySrc = std::string(srcFile);
    }

    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        std::string msg;
        msg.reserve(128 + displaySrc.size());
        msg.append(colors.error_label).append("Error deleting: ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(": Cancelled.")
           .append(UI::Palette::BoldReset);

        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false, std::memory_order_release);
        batchInsertMessages();
        return;
    }

    std::error_code ec;
    // fs::remove returns true if the file was deleted, false if it didn't exist
    if (fs::remove(srcPath, ec)) {
        completedBytes->fetch_add(fileSize, std::memory_order_relaxed);

        std::string msg;
        msg.reserve(128 + displaySrc.size());
        msg.append(colors.success_label).append("Deleted: ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(".")
           .append(UI::Palette::BoldReset);

        verboseIsos.push_back(std::move(msg));
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

        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false, std::memory_order_release);
    }
    batchInsertMessages();
}

/**
 * @brief Executes a file Move operation.
 * @details Tries a quick filesystem rename first; if moving across different devices,
 * falls back to a manual copy-then-delete procedure.
 */
bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath,
                          std::string_view srcDir, std::string_view srcFile,
                          std::string_view destDirProcessed, std::string_view destFile,
                          size_t fileSize, std::atomic<size_t>* completedBytes,
                          std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
                          std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors,
                          std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages,
                          const std::function<void(const fs::path&)>& changeOwnership) {

    // 1. Check for early cancellation
    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        logOperationResult(false, true, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
        return false;
    }

    std::error_code ec;

    // 2. Attempt Fast Move (Rename)
    fs::rename(srcPath, destPath, ec);

    if (!ec) {
        completedBytes->fetch_add(fileSize, std::memory_order_relaxed);
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
        return true;
    }

    // 3. Fallback: Cross-device move (Copy + Delete)
    ec.clear();
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);

    if (success) {
        std::error_code deleteEc;
        if (!fs::remove(srcPath, deleteEc)) {
            // Heavy lifting for the error message formatting
            const MainTheme* theme = getActiveTheme();
            const bool isOriginal  = (globalTheme == "original");
            const std::string_view errLabel = isOriginal ? UI::Palette::Red    : theme->secondary;
            const std::string_view errPath  = isOriginal ? UI::Palette::Yellow : theme->warning;

            // Build display source using our optimized pattern
            std::string displaySrc;
            if (!displayConfig::toggleNamesOnly) {
                displaySrc.reserve(srcDir.size() + srcFile.size() + 1);
                displaySrc.append(srcDir).append("/").append(srcFile);
            } else {
                displaySrc = std::string(srcFile);
            }

            std::string msg;
            msg.reserve(160 + displaySrc.size() + deleteEc.message().size());
            msg.append(errLabel).append("Move completed but failed to remove source file: ")
               .append(errPath).append("'").append(displaySrc).append("'")
               .append(UI::Palette::BoldReset).append(errLabel).append(" - ").append(deleteEc.message())
               .append(UI::Palette::BoldReset);

            verboseErrors.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            return true; // The move technically worked, just the cleanup failed
        }

        // Cleanup success
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    } else {
        // Copy failed or was cancelled
        logOperationResult(false, GlobalState::g_operationCancelled.load(std::memory_order_acquire),
                           ec, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    }

    return success;
}

/**
 * @brief Specialized move operation for multiple destinations.
 */
bool performMultiDestMoveOperation(
    const fs::path& srcPath,
    const fs::path& destPath,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDirProcessed,
    std::string_view destFile,
    std::atomic<size_t>* completedBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    std::vector<std::string>& verboseIsos,
    std::vector<std::string>& verboseErrors,
    std::atomic<bool>& operationSuccessful,
    const std::function<void()>& batchInsertMessages,
    const std::function<void(const fs::path&)>& changeOwnership)
{
    std::error_code ec;

    // bufferedCopyWithProgress usually handles the heavy lifting
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);

    if (success) {
        changeOwnership(destPath);
    }

    // logOperationResult must also be updated to accept string_view for this to be 100% copy-free
    logOperationResult(success, GlobalState::g_operationCancelled.load(), ec, "moving",
                       srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks,
                       operationSuccessful, batchInsertMessages);

    return success;
}

/**
 * @brief Specialized copy operation.
 */
bool performCopyOperation(
    const fs::path& srcPath,
    const fs::path& destPath,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDirProcessed,
    std::string_view destFile,
    std::atomic<size_t>* completedBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    std::vector<std::string>& verboseIsos,
    std::vector<std::string>& verboseErrors,
    std::atomic<bool>& operationSuccessful,
    const std::function<void()>& batchInsertMessages,
    const std::function<void(const fs::path&)>& changeOwnership)
{
    std::error_code ec;

    // Execute the actual copy
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);

    if (success) {
        changeOwnership(destPath);
    }

    // Ensure logOperationResult also takes string_view to avoid copies here
    logOperationResult(success, GlobalState::g_operationCancelled.load(), ec, "copying",
                       srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks,
                       operationSuccessful, batchInsertMessages);

    return success;
}

void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

/**
 * @brief High-level handler that iterates through ISO files to perform CP, MV, or RM.
 * @details Manages thread-safe updates to reporting sets and handles ownership changes
 * for newly created files.
 *
 * It:
 * - Processes multiple destination directories (semicolon-separated in userDestDir)
 * - Batches success/error messages (every 50 entries) to reduce mutex contention
 * - Changes ownership of new files to the real user (via chown)
 * - Handles three operation types:
 *   - Delete: Calls performDeleteOperation
 *   - Move: Single or multi-destination move with source cleanup
 *   - Copy: Single or multi-destination copy
 * - Records successful destination paths for database indexing
 *
 * @param isoFiles            Files to operate on in this chunk
 * @param isoFilesCopy        Master file list (for existence validation)
 * @param userDestDir         Semicolon-separated destination directories
 * @param isMove              True for move operation
 * @param isCopy              True for copy operation
 * @param isDelete            True for delete operation
 * @param completedBytes      Atomic counter for progress tracking (bytes processed)
 * @param completedTasks      Atomic counter for successful operations
 * @param failedTasks         Atomic counter for failed operations
 * @param overwriteExisting   If true, replace existing files at destination
 * @param successfulDestPaths Vector to collect successful destination paths (optional)
 * @param destPathsMutex      Mutex protecting successfulDestPaths
 *
 * @note For multi-destination moves, the source file is removed once at least
 *       one copy succeeds — not necessarily all copies.
 * @note Ownership is restored using the real user ID from sudo/setuid context.
 */
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, const std::string& userDestDir, bool isMove, bool isCopy,
							bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks,
							std::atomic<size_t>* failedTasks, bool overwriteExisting, std::vector<std::string>* successfulDestPaths,std::mutex* destPathsMutex) {

    std::atomic<bool> operationSuccessful(true);

    uid_t real_uid;
    gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> verboseIsos;
    std::vector<std::string> verboseErrors;

    const size_t BATCH_SIZE = 50;

    auto batchInsertMessages = [&]() {
        if (verboseIsos.size() >= BATCH_SIZE || verboseErrors.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
            verboseSets.operationFailed.insert(verboseErrors.begin(), verboseErrors.end());
            verboseSets.operationCompleted.insert(verboseIsos.begin(), verboseIsos.end());
            verboseIsos.clear();
            verboseErrors.clear();
        }
    };

    std::vector<std::string> destDirs;
    std::istringstream iss(userDestDir);
    std::string destDir;
    while (std::getline(iss, destDir, ';'))
        destDirs.push_back(fs::path(destDir).string());

    auto changeOwnership = [&](const fs::path& path) {
        [[maybe_unused]] int ret = chown(path.c_str(), real_uid, real_gid);
    };

    const std::unordered_set<std::string> isoFilesCopySet(isoFilesCopy.begin(), isoFilesCopy.end());

    auto executeOperation = [&](const std::vector<const std::string*>& files) {
        for (const auto* operateIso : files) {
            fs::path srcPath(*operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.native(), "cp_mv_rm");

            struct stat st;
            size_t fileSize = (stat(srcPath.c_str(), &st) == 0) ? st.st_size : 0;

            if (isDelete) {
                performDeleteOperation(srcPath, srcDir, srcFile, fileSize,
                                       completedBytes, completedTasks, failedTasks,
                                       verboseIsos, verboseErrors, operationSuccessful,
                                       batchInsertMessages);
                continue;
            }

            bool atLeastOneCopySucceeded = false;
            int validDestinations = 0;

            for (size_t i = 0; i < destDirs.size(); ++i) {
                const auto& dst = destDirs[i];
                fs::path destPath = fs::path(dst) / srcPath.filename();
                // Always use full destination Dir for operations
                auto [_, destFile] = extractDirectoryAndFilename(destPath.native(), "cp_mv_rm");
				const std::string& destDirProcessed = dst;

                if (fs::absolute(srcPath) == fs::absolute(destPath)) {
                    std::string operation = isMove ? "move" : "copy";
                    reportErrorCpMvRm("same_file", srcDir, srcFile, "", "", operation,
                              verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                    continue;
                }
                ++validDestinations;

                if (!fs::exists(srcPath)) {
                    reportErrorCpMvRm("source_missing", srcDir, srcFile, "", "", "",
                              verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                    continue;
                }

                if (fs::exists(destPath)) {
                    if (overwriteExisting) {
						std::error_code ec;
                        if (!fs::remove(destPath, ec)) {
                            reportErrorCpMvRm("overwrite_failed", "", "", destDirProcessed, ec.message(), "",
                                      verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                            continue;
                        }
                    } else {
                        std::string operation = isCopy ? "copying" : "moving";
                        reportErrorCpMvRm("file_exists", srcDir, srcFile, destDirProcessed, "",
                                  operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                        continue;
                    }
                }

                bool success = false;

                if (isMove && destDirs.size() > 1) {
                    success = performMultiDestMoveOperation(
                        srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                        completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                        operationSuccessful, batchInsertMessages, changeOwnership);
                    if (success) {
                        atLeastOneCopySucceeded = true;
                        if (successfulDestPaths && destPathsMutex) {
                            std::lock_guard<std::mutex> lock(*destPathsMutex);
                            successfulDestPaths->push_back(destPath.string());
                        }
                    }
                } else if (isMove) {
                    success = performMoveOperation(
                        srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                        fileSize, completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                        operationSuccessful, batchInsertMessages, changeOwnership);
                    if (success) {
                        if (successfulDestPaths && destPathsMutex) {
                            std::lock_guard<std::mutex> lock(*destPathsMutex);
                            successfulDestPaths->push_back(destPath.string());
                        }
                    }
                } else {  // isCopy
                    success = performCopyOperation(
                        srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                        completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                        operationSuccessful, batchInsertMessages, changeOwnership);
                    if (success) {
                        if (successfulDestPaths && destPathsMutex) {
                            std::lock_guard<std::mutex> lock(*destPathsMutex);
                            successfulDestPaths->push_back(destPath.string());
                        }
                    }
                }
            }

            if (isMove && destDirs.size() > 1 && validDestinations > 0 && atLeastOneCopySucceeded) {
                std::error_code deleteEc;
                if (!fs::remove(srcPath, deleteEc)) {
                    reportErrorCpMvRm("remove_after_move", srcDir, srcFile, "", deleteEc.message(), "",
                              verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                }
            }
        }
    };

    std::vector<const std::string*> isoFilesToOperate;
    for (const auto& iso : isoFiles) {
        if (isoFilesCopySet.count(iso)) {
            fs::path isoPath(iso);
            if (fs::exists(isoPath)) {
                isoFilesToOperate.push_back(&iso);
            } else {
                auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.native(), "cp_mv_rm");
                reportErrorCpMvRm("missing_file", isoDir, isoFile, "", "", "",
                          verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
            }
        }
    }

    executeOperation(isoFilesToOperate);

    {
        std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
        verboseSets.operationFailed.insert(verboseErrors.begin(), verboseErrors.end());
        verboseSets.operationCompleted.insert(verboseIsos.begin(), verboseIsos.end());
    }
}
