// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../threadpool.h"
#include "../mdf.h"
#include "../ccd.h"


// Function to process mount/unmount indices
void processIsoOperations(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& operationBreak, bool& verbose, bool isUnmount) {
    // Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
    
    g_operationCancelled.store(false);
    
    std::unordered_set<int> indicesToProcess;
    
    // Handle input ("00" = all files, else parse input)
    if (input == "00") {
        for (size_t i = 0; i < files.size(); ++i)
            indicesToProcess.insert(i + 1);
    } else {
        tokenizeInput(input, files, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            if (isUnmount) {
                operationBreak = false;
            }
            return;
        }
    }
    
    // Create selected files vector from indices
    std::vector<std::string> selectedFiles;
    selectedFiles.reserve(indicesToProcess.size());
    for (int index : indicesToProcess) {
        selectedFiles.push_back(files[index - 1]);
    }
    
    // Determine operation color and name based on isUnmount flag
    std::string operationColor = isUnmount ? "\033[1;93m" : "\033[1;92m";
    std::string operationName = isUnmount ? "umount" : "mount";
    
    std::cout << "\n\033[0;1m Processing" << (selectedFiles.size() > 1 ? " tasks" : " task") 
              << " for " << operationColor << operationName << "\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";
    
    std::string coloredProcess = std::string(operationColor + operationName + "\033[0;1m") 
                                 + (selectedFiles.size() > 1 ? " tasks" : " task");
    
    // Thread pool setup
    unsigned int numThreads = std::min(static_cast<unsigned int>(selectedFiles.size()), maxThreads);
    const size_t chunkSize = std::min(size_t(100), selectedFiles.size()/numThreads + 1);
    std::vector<std::vector<std::string>> chunks;
    
    // Split work into chunks
    for (size_t i = 0; i < selectedFiles.size(); i += chunkSize) {
        auto end = std::min(selectedFiles.begin() + i + chunkSize, selectedFiles.end());
        chunks.emplace_back(selectedFiles.begin() + i, end);
    }
    
    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    
    // Start progress thread
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
        &failedTasks,
        selectedFiles.size(),
        &isProcessingComplete,
        &verbose,
        std::string(coloredProcess)
    );
    
    // Enqueue chunk tasks
    for (const auto& chunk : chunks) {
        futures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            
            if (isUnmount) {
                unmountISO(chunk, operationFiles, operationFails, &completedTasks, &failedTasks);
            } else {
                mountIsoFiles(chunk, operationFiles, skippedMessages, operationFails, &completedTasks, &failedTasks);
            }
        }));
    }
    
    // Wait for completion or cancellation
    for (auto& future : futures) {
        future.wait();
    }
    
    // Cleanup
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
}


// Function to group files for CpMvRm, identical filenames are grouped in the same chunk and processed by the same thread
std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete) {
    // Convert unordered_set to vector
    std::vector<int> processedIndicesVector(processedIndices.begin(), processedIndices.end());

    std::vector<std::vector<int>> indexChunks;

    if (!isDelete) {
        // Group indices by their identical filename into a single chunk for proccessing them in the same thread to avoid collisions when doing cp/mv with -o
        std::unordered_map<std::string, std::vector<int>> groups;
        for (int idx : processedIndicesVector) {
            std::string baseName = std::filesystem::path(isoFiles[idx - 1]).filename().string();
            groups[baseName].push_back(idx);
        }

        std::vector<int> uniqueNameFiles;
        // Separate multi-file groups and collect unique files
        for (auto& kv : groups) {
            if (kv.second.size() > 1) {
                indexChunks.push_back(kv.second);
            } else {
                uniqueNameFiles.push_back(kv.second[0]);
            }
        }

        // Calculate max files per chunk based on numThreads
        size_t maxFilesPerChunk = std::max(1UL, numThreads > 0 ? (uniqueNameFiles.size() + numThreads - 1) / numThreads : 5);

        // Split unique files into chunks
        for (size_t i = 0; i < uniqueNameFiles.size(); i += maxFilesPerChunk) {
            auto end = std::min(i + maxFilesPerChunk, uniqueNameFiles.size());
            std::vector<int> chunk(
                uniqueNameFiles.begin() + i,
                uniqueNameFiles.begin() + end
            );
            indexChunks.emplace_back(chunk);
        }
    } else {
        // For "rm", group indices into chunks based on numThreads
        size_t maxFilesPerChunk = std::max(1UL, numThreads > 0 ? (processedIndicesVector.size() + numThreads - 1) / numThreads : 10);

        for (size_t i = 0; i < processedIndicesVector.size(); i += maxFilesPerChunk) {
            std::vector<int> chunk;
            auto end = std::min(i + maxFilesPerChunk, processedIndicesVector.size());
            for (size_t j = i; j < end; ++j) {
                chunk.push_back(processedIndicesVector[j]);
            }
            indexChunks.push_back(chunk);
        }
    }

    return indexChunks;
}


// Function to get the total size of files
size_t getTotalFileSize(const std::vector<std::string>& files) {
    size_t totalSize = 0;
    for (const auto& file : files) {
        struct stat st;
        if (stat(file.c_str(), &st) == 0) {
            totalSize += st.st_size;
        }
    }
    return totalSize;
}


// Function to process selected indices for cpMvDel accordingly
void processOperationInput(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound) {
    setupSignalHandlerCancellations();
    
    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");
    
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor       = isDelete ? "\033[1;91m" : (isCopy ? "\033[1;92m" : "\033[1;93m");

    // Parse the input to fill processedIndices.
    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }
    
	unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
	std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

	// Flag for Rm abortion
    bool abortDel = false;
    
    std::string processedUserDestDir = userDestDirRm(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
                                                     operationColor, operationDescription, umountMvRmBreak, 
                                                     filterHistory, isDelete, isCopy, abortDel, overwriteExisting);
        
    g_operationCancelled.store(false);
    
    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
        uniqueErrorMessages.clear();
        return;
    }
    uniqueErrorMessages.clear();
    clearScrollBuffer();

    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(isoFiles[index - 1]);
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    size_t totalBytes = getTotalFileSize(filesToProcess);
    size_t totalTasks = filesToProcess.size();
                 
    // Adjust totals for copy/move operations with multiple destinations
    if (isCopy || isMove) {
		size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;
    }
    
    std::cout << "\n\033[0;1m Processing " << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process <<
             "\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";
             
             std::string coloredProcess = 
    isDelete ? std::string("\033[1;91m") + process + "\033[0;1m" + (totalTasks > 1 ? " tasks" : " task") :
    isMove   ? std::string("\033[1;93m") + process + "\033[0;1m" + (totalTasks > 1 ? " tasks" : " task") :
    isCopy   ? std::string("\033[1;92m") + process + "\033[0;1m" + (totalTasks > 1 ? " tasks" : " task") :
    process + (totalTasks > 1 ? " tasks" : " task");
    
    std::atomic<bool> isProcessingComplete(false);

    // Start progress tracking in a separate thread.
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
                                 totalBytes, &completedTasks, &failedTasks, 
                                 totalTasks, &isProcessingComplete, &verbose, std::string(coloredProcess));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    // For each chunk, create a vector of file names and enqueue the operation.
    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(isoFilesInChunk),
            [&isoFiles](size_t index) { return isoFiles[index - 1]; });

        futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), 
                                             &isoFiles, &operationIsos, &operationErrors, &userDestDir, 
                                             isMove, isCopy, isDelete, &completedBytes, &completedTasks, 
                                             &failedTasks, &overwriteExisting]() {
            handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, 
                                   userDestDir, isMove, isCopy, isDelete, 
                                   &completedBytes, &completedTasks, &failedTasks, overwriteExisting);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
	
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
    

    if (!isDelete) {
		bool promptFlag = false;
        int maxDepth = 0;
        manualRefreshForDatabase(userDestDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }
    
    if (!isDelete && !operationIsos.empty()) {
        saveHistory(filterHistory);
        clear_history();
    }

    clear_history();
}


// Function to calculate converted files size
size_t calculateSizeForConverted(const std::vector<std::string>& filesToProcess, bool modeNrg, bool modeMdf) {
    size_t totalBytes = 0;

    if (modeNrg) {
        for (const auto& file : filesToProcess) {
            std::ifstream nrgFile(file, std::ios::binary);
            if (nrgFile) {
                // Seek to the end of the file to get the total size
                nrgFile.seekg(0, std::ios::end);
                size_t nrgFileSize = nrgFile.tellg();

                // The ISO data starts after the 307,200-byte header
                size_t isoDataSize = nrgFileSize - 307200;

                // Add the ISO data size to the total bytes
                totalBytes += isoDataSize;
            }
        }
    } else if (modeMdf) {
        for (const auto& file : filesToProcess) {
            std::ifstream mdfFile(file, std::ios::binary);
            if (mdfFile) {
                MdfTypeInfo mdfInfo;
                if (!mdfInfo.determineMdfType(mdfFile)) {
                    continue;
                }
                mdfFile.seekg(0, std::ios::end);
                size_t fileSize = mdfFile.tellg();
                size_t numSectors = fileSize / mdfInfo.sector_size;
                totalBytes += numSectors * mdfInfo.sector_data;
            }
        }
    } else {
        for (const auto& file : filesToProcess) {
            std::ifstream ccdFile(file, std::ios::binary | std::ios::ate);
            if (ccdFile) {
                size_t fileSize = ccdFile.tellg();
                totalBytes += (fileSize / sizeof(CcdSector)) * DATA_SIZE;
            }
        }
    }

    return totalBytes;
}

// Function to process user input and convert selected BIN/MDF/NRG files to ISO format
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound) {
	// Setup signal handler at the start of the operation
    setupSignalHandlerCancellations();
    
    g_operationCancelled.store(false);
    
    std::unordered_set<std::string> selectedFilePaths;
    std::string concatenatedFilePaths;

    std::unordered_set<int> processedIndices;
    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))){
		tokenizeInput(input, fileList, processedErrors, processedIndices);
	} else {
		return;
	}
    
    if (processedIndices.empty()) {
		clearScrollBuffer();
		std::cout << "\n\033[1;91mNo valid input provided.\033[1;91m\n";
		std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		needsClrScrn = true;
        return;
    }

    // Ensure safe unsigned conversion of number of threads
    unsigned int numThreads = std::min(static_cast<unsigned int>(processedIndices.size()), maxThreads);
    std::vector<std::vector<size_t>> indexChunks;
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
    
    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(fileList[index - 1]);
    }

    // Calculate total bytes and tasks
    size_t totalTasks = filesToProcess.size();  // Each file is a task
    
    size_t totalBytes = calculateSizeForConverted(filesToProcess, modeNrg, modeMdf);

	std::string operation = modeMdf ? (std::string("\033[1;38;5;208mMDF\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion")) :
                       modeNrg ? (std::string("\033[1;38;5;208mNRG\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion")) :
                                 (std::string("\033[1;38;5;208mBIN/IMG\033[0;1m") + (totalTasks > 1 ? " conversions" : " conversion"));
                     
	clearScrollBuffer();
    std::cout << "\n\033[0;1m Processing \001\033[1;38;5;208m\002" << operation << "\033[0;1m... (\033[1;91mCtrl+c\033[0;1m:cancel)\n";

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    // Use the enhanced progress bar with task tracking
    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
        totalBytes, &completedTasks, &failedTasks, totalTasks, &isProcessingComplete, &verbose, std::string(operation));

    ThreadPool pool(numThreads);
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> imageFilesInChunk;
        imageFilesInChunk.reserve(chunk.size());
        std::transform(
            chunk.begin(),
            chunk.end(),
            std::back_inserter(imageFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; }
        );

        futures.emplace_back(pool.enqueue([imageFilesInChunk = std::move(imageFilesInChunk), 
            &fileList, &successOuts, &skippedOuts, &failedOuts, 
            modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, &newISOFound]() {
            // Process each file with task tracking
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts, 
                modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, newISOFound);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion of futures
    progressThread.join();
}
