// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"


// Function to generate entries for selected ISO files
std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles) {
    std::vector<std::string> entries;

    // --- Original Color Constants ---
    static constexpr std::string_view defaultColor = "\033[0;1m";
    static constexpr std::string_view magentaBold  = "\033[95;1m";
    static constexpr std::string_view reset        = "\033[0m";
    static constexpr std::string_view bold         = "\033[1m";

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalListTheme == "original");

    for (const auto& chunk : indexChunks) {
        for (int index : chunk) {
            // Index safety: assume 1-based indexing
            if (index <= 0 || static_cast<size_t>(index) > isoFiles.size()) continue;

            auto [isoDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");
            
            std::string entry;
            // Pre-allocate: ~64 chars for ANSI codes + path length
            entry.reserve(isoDir.length() + filename.length() + 64);

            // Structure: [Bold]-> [PathColor]Path/ [FileColor]Filename [Reset]
            entry += bold;
            entry += "-> ";

            // 1. Handle Directory Path
            if (!displayConfig::toggleNamesOnly) {
                // In printList, original mode uses defaultColor for the directory
                entry += (isOriginal ? defaultColor : theme->muted);
                entry.append(isoDir);
                entry += (isOriginal ? defaultColor : ""); // Maintain consistency with printList reset behavior
                entry += "/";
            }

            // 2. Handle Filename (Magenta in original mode)
            entry += (isOriginal ? magentaBold : theme->accent);
            entry.append(filename);
            
            // 3. Reset and Newline
            entry += reset;
            entry += '\n';

            entries.push_back(std::move(entry));
        }
    }

    return entries;
}


// Validate a Linux path and return an error message if invalid, empty string if valid
static std::string validateLinuxPath(const std::string& path) {
    if (path.empty() || path[0] != '/')
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 must be absolute (start with '/').\001\033[0m\002";

    for (char c : path)
        if (iscntrl(static_cast<unsigned char>(c)))
            return "\001\033[1;91m\002Error: Control characters in path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002.\001\033[0m\002";

    // Reject paths that are only whitespace
    if (path.find_first_not_of(" \t") == std::string::npos)
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 is blank.\001\033[0m\002";

    struct stat pathStat;
    if (stat(path.c_str(), &pathStat) != 0)
        return "\001\033[1;91m\002Error: Path \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 does not exist.\001\033[0m\002";

    if (!S_ISDIR(pathStat.st_mode))
        return "\001\033[1;91m\002Error: \001\033[1;93m\002'" + path + "'\001\033[1;91m\002 is not a directory.\001\033[0m\002";

    return "";
}


// Function to handle rm including pagination
bool handleDeleteOperation(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::vector<std::vector<int>>& indexChunks, 
bool& umountMvRmBreak, bool& abortDel) {
    
    // --- Theme & Color Setup ---
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalListTheme == "original");

    // Map theme colors: Original uses Green (92) and Blue (94)
    std::string green = isOriginal ? "\033[1;92m" : std::string(theme->accent);
    std::string blue  = isOriginal ? "\033[1;94m" : std::string(theme->secondary);
    std::string red   = "\033[1;91m"; // Keep red for the "Deleted" warning
    std::string reset = "\033[0;1m";

    bool isPageTurn = false;
    bool disablePagination = (ITEMS_PER_PAGE <= 0 || isoFiles.size() <= ITEMS_PER_PAGE);

    auto setupEnv = [&]() {
        if (!disablePagination) {
            rl_bind_key('\f', clear_screen_and_buffer);
        } else {
            rl_bind_key('\f', [](int, int) -> int { return 0; });
        }
    };

    // Generate entries based on selected indexes
    std::vector<std::string> entries = generateIsoEntries(indexChunks, isoFiles);
    sortFilesCaseInsensitive(entries);

    std::string promptPrefix = "\n";
    
    // Theme-aware prompt construction
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

            // Batch output for performance
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
                
                // Final abort message also uses yellow/green themed logic if desired
                std::cout << "\n\033[1;93mrm operation aborted by user.\033[0;1m\n";
                std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
                
                // Clear any leftover input
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                return false;
            }
        }
    }
}


// This function manages the user input for setting the destination directory and operation type
std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, 
std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel, 
bool& overwriteExisting) {

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

			// --- Theme & Color Setup ---
			const ListTheme* theme = getActiveTheme();
			const bool isOriginal = (globalListTheme == "original");

			// Use std::string to allow concatenation with +
			// We cast the string_view to std::string or wrap the literal
			std::string green = isOriginal ? "\001\033[1;92m\002" : std::string("\001") + std::string(theme->accent) + "\002";
			std::string blue  = isOriginal ? "\001\033[1;94m\002" : std::string("\001") + std::string(theme->secondary) + "\002";
			std::string reset = "\001\033[0;1m\002";

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
            if (userInput.empty()) {
				continue;
			}
            // Strip trailing " -o" flag once
            bool hasOverwriteFlag = (userInput.size() >= 3 && userInput.substr(userInput.size() - 3) == " -o");
            std::string pathsInput = hasOverwriteFlag ? userInput.substr(0, userInput.size() - 3) : userInput;

            // Validate all semicolon-separated paths
            bool pathsValid = true;
            std::string invalidPathError;
            std::string historyInput;

            std::istringstream iss(pathsInput);
            std::string token;
            while (std::getline(iss, token, ';')) {
                // Trim whitespace
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

            // Save to history without the -o flag
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


// Function to copy a file with progress reporting
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


// Shared helper to log operation result and call batchInsertMessages
static void logOperationResult(bool success, bool cancelled, const std::error_code& ec, const std::string& verb, const std::string& srcDir, const std::string& srcFile, 
const std::string& destDirProcessed, const std::string& destFile,std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<size_t>* completedTasks, 
std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful,const std::function<void()>& batchInsertMessages) {
    if (!success || ec) {
        std::string errorDetail = cancelled ? "Cancelled" : ec.message();
        verboseErrors.push_back("\033[1;91mError " + verb + ": \033[1;93m'" +
                                (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile +
                                "'\033[1;91m to '" + destDirProcessed + "/': " + errorDetail + "\033[1;91m.\033[0;1m");
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    } else {
        std::string pastVerb = (verb == "moving") ? "Moved" : "Copied";
        verboseIsos.push_back("\033[0;1m" + pastVerb + ": \033[1;92m'" +
                              (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile +
                              "'\033[0;1m to \033[1;94m'" + destDirProcessed + "/" + destFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    }
    batchInsertMessages();
}


// Function to perform Delete operation
void performDeleteOperation(const fs::path& srcPath, const std::string& srcDir, const std::string& srcFile, size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, 
std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages) {
    if (g_operationCancelled.load()) {
        verboseErrors.push_back("\033[1;91mError deleting: \033[1;93m'" +
                                (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile +
                                "'\033[1;91m: Cancelled.\033[0;1m");
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
        batchInsertMessages();
        return;
    }

    std::error_code ec;
    if (fs::remove(srcPath, ec)) {
        completedBytes->fetch_add(fileSize);
        verboseIsos.push_back("\033[0;1mDeleted: \033[1;92m'" +
                              (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile + "'\033[0;1m.");
        completedTasks->fetch_add(1, std::memory_order_acq_rel);
    } else {
        verboseErrors.push_back("\033[1;91mError deleting: \033[1;93m'" +
                                (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile +
                                "'\033[1;91m: " + ec.message() + ".\033[0;1m");
        failedTasks->fetch_add(1, std::memory_order_acq_rel);
        operationSuccessful.store(false);
    }
    batchInsertMessages();
}


// Function to perform Move operation
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
        // Fast same-filesystem rename succeeded
        completedBytes->fetch_add(fileSize);
        changeOwnership(destPath);
        logOperationResult(true, false, {}, "moving", srcDir, srcFile, destDirProcessed, destFile,
                           verboseIsos, verboseErrors, completedTasks, failedTasks, operationSuccessful, batchInsertMessages);
        return true;
    }

    // Cross-filesystem fallback: copy then delete
    ec.clear();
    bool success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
    if (success) {
        std::error_code deleteEc;
        if (!fs::remove(srcPath, deleteEc)) {
            verboseErrors.push_back("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" +
                                    (!displayConfig::toggleNamesOnly ? srcDir + "/" : "") + srcFile +
                                    "'\033[1;91m - " + deleteEc.message() + "\033[0m");
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


// Helper function for multi-destination Move operation
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


// Function to perform Copy operation
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


// Function to handle CpMvRm
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

    // Build list of valid ISO files to operate on
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
