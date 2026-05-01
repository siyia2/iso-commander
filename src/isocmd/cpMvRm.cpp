// SPDX-License-Identifier: GPL-3.0-or-later

#include "../state.h"
#include "../display.h"
#include "../themes.h"
#include "../concurrency.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../inputHandling.h"
#include "../sort.h"
#include "../history.h"
#include "../verbose.h"
#include "../stringManipulation.h"

namespace fs = std::filesystem;

void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages);

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
        return msg + std::string(UI::Palette::RL_Reset);
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

std::string handlePaginatedDisplay(const std::vector<std::string>& entries, std::unordered_set<std::string>& uniqueErrorMessages, const std::string& promptPrefix, 
const std::string& promptSuffix, const std::function<void()>& setupEnvironmentFn, bool& isPageTurn);

/**
 * @brief Orchestrates the user confirmation prompt for file deletion operations.
 * @details Handles paginated or batch display of files marked for deletion and captures user input.
 * * @return True if the user confirms deletion (Y), false otherwise.
 */
bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::vector<std::vector<int>>& indexChunks,
bool& umountMvRmBreak, bool& abortDel) {
    rl_attempted_completion_function = nullptr;
    
    reset_custom_keybindingsForSelect();

    const CpMvRmColors colors = getCpMvRmColors();

    std::string green = std::string(colors.prompt_green);
    std::string blue  = std::string(colors.prompt_blue);
    std::string red   = std::string(UI::Palette::Red);
    std::string reset = std::string(UI::Palette::BoldReset);

    bool isPageTurn = false;
    bool disablePagination = (GlobalState::ITEMS_PER_PAGE <= 0 || isoFiles.size() <= GlobalState::ITEMS_PER_PAGE);

    auto setupEnv = [&]() {
        if (!disablePagination) {
            rl_bind_key('\f', clear_screen_and_buffer);
        } else {
            rl_bind_key('\f', [](int, int) -> int { return 0; });
        }
    };

    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);

    std::string promptPrefix = "\n";
    std::string promptSuffix =
        "\n\001" + blue + "\002The selected \001" +
        green + "\002ISO\001" +
        blue + "\002 will be \001" +
        red + "\002*PERMANENTLY DELETED FROM DISK*\001" +
        blue + "\002. Proceed? (y/n):\001" +
        reset + "\002 ";

    while (true) {
        std::string userInput;

        if (disablePagination) {
            displayErrors(uniqueErrorMessages);
            std::cout << promptPrefix;

            std::ostringstream batch;
            const size_t BATCH_SIZE = 100;
            for (size_t i = 0; i < entries.size(); i += BATCH_SIZE) {
                batch.str(""); batch.clear();
                size_t end = std::min(i + BATCH_SIZE, entries.size());
                for (size_t j = i; j < end; ++j) batch << entries[j];
                std::cout << batch.str();
            }

            std::cout << promptSuffix;
            std::cout.flush();

            char* input = readline("");
            if (!input) {
                userInput = "EOF_SIGNAL";
            } else {
                userInput = std::string(input);
                free(input);
            }
        } else {
            userInput = handlePaginatedDisplay(
                entries, uniqueErrorMessages, promptPrefix, promptSuffix, setupEnv, isPageTurn
            );
        }

        rl_bind_key('\f', prevent_readline_keybindings);

        if (userInput.empty()) continue;

        if (userInput == "EOF_SIGNAL") {
            umountMvRmBreak = false;
            abortDel = true;
            return false;
        }

        if (!isPageTurn) {
            if (userInput == "Y" || userInput == "y") {
                umountMvRmBreak = true;
                return true;
            } else {
                umountMvRmBreak = false;
                abortDel = true;

                std::cout << "\n" << UI::Palette::Red << "rm" << colors.abort << " operation aborted by user." << UI::Palette::BoldReset << "\n";
                pressEnterToContinue();
                return false;
            }
        }
    }
}

void helpSearches(bool isCpMv, bool import2ISO);

/**
 * @brief Prompts the user for a destination directory for Copy or Move operations.
 * @details Validates the input path, handles pagination, and processes flags such as overwrite (-o).
 * * @return The validated destination directory path string.
 */
std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages,
std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel,
bool& overwriteExisting) {
    rl_attempted_completion_function = nullptr;
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);
    clearScrollBuffer();
    
	reset_custom_keybindingsForCpMvWrite2Usb();

    bool shouldContinue = true;
    std::string userInput;

    while (shouldContinue) {
        resetReadlinePagination();
        if (!isDelete) {
            bool isPageTurn = false;

            auto setupEnv = [&]() {
                enable_ctrl_d();
                setupSignalHandlerCancellations();
                rl_bind_key('\f', clear_screen_and_buffer);
                rl_bind_key('\t', rl_complete);
                if (!isCopy) umountMvRmBreak = true;
                if (!isPageTurn) {
                    clear_history();
                    filterHistory = false;
                    loadHistory(filterHistory);
                }
            };

            const CpMvRmColors colors = getCpMvRmColors();

            std::string green = "\001" + std::string(colors.prompt_green) + "\002";
            std::string blue  = "\001" + std::string(colors.prompt_blue) + "\002";
            std::string reset = "\001" + std::string(UI::Palette::BoldReset) + "\002";

            std::string promptPrefix = "\n";
            std::string promptSuffix =
                "\n" + green + "FolderPaths" +
                blue + " ↵ for selected " +
                green + "ISO" +
                blue + " to be " +
                operationColor + operationDescription +
                blue + " into, ? help, < return:\n" +
                reset;

            userInput = handlePaginatedDisplay(
                entries, uniqueErrorMessages, promptPrefix, promptSuffix, setupEnv, isPageTurn
            );

            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);

            if (userInput == "EOF_SIGNAL") { shouldContinue = false; break; }
            if (userInput == "?") {
                bool import2ISO = false, isCpMv = true;
                helpSearches(isCpMv, import2ISO);
                userDestDir = "";
                continue;
            }
            if (userInput == "<") {
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
                uniqueErrorMessages.insert(invalidPathError);
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
            bool proceedWithDelete = handleDeleteOperation(isoFiles, uniqueErrorMessages, indexChunks, umountMvRmBreak, abortDel);
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
static void logOperationResult(bool success, bool cancelled, const std::error_code& ec, const std::string& verb, const std::string& srcDir, const std::string& srcFile,
const std::string& destDirProcessed, const std::string& destFile, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<size_t>* completedTasks,
std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {

    const CpMvRmColors colors = getCpMvRmColors();

    const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    if (!success || ec) {
        std::string errorDetail = cancelled ? "Cancelled" : ec.message();
        std::string msg;
        msg.reserve(128);
        msg.append(colors.error_label).append("Error ").append(verb).append(": ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(" to '").append(destDirProcessed).append("/': ")
           .append(errorDetail).append(".")
           .append(UI::Palette::BoldReset).append(UI::Palette::BoldReset);
        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        std::string pastVerb = (verb == "moving") ? "Moved" : "Copied";
        std::string msg;
        msg.reserve(128);
        msg.append(colors.success_label).append(pastVerb).append(": ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(" to ")
           .append(colors.dest_path).append("'").append(destDirProcessed).append("/").append(destFile).append("'")
           .append(UI::Palette::BoldReset).append(UI::Palette::BoldReset).append(".");
        verboseIsos.push_back(std::move(msg));
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    batchInsertMessages();
}

/**
 * @brief Removes a single ISO file from the disk and logs the outcome.
 */
void performDeleteOperation(const fs::path& srcPath, const std::string& srcDir, const std::string& srcFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {

    const CpMvRmColors colors = getCpMvRmColors();

    const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    if (GlobalState::g_operationCancelled.load()) {
        std::string msg;
        msg.reserve(128);
        msg.append(colors.error_label).append("Error deleting: ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(": Cancelled.")
           .append(UI::Palette::BoldReset).append(UI::Palette::BoldReset);
        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
        batchInsertMessages();
        return;
    }

    std::error_code ec;
    if (fs::remove(srcPath, ec)) {
        completedBytes->fetch_add(fileSize);
        std::string msg;
        msg.reserve(128);
        msg.append(colors.success_label).append("Deleted: ")
           .append(colors.success_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.success_label).append(".")
           .append(UI::Palette::BoldReset);
        verboseIsos.push_back(std::move(msg));
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    } else {
        std::string msg;
        msg.reserve(128);
        msg.append(colors.error_label).append("Error deleting: ")
           .append(colors.error_path).append("'").append(displaySrc).append("'")
           .append(UI::Palette::BoldReset).append(colors.error_label).append(": ").append(ec.message()).append(".")
           .append(UI::Palette::BoldReset).append(UI::Palette::BoldReset);
        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    }
    batchInsertMessages();
}

/**
 * @brief Executes a file Move operation. 
 * @details Tries a quick filesystem rename first; if moving across different devices, 
 * falls back to a manual copy-then-delete procedure.
 */
bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages,
const std::function<void(const fs::path&)>& changeOwnership) {

    if (GlobalState::g_operationCancelled.load()) {
        logOperationResult(false, true, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
        return false;
    }

    std::error_code ec;
    fs::rename(srcPath, destPath, ec);

    if (!ec) {
        completedBytes->fetch_add(fileSize);
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
        return true;
    }

    ec.clear();
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) {
        std::error_code deleteEc;
        if (!fs::remove(srcPath, deleteEc)) {
            const MainTheme* theme = getActiveTheme();
            const bool isOriginal  = (globalTheme == "original");
            std::string_view errLabel = isOriginal ? UI::Palette::Red    : theme->secondary;
            std::string_view errPath  = isOriginal ? UI::Palette::Yellow : theme->warning;

            const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Move completed but failed to remove source file: ")
               .append(errPath).append("'").append(displaySrc).append("'")
               .append(UI::Palette::BoldReset).append(errLabel).append(" - ").append(deleteEc.message())
               .append(UI::Palette::BoldReset);
            verboseErrors.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            return true;
        }
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    } else {
        logOperationResult(false, GlobalState::g_operationCancelled.load(), ec, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    }
    return success;
}

/**
 * @brief Specialized move operation for multiple destinations.
 */
bool performMultiDestMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos,
std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {

    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) changeOwnership(destPath);
    logOperationResult(success, GlobalState::g_operationCancelled.load(), ec, "moving", srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    return success;
}

/**
 * @brief Specialized copy operation.
 */
bool performCopyOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos,
std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {

    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) changeOwnership(destPath);
    logOperationResult(success, GlobalState::g_operationCancelled.load(), ec, "copying", srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    return success;
}

void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

/**
 * @brief High-level handler that iterates through ISO files to perform CP, MV, or RM.
 * @details Manages thread-safe updates to reporting sets and handles ownership changes 
 * for newly created files.
 *
 * This function is designed to be called from thread pool workers. It:
 * - Processes multiple destination directories (semicolon-separated in userDestDir)
 * - Batches success/error messages (every 50 entries) to reduce mutex contention
 * - Changes ownership of new files to the real user (via chown)
 * - Handles three operation types:
 *   - Delete: Calls performDeleteOperation (no database recording)
 *   - Move: Single or multi-destination move with source cleanup
 *   - Copy: Single or multi-destination copy
 * - Records successful destination paths for database indexing
 *
 * @param isoFiles       Files to operate on in this chunk
 * @param isoFilesCopy   Master file list (for existence validation)
 * @param operationIsos  Set tracking successfully processed items
 * @param operationErrors Set tracking errors with themed messages
 * @param userDestDir    Semicolon-separated destination directories
 * @param isMove         True for move operation
 * @param isCopy         True for copy operation  
 * @param isDelete       True for delete operation
 * @param completedBytes Atomic counter for progress bar (bytes processed)
 * @param completedTasks Atomic counter for successful operations
 * @param failedTasks    Atomic counter for failed operations
 * @param overwriteExisting If true, replace existing files at destination
 * @param successfulDestPaths Vector to collect successful destination paths (optional)
 * @param destPathsMutex Mutex protecting successfulDestPaths
 *
 * @note For multi-destination moves, the source file is only removed after ALL
 *       copies succeed, and at least one copy was successful.
 * @note Ownership is restored using the real user ID from sudo/setuid context.
 */
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos,
std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks,
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
            operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
            operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
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

    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string(), "cp_mv_rm");

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
                auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string(), "cp_mv_rm");

                if (fs::absolute(srcPath) == fs::absolute(destPath)) {
                    std::string operation = isMove ? "move" : "copy";
                    reportErrorCpMvRm("same_file", srcDir, srcFile, "", "", operation,
                              verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
                    continue;
                }

                std::error_code ec;
                if (!fs::exists(dst, ec) || !fs::is_directory(dst, ec)) {
                    std::string operation = isCopy ? "copying" : "moving";
                    reportErrorCpMvRm("invalid_dest", srcDir, srcFile, dst, "Invalid destination",
                              operation, verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
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
                        // Record successful destination for multi‑dest move (each copy)
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

    std::vector<std::string> isoFilesToOperate;
    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string(), "cp_mv_rm");

        if (std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso) != isoFilesCopy.end()) {
            if (fs::exists(isoPath)) {
                isoFilesToOperate.push_back(iso);
            } else {
                reportErrorCpMvRm("missing_file", isoDir, isoFile, "", "", "",
                          verboseErrors, failedTasks, operationSuccessful, batchInsertMessages);
            }
        }
    }

    executeOperation(isoFilesToOperate);

    {
        std::lock_guard<std::mutex> lock(GlobalConcurrency::globalSetsMutex);
        operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
        operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
    }
}
