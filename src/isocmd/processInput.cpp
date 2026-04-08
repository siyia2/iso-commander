// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mdf.h"
#include "../ccd.h"
#include "../themes.h"

/**
 * @file operations.cpp
 * @brief High-level processing functions for mounting, file management (Cp/Mv/Rm), and format conversions.
 */

/**
 * @brief Orchestrates the mounting or unmounting of ISO files using a static thread pool and progress tracking.
 * * @param input Raw user input string (indices or "00").
 * @param files List of available files.
 * @param operationFiles Set to populate with successfully processed files.
 * @param skippedMessages Set to track items skipped during the operation.
 * @param operationFails Set to track failed file paths.
 * @param uniqueErrorMessages Set to collect unique error strings.
 * @param operationBreak Boolean flag to control outer loop flow.
 * @param verbose Toggle for detailed progress output.
 * @param isUnmount True for unmount operation, false for mount.
 */
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& operationBreak, bool& verbose, bool isUnmount) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);
    
    std::unordered_set<int> indicesToProcess;
    
    if (input == "00") {
        for (int i = 1; i <= static_cast<int>(files.size()); ++i) {
            indicesToProcess.insert(i);
        }
    } else {
        tokenizeInput(input, files, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            if (isUnmount) operationBreak = false;
            return;
        }
    }
    
    std::vector<std::string> selectedFiles;
    selectedFiles.reserve(indicesToProcess.size());
    for (int index : indicesToProcess) {
        selectedFiles.push_back(files[index - 1]);
    }
    
    std::string operationColor = std::string(isUnmount ? originalColors::yellow : originalColors::green);
    std::string operationName = isUnmount ? "umount" : "mount";
    
    std::cout << originalColors::boldAlt << "\n Processing" 
          << (selectedFiles.size() > 1 ? " tasks" : " task") 
          << " for " << operationColor << operationName 
          << originalColors::boldAlt << "... (" 
          << originalColors::red << "Ctrl+c" 
          << originalColors::boldAlt << ":cancel)\n";
    
    std::string coloredProcess = std::string(operationColor) + operationName + std::string(originalColors::boldAlt);;
    
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isUnmount ? UMOUNT_THREAD_CAP : MOUNT_THREAD_CAP;
    size_t numThreads = std::max(size_t(2), std::min({selectedFiles.size(), cap, poolSize}));

    if ((selectedFiles.size() + numThreads - 1) / numThreads > 100) {
        numThreads = (selectedFiles.size() + 99) / 100;
    }

    std::vector<std::vector<std::string>> chunks(numThreads);
    for (size_t i = 0; i < selectedFiles.size(); ++i) {
        chunks[i % numThreads].push_back(std::move(selectedFiles[i]));
    }
    
    std::vector<std::future<void>> futures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    
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
    
    for (const auto& chunk : chunks) {
        futures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            
            if (isUnmount) {
                unmountISO(chunk, operationFiles, operationFails, &completedTasks, &failedTasks, false);
            } else {
                mountIsoFiles(chunk, operationFiles, skippedMessages, operationFails, &completedTasks, &failedTasks, false);
            }
        }));
    }
    
    for (auto& future : futures) {
        future.wait();
    }
    
    if (completedTasks == 0 && isUnmount) operationBreak = false;
    
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();
}

/**
 * @brief Groups file indices into chunks for parallel processing, preventing race conditions by grouping identical filenames.
 * * @param processedIndices Set of indices selected by the user.
 * @param isoFiles Master list of file paths.
 * @param numThreads Desired concurrency level.
 * @param isDelete Flag indicating if the operation is a deletion (avoids name-collision logic).
 * @return A vector of chunks, where each chunk is a vector of indices.
 */
std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete) 
{
    std::vector<int> processedIndicesVector(processedIndices.begin(), processedIndices.end());
    std::vector<std::vector<int>> indexChunks;

    if (processedIndicesVector.empty()) return indexChunks;

    if (!isDelete) {
        std::unordered_map<std::string, std::vector<int>> groups;
        for (int idx : processedIndicesVector) {
            std::string baseName = std::filesystem::path(isoFiles[idx - 1]).filename().string();
            groups[baseName].push_back(idx);
        }

        std::vector<int> uniqueNameFiles;
        for (auto& [baseName, indices] : groups) {
            if (indices.size() > 1) {
                indexChunks.push_back(std::move(indices));
            } else {
                uniqueNameFiles.push_back(indices[0]);
            }
        }

        if (!uniqueNameFiles.empty()) {
            size_t usedChunks = indexChunks.size();
            size_t remainingThreads = (numThreads > usedChunks) ? numThreads - usedChunks : 1;

            size_t maxFilesPerChunk = std::max<size_t>(1, (uniqueNameFiles.size() + remainingThreads - 1) / remainingThreads);

            for (size_t i = 0; i < uniqueNameFiles.size(); i += maxFilesPerChunk) {
                auto end = std::min(i + maxFilesPerChunk, uniqueNameFiles.size());
                indexChunks.emplace_back(uniqueNameFiles.begin() + i, uniqueNameFiles.begin() + end);
            }
        }
    } else {
        size_t maxFilesPerChunk = std::max<size_t>(1, (processedIndicesVector.size() + numThreads - 1) / numThreads);
        for (size_t i = 0; i < processedIndicesVector.size(); i += maxFilesPerChunk) {
            auto end = std::min(i + maxFilesPerChunk, processedIndicesVector.size());
            indexChunks.emplace_back(processedIndicesVector.begin() + i, processedIndicesVector.begin() + end);
        }
    }

    return indexChunks;
}

/**
 * @brief Sums the physical file sizes of a given list of file paths.
 * * @param files Vector of file paths.
 * @return Total size in bytes.
 */
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

/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 * * @param input Raw user input.
 * @param isoFiles Master list of files.
 * @param process Operation type ("cp", "mv", or "rm").
 * @param operationIsos Set to track successfully modified items.
 * @param operationErrors Set to track failed items.
 * @param uniqueErrorMessages Set for unique UI error reporting.
 * @param umountMvRmBreak Flag for outer loop control.
 * @param filterHistory Flag indicating if the view is filtered.
 * @param verbose Detailed output toggle.
 * @param newISOFound Atomic flag for filesystem changes.
 */
void processInputForCpMvRm(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound) {
    setupSignalHandlerCancellations();
    
    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");
    
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = std::string(isDelete ? originalColors::red : (isCopy ? originalColors::green : originalColors::yellow));

    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }
    
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isDelete ? RM_THREAD_CAP : CPMV_THREAD_CAP;
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), cap, poolSize}));
    
    std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirCpMv(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
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
                 
    if (isCopy || isMove) {
        size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;
    }
    
    std::cout << "\n" << originalColors::boldAlt << " Processing " 
          << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process 
          << originalColors::boldAlt << "... (" 
          << originalColors::red << "Ctrl+c" 
          << originalColors::boldAlt << ":cancel)\n";
             
    std::string coloredProcess = 
    isDelete ? std::string(originalColors::red)    + process + std::string(originalColors::boldAlt) :
    isMove   ? std::string(originalColors::yellow) + process + std::string(originalColors::boldAlt) :
    isCopy   ? std::string(originalColors::green)  + process + std::string(originalColors::boldAlt) :
    process;
    
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
                                 totalBytes, &completedTasks, &failedTasks, 
                                 totalTasks, &isProcessingComplete, &verbose, std::string(coloredProcess));

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

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
    
    if (completedTasks == 0) umountMvRmBreak = false;
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  
    progressThread.join();
    
    if (!isDelete) {
        bool promptFlag = false;
        int maxDepth = 0;
        refreshForDatabase(userDestDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }

    clear_history();
}

/**
 * @brief Predicts the output size of converted files based on the input format.
 * * @param filesToProcess Vector of image file paths.
 * @param modeNrg True if input is Nero NRG.
 * @param modeMdf True if input is Alcohol 120% MDF.
 * @return Total expected size in bytes.
 */
size_t calculateSizeForConverted(const std::vector<std::string>& filesToProcess, bool modeNrg, bool modeMdf) {
    size_t totalBytes = 0;

    if (modeNrg) {
        for (const auto& file : filesToProcess) {
            std::ifstream nrgFile(file, std::ios::binary);
            if (nrgFile) {
                nrgFile.seekg(0, std::ios::end);
                size_t nrgFileSize = nrgFile.tellg();
                totalBytes += (nrgFileSize - 307200);
            }
        }
    } else if (modeMdf) {
        for (const auto& file : filesToProcess) {
            std::ifstream mdfFile(file, std::ios::binary);
            if (mdfFile) {
                MdfTypeInfo mdfInfo;
                if (!mdfInfo.determineMdfType(mdfFile)) continue;
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

/**
 * @brief Processes user selection for converting multi-track images (BIN/MDF/NRG) to standard ISO.
 * * @param input Raw user selection string.
 * @param fileList Master list of non-ISO images.
 * @param modeMdf MDF mode toggle.
 * @param modeNrg NRG mode toggle.
 * @param processedErrors Set for error reporting.
 * @param successOuts Set for successful conversions.
 * @param skippedOuts Set for skipped items.
 * @param failedOuts Set for failed items.
 * @param verbose Verbosity toggle.
 * @param needsClrScrn Flag to trigger screen refresh.
 * @param newISOFound Atomic flag to signal new ISO availability.
 */
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList, 
                               const bool& modeMdf, const bool& modeNrg, 
                               std::unordered_set<std::string>& processedErrors, 
                               std::unordered_set<std::string>& successOuts, 
                               std::unordered_set<std::string>& skippedOuts, 
                               std::unordered_set<std::string>& failedOuts, 
                               bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound) {
    
    setupSignalHandlerCancellations();
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    g_operationCancelled.store(false);
    std::unordered_set<int> processedIndices;
    
    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))){
        tokenizeInput(input, fileList, processedErrors, processedIndices);
    } else return;
    
    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "No valid input provided." << originalColors::boldAlt << "\n";
        std::cout << color << "\n↵ to continue..." << reset;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        needsClrScrn = true;
        return;
    }

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), CONV_THREAD_CAP, poolSize}));
    
    std::vector<std::vector<size_t>> indexChunks;
    const size_t totalFiles = processedIndices.size();
    const size_t filesPerChunk = (totalFiles + numThreads - 1) / numThreads;

    auto it = processedIndices.begin();
    for (size_t i = 0; i < totalFiles; i += filesPerChunk) {
        auto chunkEnd = std::next(it, std::min(filesPerChunk, static_cast<size_t>(std::distance(it, processedIndices.end()))));
        indexChunks.emplace_back(it, chunkEnd);
        it = chunkEnd;
    }
    
    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(fileList[index - 1]);
    }

    size_t totalTasks = filesToProcess.size();
    size_t totalBytes = calculateSizeForConverted(filesToProcess, modeNrg, modeMdf);

	// Define the suffix once
	std::string suffix = (totalTasks > 1 ? " conversions" : " conversion");

	// Explicitly wrap the string_view members in std::string() to allow concatenation
	std::string operation = modeMdf ? (std::string(originalColors::orange) + "MDF"     + std::string(originalColors::boldAlt) + suffix) :
							modeNrg ? (std::string(originalColors::orange) + "NRG"     + std::string(originalColors::boldAlt) + suffix) :
									  (std::string(originalColors::orange) + "BIN/IMG" + std::string(originalColors::boldAlt) + suffix);

	clearScrollBuffer();

	// For std::cout, you don't need the string cast because it handles string_view natively
	std::cout << "\n" << originalColors::boldAlt << " Processing " 
			  << operation << originalColors::boldAlt << "... (" 
			  << originalColors::red << "Ctrl+c" 
			  << originalColors::boldAlt << ":cancel)\n";
			  
    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
        totalBytes, &completedTasks, &failedTasks, totalTasks, &isProcessingComplete, &verbose, std::string(operation));

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> imageFilesInChunk;
        imageFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(imageFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; });

        futures.emplace_back(pool.enqueue([imageFilesInChunk = std::move(imageFilesInChunk), 
            &fileList, &successOuts, &skippedOuts, &failedOuts, 
            modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, &newISOFound]() {
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts, 
                modeMdf, modeNrg, &completedBytes, &completedTasks, &failedTasks, newISOFound);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  
    progressThread.join();
}


void processInputCHD(const std::string& input, std::vector<std::string>& fileList,
                     std::unordered_set<std::string>& processedErrors,
                     std::unordered_set<std::string>& successOuts,
                     std::unordered_set<std::string>& skippedOuts,
                     std::unordered_set<std::string>& failedOuts,
                     bool& verbose, bool& needsClrScrn, std::atomic<bool>& newCHDFound) {

    setupSignalHandlerCancellations();
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    g_operationCancelled.store(false);
    std::unordered_set<int> processedIndices;

    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))) {
        tokenizeInput(input, fileList, processedErrors, processedIndices);
    } else return;

    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n" << (isOrig ? originalColors::red : theme->secondary)
                  << "No valid input provided." << originalColors::boldAlt << "\n";
        std::cout << color << "\n↵ to continue..." << reset;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        needsClrScrn = true;
        return;
    }

    ThreadPool& pool           = getStaticThreadPool();
    const size_t poolSize      = pool.threadCount();
    const size_t numThreads    = std::max(size_t(2), std::min({processedIndices.size(), CONV_THREAD_CAP, poolSize}));

    std::vector<std::vector<size_t>> indexChunks;
    const size_t totalFiles    = processedIndices.size();
    const size_t filesPerChunk = (totalFiles + numThreads - 1) / numThreads;

    auto it = processedIndices.begin();
    for (size_t i = 0; i < totalFiles; i += filesPerChunk) {
        auto chunkEnd = std::next(it, std::min(filesPerChunk, static_cast<size_t>(std::distance(it, processedIndices.end()))));
        indexChunks.emplace_back(it, chunkEnd);
        it = chunkEnd;
    }

    std::vector<std::string> filesToProcess;
    filesToProcess.reserve(totalFiles);
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(fileList[index - 1]);
    }

    const size_t totalTasks = filesToProcess.size();

    const std::string suffix    = (totalTasks > 1 ? " conversions" : " conversion");
    const std::string operation = std::string(originalColors::orange) + "ISO"
                                  + std::string(originalColors::boldAlt) + suffix;

    clearScrollBuffer();
    std::cout << "\n" << originalColors::boldAlt << " Processing "
              << operation << originalColors::boldAlt << "... ("
              << originalColors::red << "Ctrl+c"
              << originalColors::boldAlt << ":cancel)\n";

    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool>   isProcessingComplete(false);

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(isoFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; });

        futures.emplace_back(pool.enqueue(
            [isoFilesInChunk = std::move(isoFilesInChunk),
             &successOuts, &skippedOuts, &failedOuts,
             &completedTasks, &failedTasks, &newCHDFound]() {
                convertToCHD(isoFilesInChunk, successOuts, skippedOuts, failedOuts,
                             &completedTasks, &failedTasks, newCHDFound);
            }
        ));
    }

    for (auto& future : futures) {
        future.wait();
    }
    
    // Print final summary after all conversions
std::cout << "\n\033[K"  // Clear any leftover progress line
          << "Tasks: " << completedTasks.load() << "/" << totalTasks
          << " completed, " << failedTasks.load() << " failed\n";

std::cout << color << "↵ to continue..." << reset;
std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    std::cout << color << "↵ to continue..." << reset;

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
}

