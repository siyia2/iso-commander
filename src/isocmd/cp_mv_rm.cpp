// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../threadpool.h"


// Function to process selected indices for cpMvDel accordingly
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, 
    const std::string& process, std::set<std::string>& operationIsos, 
    std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, 
    bool& promptFlag, int& maxDepth, bool& umountMvRmBreak, bool& historyPattern, bool& verbose) {
    
    std::string userDestDir;
    std::set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);
    
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
        for (const auto& errorMsg : uniqueErrorMessages) {
            std::cerr << "\033[1;93m" << errorMsg << "\033[0;1m\n";
        }
        if (!processedIndices.empty()) std::cout << "\n";
    }

    if (processedIndices.empty()) {
        clearScrollBuffer();
        umountMvRmBreak = false;
        std::cout << "\n\033[1;91mNo valid indices to be " << operationDescription << ".\033[1;91m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        clear_history();
        return;
    }

    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
    std::vector<std::vector<int>> indexChunks;
    const size_t maxFilesPerChunk = 5;

    size_t totalFiles = processedIndices.size();
    size_t filesPerThread = (totalFiles + numThreads - 1) / numThreads;
    size_t chunkSize = std::min(maxFilesPerChunk, filesPerThread);

    auto it = processedIndices.begin();
    for (size_t i = 0; i < totalFiles; i += chunkSize) {
        auto chunkEnd = std::next(it, std::min(chunkSize, 
            static_cast<size_t>(std::distance(it, processedIndices.end()))));
        indexChunks.emplace_back(it, chunkEnd);
        it = chunkEnd;
    }

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirRm(isoFiles, indexChunks, userDestDir, 
        operationColor, operationDescription, umountMvRmBreak, historyPattern, isDelete, isCopy, abortDel);
    
    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
        return;
    }

    clearScrollBuffer();
    std::cout << "\033[1m\n";

    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(isoFiles[index - 1]);
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    size_t totalBytes = getTotalFileSize(filesToProcess);
    size_t totalTasks = filesToProcess.size();
    
    // Adjust total bytes for copy operations with multiple destinations
    if (isCopy || isMove) {
        size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;  // Also adjust total tasks for multiple destinations
    }
    
    std::atomic<bool> isProcessingComplete(false);

    // Create progress thread with both byte and task tracking
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
        totalBytes, &completedTasks, totalTasks, &isProcessingComplete, &verbose);

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(
            chunk.begin(),
            chunk.end(),
            std::back_inserter(isoFilesInChunk),
            [&isoFiles](size_t index) { return isoFiles[index - 1]; }
        );

        futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), 
            &isoFiles, &operationIsos, &operationErrors, &userDestDir, 
            isMove, isCopy, isDelete, &completedBytes, &completedTasks]() {
            handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, 
                operationErrors, userDestDir, isMove, isCopy, isDelete, 
                &completedBytes, &completedTasks);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    progressThread.join();

    promptFlag = false;
    maxDepth = 0;
    
    if (!isDelete) {
        manualRefreshCache(userDestDir, promptFlag, maxDepth, historyPattern);
    }
    
    if (!isDelete && !operationIsos.empty()) {
        saveHistory(historyPattern);
        clear_history();
    }

    clear_history();
    promptFlag = true;
    maxDepth = -1;
}


// Function to prompt for userDestDir and Delete confirmation
std::string userDestDirRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel) {
	
	    auto displaySelectedIsos = [&]() {
        std::cout << "\n";
        for (const auto& chunk : indexChunks) {
            for (const auto& index : chunk) {
                auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoFiles[index - 1]);
                std::cout << "\033[1m-> " << isoDirectory << "/\033[1;95m" << isoFilename << "\033[0;1m\n";
            }
        }
    };
	if (!isDelete) {
        while (true) {
			// Restore readline autocomplete and screen clear bindings
			rl_bind_key('\f', rl_clear_screen);
			rl_bind_key('\t', rl_complete);
			if (!isCopy) {
				umountMvRmBreak = true;
			}
            clearScrollBuffer();
            displaySelectedIsos();
            clear_history();
            historyPattern = false;
            loadHistory(historyPattern);
            userDestDir.clear();
			
            std::string prompt = "\n\001\033[1;92m\002DestinationDirs\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " + operationColor + operationDescription + "\001\033[1;94m\002 into (multi-path separator: \001\033[1m\002\001\033[1;93m\002;\001\033[1;94m\002), ↵ return:\n\001\033[0;1m\002";
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            std::string mainInputString(input.get());
            
            rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
			rl_bind_key('\t', prevent_clear_screen_and_tab_completion);

            if (mainInputString.empty()) {
                umountMvRmBreak = false;
                userDestDir = "";
                clear_history();
                return userDestDir;
            } else {
                userDestDir = mainInputString;
                add_history(input.get());
                break;
            }

        }
    } else {
        clearScrollBuffer();
        displaySelectedIsos();

        std::string confirmation;
        std::string prompt = "\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be \001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (y/n):\001\033[0;1m\002 ";
        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string mainInputString(input.get());

        if (!(mainInputString == "y" || mainInputString == "Y")) {
            umountMvRmBreak = false;
            abortDel = true;
            userDestDir = "";
            std::cout << "\n\033[1;93mDelete operation aborted by user.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return userDestDir;
        }
        umountMvRmBreak = true;
    }
    return userDestDir;
}

namespace fs = std::filesystem;


// Function to buffer file copying
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {

    const size_t bufferSize = 8 * 1024 * 1024; // 8MB buffer
    std::vector<char> buffer(bufferSize);
    
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
    
    while (true) {
        input.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = input.gcount();
        
        if (bytesRead == 0) {
            break;
        }
        
        output.write(buffer.data(), bytesRead);
        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        
        completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
    }
    
    return true;
}

// Function to handle cpMvDel
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks) {
    
    bool operationSuccessful = true;
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname, operationErrors);

    std::vector<std::string> isoFilesToOperate;
    std::vector<std::string> destDirs;
    std::istringstream iss(userDestDir);
    std::string destDir;
    while (std::getline(iss, destDir, ';')) {
        destDirs.push_back(fs::path(destDir).string());
    }

    auto changeOwnership = [&](const fs::path& path) -> bool {
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) == 0) {
            if (file_stat.st_uid != real_uid || file_stat.st_gid != real_gid) {
                if (chown(path.c_str(), real_uid, real_gid) != 0) {
                    std::string errorMessage = "\033[1;91mFailed to change ownership of '" + path.string() + "': " + 
                                               std::string(strerror(errno)) + "\033[0;1m";
                    operationErrors.emplace(errorMessage);
                    return false;
                }
            }
        } else {
            std::string errorMessage = "\033[1;91mFailed to get file information for '" + path.string() + "': " + 
                                       std::string(strerror(errno)) + "\033[0;1m";
            operationErrors.emplace(errorMessage);
            return false;
        }
        return true;
    };

    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string());
            
            struct stat st;
            size_t fileSize = 0;
            if (stat(srcPath.c_str(), &st) == 0) {
                fileSize = st.st_size;
            }

            if (isDelete) {
                std::error_code ec;
                if (fs::remove(srcPath, ec)) {
                    completedBytes->fetch_add(fileSize, std::memory_order_relaxed);
                    std::string operationInfo = "\033[1mDeleted: \033[1;92m'" + srcDir + "/" + srcFile + "'\033[1m\033[0;1m.";
                    operationIsos.emplace(operationInfo);
                } else {
                    std::string errorMessageInfo = "\033[1;91mError deleting: \033[1;93m'" + srcDir + "/" + srcFile +
                        "'\033[1;91m: " + ec.message() + "\033[1;91m.\033[0;1m";
                    operationErrors.emplace(errorMessageInfo);
                    operationSuccessful = false;
                }
                // Increment task counter for delete operation
                completedTasks->fetch_add(1, std::memory_order_relaxed);
            } else {
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& destDir = destDirs[i];
                    fs::path destPath = fs::path(destDir) / srcPath.filename();
                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string());

                    // Check if the destination directory exists and is valid
                    if (!fs::exists(destDir) || !fs::is_directory(destDir)) {
                        std::string errorMessageInfo = "\033[1;91mError " +
                            std::string(isCopy ? "copying" : "moving") +
                            ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                            " to '" + destDir + "': Invalid or non-existent destination directory\033[1;91m.\033[0;1m";
                        operationErrors.emplace(errorMessageInfo);
                        operationSuccessful = false;

                        // Increment task counter even for invalid directories
                        completedTasks->fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    std::error_code ec;
                    bool success = false;
                    
                    if (isCopy) {
                        success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                    } else if (isMove) {
                        if (i < destDirs.size() - 1) {
                            success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                        } else {
                            fs::rename(srcPath, destPath, ec);
                            if (!ec) {
                                completedBytes->fetch_add(fileSize, std::memory_order_relaxed);
                                success = true;
                            }
                        }
                    }

                    if (!success || ec) {
                        std::string errorMessageInfo = "\033[1;91mError " +
                            std::string(isCopy ? "copying" : (i < destDirs.size() - 1 ? "moving" : "moving")) +
                            ": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                            " to '" + destDirProcessed + "/': " + ec.message() + "\033[1;91m.\033[0;1m";
                        operationErrors.emplace(errorMessageInfo);
                        operationSuccessful = false;
                    } else {
                        if (!changeOwnership(destPath)) {
                            operationSuccessful = false;
                        } else {
                            std::string operationInfo = "\033[1m" +
                                std::string(isCopy ? "Copied" : (i < destDirs.size() - 1 ? "Moved" : "Moved")) +
                                ": \033[1;92m'" + srcDir + "/" + srcFile + "'\033[1m\033[0;1m" +
                                " to \033[1;94m'" + destDirProcessed + "/" + destFile + "'\033[0;1m.";
                            operationIsos.emplace(operationInfo);
                        }
                    }

                    // Increment task counter for each destination path, regardless of success or failure
                    completedTasks->fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    for (const auto& iso : isoFiles) {
        fs::path isoPath(iso);
        auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string());

        auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
        if (it != isoFilesCopy.end()) {
            if (fs::exists(isoPath)) {
                isoFilesToOperate.push_back(iso);
            } else {
                std::string errorMessageInfo = "\033[1;35mFile not found: \033[0;1m'" +
                    isoDir + "/" + isoFile + "'\033[1;35m.\033[0;1m";
                operationErrors.emplace(errorMessageInfo);
                operationSuccessful = false;
                // Increment task counter for failed operation
                completedTasks->fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            std::string errorMessageInfo = "\033[1;93mFile not found in cache: \033[0;1m'" +
                isoDir + "/" + isoFile + "'\033[1;93m.\033[0;1m";
            operationErrors.emplace(errorMessageInfo);
            operationSuccessful = false;
            // Increment task counter for failed operation
            completedTasks->fetch_add(1, std::memory_order_relaxed);
        }
    }

    executeOperation(isoFilesToOperate);
}
