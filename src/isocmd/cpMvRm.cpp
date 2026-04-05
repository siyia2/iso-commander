// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"


std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
    std::vector<std::string> entries;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    for (const auto& chunk : indexChunks) {
        for (int index : chunk) {
            if (index <= 0 || static_cast<size_t>(index) > isoFiles.size()) continue;

            auto [isoDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");

            std::string entry;
            entry.reserve(isoDir.length() + filename.length() + 64);

            entry += originalColors::bold;
            entry += "-> ";

            if (!displayConfig::toggleNamesOnly) {
                entry += (isOriginal ? originalColors::boldAlt : theme->muted);
                entry.append(isoDir);
                entry += "/";
            }

            entry += (isOriginal ? originalColors::magenta : theme->accent);
            entry.append(filename);
            entry += originalColors::reset;
            entry += '\n';

            entries.push_back(std::move(entry));
        }
    }

    return entries;
}


static std::string validateLinuxPath(const std::string& path) {
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    // \001/\002 wrappers are required for readline prompt width calculation —
    // keep them even in themed mode so cursor position stays correct
    std::string errLabel = isOriginal
        ? std::string(originalColors::rl_red)
        : "\001" + std::string(theme->secondary) + "\002";
    std::string errPath  = isOriginal
        ? std::string(originalColors::rl_yellow)
        : "\001" + std::string(theme->warning) + "\002";

    auto makeError = [&](const std::string& msg) -> std::string {
        return msg + std::string(originalColors::rl_reset);
    };

    if (path.empty() || path[0] != '/')
        return makeError(errLabel + "Error: Path " + errPath + "'" + path + "'" + errLabel + " must be absolute (start with '/').");

    for (char c : path)
        if (iscntrl(static_cast<unsigned char>(c)))
            return makeError(errLabel + "Error: Control characters in path " + errPath + "'" + path + "'" + errLabel + ".");

    if (path.find_first_not_of(" \t") == std::string::npos)
        return makeError(errLabel + "Error: Path " + errPath + "'" + path + "'" + errLabel + " is blank.");

    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0)
        return makeError(errLabel + "Error: Path " + errPath + "'" + path + "'" + errLabel + " does not exist.");

    if (!S_ISDIR(pathStat.st_mode))
        return makeError(errLabel + "Error: " + errPath + "'" + path + "'" + errLabel + " is not a directory.");

    return "";
}


bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::vector<std::vector<int>>& indexChunks,
bool& umountMvRmBreak, bool& abortDel) {
    rl_attempted_completion_function = nullptr;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::string green = isOriginal ? std::string(originalColors::green) : std::string(theme->accent);
    std::string blue  = isOriginal ? std::string(originalColors::blue)  : std::string(theme->secondary);
    std::string red   = std::string(originalColors::red);
    std::string reset = std::string(originalColors::boldAlt);

    bool isPageTurn = false;
    bool disablePagination = (ITEMS_PER_PAGE <= 0 || isoFiles.size() <= ITEMS_PER_PAGE);

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
        blue + "\002. Proceed? (Y/N):\001" +
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

                const ListTheme* t = getActiveTheme();
                const bool orig    = (globalTheme == "original");
                std::string_view abortColor = orig ? originalColors::yellow   : t->warning;

                std::cout << "\n" << abortColor << "rm operation aborted by user." << originalColors::boldAlt << "\n";
                std::cout << color << "\n↵ to continue..." << reset;

                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return false;
            }
        }
    }
}


std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages,
std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel,
bool& overwriteExisting) {
    rl_attempted_completion_function = nullptr;
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);
    clearScrollBuffer();

    bool shouldContinue = true;
    std::string userInput;

    while (shouldContinue) {
        resetReadlinePagination();
        if (!isDelete) {
            bool isPageTurn = false;

            auto setupEnv = [&]() {
                enable_ctrl_d();
                setupSignalHandlerCancellations();
                g_operationCancelled.store(false);
                rl_bind_key('\f', clear_screen_and_buffer);
                rl_bind_key('\t', rl_complete);
                if (!isCopy) umountMvRmBreak = true;
                if (!isPageTurn) {
                    clear_history();
                    filterHistory = false;
                    loadHistory(filterHistory);
                }
            };

            const ListTheme* theme = getActiveTheme();
            const bool isOriginal  = (globalTheme == "original");

            std::string green = isOriginal ? "\001" + std::string(originalColors::green) + "\002" : "\001" + std::string(theme->accent)    + "\002";
            std::string blue  = isOriginal ? "\001" + std::string(originalColors::blue)  + "\002" : "\001" + std::string(theme->secondary) + "\002";
            std::string reset = "\001" + std::string(originalColors::boldAlt) + "\002";

            std::string promptPrefix = "\n";
            std::string promptSuffix =
                "\n" + green + "FolderPaths" +
                blue + " ↵ for selected " +
                green + "ISO" +
                blue + " to be " +
                operationColor + operationDescription +
                blue + " into, ? ↵ for help, < ↵ to return:\n" +
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


bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
    const size_t bufferSize = 8 * 1024 * 1024;
    std::vector<char> buffer(bufferSize);

    if (g_operationCancelled.load()) return false;

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

    while (!g_operationCancelled.load()) {
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

    if (g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        output.close();
        fs::remove(dst, ec);
        return false;
    }

    return true;
}


static void logOperationResult(bool success, bool cancelled, const std::error_code& ec, const std::string& verb, const std::string& srcDir, const std::string& srcFile,
const std::string& destDirProcessed, const std::string& destFile, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<size_t>* completedTasks,
std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view errLabel = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view errPath  = isOriginal ? originalColors::yellow   : theme->warning;
    std::string_view okLabel  = isOriginal ? originalColors::boldAlt  : theme->muted;
    std::string_view okPath   = isOriginal ? originalColors::green    : theme->primary;
    std::string_view destPath = isOriginal ? originalColors::blue     : theme->accent;

    const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    if (!success || ec) {
        std::string errorDetail = cancelled ? "Cancelled" : ec.message();
        std::string msg;
        msg.reserve(128);
        msg.append(errLabel).append("Error ").append(verb).append(": ")
           .append(errPath).append("'").append(displaySrc).append("'")
           .append(originalColors::reset).append(errLabel).append(" to '").append(destDirProcessed).append("/': ")
           .append(errorDetail).append(".")
           .append(originalColors::reset).append(originalColors::boldAlt);
        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        std::string pastVerb = (verb == "moving") ? "Moved" : "Copied";
        std::string msg;
        msg.reserve(128);
        msg.append(okLabel).append(pastVerb).append(": ")
           .append(okPath).append("'").append(displaySrc).append("'")
           .append(originalColors::reset).append(okLabel).append(" to ")
           .append(destPath).append("'").append(destDirProcessed).append("/").append(destFile).append("'")
           .append(originalColors::reset).append(originalColors::boldAlt).append(".");
        verboseIsos.push_back(std::move(msg));
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    batchInsertMessages();
}


void performDeleteOperation(const fs::path& srcPath, const std::string& srcDir, const std::string& srcFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal  = (globalTheme == "original");

    std::string_view errLabel = isOriginal ? originalColors::red     : theme->secondary;
    std::string_view errPath  = isOriginal ? originalColors::yellow  : theme->warning;
    std::string_view okLabel  = isOriginal ? originalColors::boldAlt : theme->muted;
    std::string_view okPath   = isOriginal ? originalColors::green   : theme->primary;

    const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;

    if (g_operationCancelled.load()) {
        std::string msg;
        msg.reserve(128);
        msg.append(errLabel).append("Error deleting: ")
           .append(errPath).append("'").append(displaySrc).append("'")
           .append(originalColors::reset).append(errLabel).append(": Cancelled.")
           .append(originalColors::reset).append(originalColors::boldAlt);
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
        msg.append(okLabel).append("Deleted: ")
           .append(okPath).append("'").append(displaySrc).append("'")
           .append(originalColors::reset).append(okLabel).append(".")
           .append(originalColors::reset);
        verboseIsos.push_back(std::move(msg));
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    } else {
        std::string msg;
        msg.reserve(128);
        msg.append(errLabel).append("Error deleting: ")
           .append(errPath).append("'").append(displaySrc).append("'")
           .append(originalColors::reset).append(errLabel).append(": ").append(ec.message()).append(".")
           .append(originalColors::reset).append(originalColors::boldAlt);
        verboseErrors.push_back(std::move(msg));
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    }
    batchInsertMessages();
}


bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages,
const std::function<void(const fs::path&)>& changeOwnership) {

    if (g_operationCancelled.load()) {
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
            const ListTheme* theme = getActiveTheme();
            const bool isOriginal  = (globalTheme == "original");
            std::string_view errLabel = isOriginal ? originalColors::red    : theme->secondary;
            std::string_view errPath  = isOriginal ? originalColors::yellow : theme->warning;

            const std::string displaySrc = (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile;
            std::string msg;
            msg.reserve(128);
            msg.append(errLabel).append("Move completed but failed to remove source file: ")
               .append(errPath).append("'").append(displaySrc).append("'")
               .append(originalColors::reset).append(errLabel).append(" - ").append(deleteEc.message())
               .append(originalColors::reset);
            verboseErrors.push_back(std::move(msg));
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            batchInsertMessages();
            return true;
        }
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    } else {
        logOperationResult(false, g_operationCancelled.load(), ec, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    }
    return success;
}


bool performMultiDestMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos,
std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {

    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) changeOwnership(destPath);
    logOperationResult(success, g_operationCancelled.load(), ec, "moving", srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    return success;
}


bool performCopyOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed,
const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos,
std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership) {

    std::error_code ec;
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) changeOwnership(destPath);
    logOperationResult(success, g_operationCancelled.load(), ec, "copying", srcDir, srcFile, destDirProcessed, destFile,
                       verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
    return success;
}


void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos,
std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes,
std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting) {

    std::atomic<bool> operationSuccessful(true);

    uid_t real_uid;
    gid_t real_gid;
    std::string real_username, real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    std::vector<std::string> verboseIsos;
    std::vector<std::string> verboseErrors;

    const size_t BATCH_SIZE = 1000;

    auto batchInsertMessages = [&]() {
        if (verboseIsos.size() >= BATCH_SIZE || verboseErrors.size() >= BATCH_SIZE) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
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
                    if (success) atLeastOneCopySucceeded = true;
                } else if (isMove) {
                    success = performMoveOperation(
                        srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                        fileSize, completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                        operationSuccessful, batchInsertMessages, changeOwnership);
                } else {
                    success = performCopyOperation(
                        srcPath, destPath, srcDir, srcFile, destDirProcessed, destFile,
                        completedBytes, completedTasks, failedTasks, verboseIsos, verboseErrors,
                        operationSuccessful, batchInsertMessages, changeOwnership);
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
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        operationErrors.insert(verboseErrors.begin(), verboseErrors.end());
        operationIsos.insert(verboseIsos.begin(), verboseIsos.end());
    }
}
