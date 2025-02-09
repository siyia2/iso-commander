// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"


// Function to process selected indices for cpMvDel accordingly
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& umountMvRmBreak, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound) {
	bool overwriteExisting =false;
    
    std::string userDestDir;
    std::set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove = (process == "mv");
    bool isCopy = (process == "cp");
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
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
    std::string processedUserDestDir = userDestDirRm(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
        operationColor, operationDescription, umountMvRmBreak, historyPattern, isDelete, isCopy, abortDel, overwriteExisting);
    
    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
		uniqueErrorMessages.clear();
        return;
    }
	uniqueErrorMessages.clear();
    clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing " + operationColor + process + "\033[0;1m operations... (\033[1;91mCtrl + c\033[0;1m:cancel)\n";

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
            isMove, isCopy, isDelete, &completedBytes, &completedTasks, &overwriteExisting]() {
            handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, 
                operationErrors, userDestDir, isMove, isCopy, isDelete, 
                &completedBytes, &completedTasks , overwriteExisting);
        }));
    }

    for (auto& future : futures) {
        future.wait();
        if (g_operationCancelled.load()) break;
    }

    isProcessingComplete.store(true);
    progressThread.join();

    promptFlag = false;
    maxDepth = 0;
    
    if (!isDelete) {
        manualRefreshCache(userDestDir, promptFlag, maxDepth, historyPattern, newISOFound);
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
std::string userDestDirRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting) {
	
    auto generateSelectedIsosPrompt = [&]() {
		std::ostringstream oss;
		for (const auto& chunk : indexChunks) {
			for (int index : chunk) {
				// Extract the directory and filename
				auto [shortDir, filename] = extractDirectoryAndFilename(isoFiles[index - 1], "cp_mv_rm");

				// Stream formatted entries directly into the buffer
				oss << "\033[1m-> " << shortDir << "/\033[1;95m" << filename << "\033[0;1m\n";
			}
		}
		return oss.str();
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
            clear_history();
            historyPattern = false;
            loadHistory(historyPattern);
            userDestDir.clear();
            if (!uniqueErrorMessages.empty()) {
				std::cout << "\n";
				for (const auto& err : uniqueErrorMessages) {
					std::cout << err << "\n";  // Newline ensures separate lines
				}
			}
			bool isCpMv= true;
            // Generate the prompt with selected ISOs at the beginning
            std::string selectedIsosPrompt = generateSelectedIsosPrompt();
			std::string prompt = "\n" + selectedIsosPrompt + 
			"\n\001\033[1;92m\002FolderPaths\001\033[1;94m\002 ↵ for selected \001\033[1;92m\002ISO\001\033[1;94m\002 to be " + 
			operationColor + operationDescription + 
			"\001\033[1;94m\002 into, ? ↵ for help, " +
			"↵ to return:\n\001\033[0;1m\002";

            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            // Check for EOF (Ctrl+D) or NULL input before processing
			if (!input.get()) {
				break; // Exit the loop on EOF
			}

			std::string mainInputString(input.get());

            rl_bind_key('\f', prevent_readline_keybindings);
            rl_bind_key('\t', prevent_readline_keybindings);
			
			if (mainInputString == "?") {
				helpSearches(isCpMv);
				continue;
			}
			
            if (mainInputString.empty()) {
                umountMvRmBreak = false;
                userDestDir = "";
                clear_history();
                return userDestDir;
            } else {
                userDestDir = mainInputString;
                // Check if userDestDir ends with ";^O"
				if (userDestDir.size() >= 4 && 
					userDestDir.substr(userDestDir.size() - 4) == " |^O") {
					// Set overwriteExisting to true
					overwriteExisting = true;
					// Remove ";^O" from userDestDir
					userDestDir = userDestDir.substr(0, userDestDir.size() - 4);
				} else {
					// Ensure overwriteExisting is false if the suffix is not present
					overwriteExisting = false;
				}
				// Remove ";^O" from the input before adding to history
				std::string historyInput = mainInputString;
				if (historyInput.size() >= 4 && 
					historyInput.substr(historyInput.size() - 4) == " |^O") {
					historyInput = historyInput.substr(0, historyInput.size() - 4);
				}
                add_history(historyInput.c_str());
                break;
            }
        }
    } else {
		rl_bind_key('\f', rl_clear_screen);
        clearScrollBuffer();
		if (!uniqueErrorMessages.empty()) {
				std::cout << "\n";
				for (const auto& err : uniqueErrorMessages) {
					std::cout << err << "\n";  // Newline ensures separate lines
				}
			}
        // Generate the prompt with selected ISOs at the beginning for deletion confirmation
        std::string selectedIsosPrompt = generateSelectedIsosPrompt();
        std::string prompt = "\n" + selectedIsosPrompt + "\n\001\033[1;94m\002The selected \001\033[1;92m\002ISO\001\033[1;94m\002 will be \001\033[1;91m\002*PERMANENTLY DELETED FROM DISK*\001\033[1;94m\002. Proceed? (y/n): \001\033[0;1m\002";

        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        
        rl_bind_key('\f', prevent_readline_keybindings);
        
        // Check for EOF (Ctrl+D) or NULL input before processing
		if (!input.get()) {
			userDestDir = "";
			abortDel = true;
			return userDestDir; // Exit on EOF
		}

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
std::atomic<bool> g_operationCancelled{false};

// Function to buffer file copying
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec) {
	g_operationCancelled = false;
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
    
    while (!g_operationCancelled.load()) { // Check cancellation flag at each iteration
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

    // Check if the operation was cancelled
    if (g_operationCancelled.load()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        output.close(); // Close the output stream before attempting to delete
        fs::remove(dst, ec); // Delete the partial file, ignore errors here
        return false;
    }
    
    return true;
}


// Function to handle cpMvDel
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, bool overwriteExisting) {

    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    bool operationSuccessful = true;
    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    getRealUserId(real_uid, real_gid, real_username, real_groupname, operationErrors);

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
                    auto [dir, file] = extractDirectoryAndFilename(path.string(), "cp_mv_rm");
                    std::string errorMessage = "\033[1;91mFailed to change ownership of '" + dir + "/" + file + "': " + strerror(errno) + "\033[0m";
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                        operationErrors.emplace(errorMessage);
                    }
                    return false;
                }
            }
        } else {
            auto [dir, file] = extractDirectoryAndFilename(path.string(), "cp_mv_rm");
            std::string errorMessage = "\033[1;91mFailed to get file info for '" + dir + "/" + file + "': " + strerror(errno) + "\033[0m";
            {
                std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                operationErrors.emplace(errorMessage);
            }
            return false;
        }
        return true;
    };

    auto executeOperation = [&](const std::vector<std::string>& files) {
        for (const auto& operateIso : files) {
            if (g_operationCancelled.load()) {
                completedTasks->fetch_add(files.size() - (&operateIso - files.data()));
                break;
            }

            fs::path srcPath(operateIso);
            auto [srcDir, srcFile] = extractDirectoryAndFilename(srcPath.string(), "cp_mv_rm");

            struct stat st;
            size_t fileSize = 0;
            if (stat(srcPath.c_str(), &st) == 0) {
                fileSize = st.st_size;
            }

            if (isDelete) {
                std::error_code ec;
                if (fs::remove(srcPath, ec)) {
                    completedBytes->fetch_add(fileSize);
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                        operationIsos.emplace("\033[0;1mDeleted: \033[1;92m'" + srcDir + "/" + srcFile + "'\033[0;1m.");
                    }
                } else {
                    {
                        std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                        operationErrors.emplace("\033[1;91mError deleting: \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m: " + ec.message() + ".\033[0;1m");
                    }
                    operationSuccessful = false;
                }
                completedTasks->fetch_add(1);
            } else {
                for (size_t i = 0; i < destDirs.size(); ++i) {
                    const auto& destDir = destDirs[i];
                    fs::path destPath = fs::path(destDir) / srcPath.filename();

                    auto [destDirProcessed, destFile] = extractDirectoryAndFilename(destPath.string(), "cp_mv_rm");

                    // Check if source and destination are the same
                    fs::path absSrcPath = fs::absolute(srcPath);
                    fs::path absDestPath = fs::absolute(destPath);

                    if (absSrcPath == absDestPath) {
                        {
                            std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                            operationErrors.emplace("\033[1;91mCannot " + std::string(isMove ? "move" : "copy") +
                                                   " file to itself: \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m.\033[0m");
                        }
                        operationSuccessful = false;
                        completedTasks->fetch_add(1);
                        continue;
                    }

                    if (!fs::exists(destDir) || !fs::is_directory(destDir)) {
                        {
                            std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                            operationErrors.emplace("\033[1;91mInvalid destination: \033[1;93m'" + destDirProcessed + "'\033[1;91m.\033[0;1m");
                        }
                        operationSuccessful = false;
                        completedTasks->fetch_add(1);
                        continue;
                    }

                    if (fs::exists(destPath)) {
                        if (overwriteExisting) {
                            std::error_code ec;
                            if (!fs::remove(destPath, ec)) {
                                {
                                    std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                                    operationErrors.emplace("\033[1;91mFailed to overwrite: \033[1;93m'" +
                                                           destDirProcessed + "/" + destFile + "'\033[1;91m - " + ec.message() + ".\033[0;1m");
                                }
                                operationSuccessful = false;
                                completedTasks->fetch_add(1);
                                continue;
                            }
                        } else {
                            {
                                std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                                operationErrors.emplace("\033[1;91mFile exists: \033[1;93m'" +
                                                      destDirProcessed + "/" + destFile + "'\033[1;91m (enable overwrites).\033[0;1m");
                            }
                            operationSuccessful = false;
                            completedTasks->fetch_add(1);
                            continue;
                        }
                    }

                    std::error_code ec;
                    bool success = false;

                    if (isCopy) {
                        success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                    } else if (isMove) {
                        // Try rename first (atomic move within same filesystem)
                        fs::rename(srcPath, destPath, ec);
                        
                        if (ec) {
                            // If rename fails (likely different filesystem), fall back to copy+delete
                            ec.clear();
                            success = bufferedCopyWithProgress(srcPath, destPath, completedBytes, ec);
                            
                            if (success && i == destDirs.size() - 1) {
                                // Only delete source after successful copy and on last destination
                                std::error_code deleteEc;
                                if (!fs::remove(srcPath, deleteEc)) {
                                    {
                                        std::lock_guard<std::mutex> lock(globalSetsMutex);
                                        operationErrors.emplace("\033[1;91mMove completed but failed to remove source file: \033[1;93m'" + 
                                            srcDir + "/" + srcFile + "'\033[1;91m - " + deleteEc.message() + "\033[0m");
                                    }
                                }
                            }
                        } else {
                            completedBytes->fetch_add(fileSize);
                            success = true;
                        }
                    }

                    if (!success || ec) {
						std::string errorMessageInfo = "\033[1;91mError " +
                        std::string(isCopy ? "copying" : (i < destDirs.size() - 1 ? "moving" : "moving")) +
									": \033[1;93m'" + srcDir + "/" + srcFile + "'\033[1;91m" +
                                    " to '" + destDirProcessed + "/': " + ec.message() + "\033[1;91m.\033[0;1m";
                        {
							std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                            operationErrors.emplace(errorMessageInfo);
                        }
                        operationSuccessful = false;
                        operationSuccessful = false;
                    } else {
                        if (!changeOwnership(destPath)) {
                            operationSuccessful = false;
                        } else {
                            {
                                std::lock_guard<std::mutex> lock(globalSetsMutex); // Protect the set
                                operationIsos.emplace("\033[0;1m" +
                                                     std::string(isCopy ? "Copied" : "Moved") + ": \033[1;92m'" +
                                                     srcDir + "/" + srcFile + "'\033[1m to \033[1;94m'" +
                                                     destDirProcessed + "/" + destFile + "'\033[0;1m.");
                            }
                        }
                    }

                    completedTasks->fetch_add(1);
                }
            }
        }
    };

    std::vector<std::string> isoFilesToOperate;
    for (const auto& iso : isoFiles) {
		fs::path isoPath(iso);
		auto [isoDir, isoFile] = extractDirectoryAndFilename(isoPath.string(), "cp_mv_rm");

		auto it = std::find(isoFilesCopy.begin(), isoFilesCopy.end(), iso);
		if (it != isoFilesCopy.end()) {
			if (fs::exists(isoPath)) {
				isoFilesToOperate.push_back(iso);
			} else {
				std::lock_guard<std::mutex> lock(globalSetsMutex);
				operationErrors.emplace("\033[1;35mMissing: \033[1;93m'" + isoDir + "/" + isoFile + "'\033[1;35m.\033[0;1m");
				// Increment tasks based on operation type
				if (isDelete) {
					completedTasks->fetch_add(1);
				} else {
					completedTasks->fetch_add(destDirs.size());
				}
			}
		} else {
			std::lock_guard<std::mutex> lock(globalSetsMutex);
			operationErrors.emplace("\033[1;91mNot in cache: \033[1;93m'" + isoDir + "/" + isoFile + "'\033[1;91m.\033[0;1m");
			// Increment tasks based on operation type
			if (isDelete) {
				completedTasks->fetch_add(1);
			} else {
				completedTasks->fetch_add(destDirs.size());
			}
		}
	}

    executeOperation(isoFilesToOperate);
}
