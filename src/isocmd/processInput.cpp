// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mdf.h"
#include "../ccd.h"
#include "../themes.h"
#include "../daa2iso.h"
#include "../chd.h"
#include <chd.h>

/**
 * @file operations.cpp
 * @brief High-level processing functions for mounting, file management (Cp/Mv/Rm), and format conversions.
 */

/**
 * @brief Orchestrates the mounting or unmounting of ISO files using a static thread pool.
 * * High-performance orchestration that minimizes memory overhead by capturing the master 
 * file list by reference while distributing work via index-based chunks.
 * * @section threading_logic Threading Logic
 * - Uses a mixed-capture lambda: large containers are referenced, while task-specific 
 * indices and operation flags are captured by value for thread-local consistency.
 * - Employs std::future::wait to ensure all worker threads complete before the master 
 * list's scope ends, preventing use-after-free conditions.
 * * @param input Raw user input string (indices or "00").
 * @param files Master list of available files (captured by reference in workers).
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
    std::vector<int> selectedIndices;

    if (input == "00") {
        for (int i = 1; i <= static_cast<int>(files.size()); ++i)
            indicesToProcess.insert(i);
    } else {
        tokenizeInput(input, files, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            if (isUnmount) operationBreak = false;
            return;
        }
    }

    selectedIndices.reserve(indicesToProcess.size());
    for (int index : indicesToProcess)
        selectedIndices.push_back(index);

    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    std::string colorMuted = isOrig ? std::string(UI::Palette::BoldReset) : std::string(theme->muted);
    std::string operationColor = std::string(isUnmount ? UI::Palette::Yellow : UI::Palette::Green);
    std::string operationName = isUnmount ? "umount" : "mount";

    std::cout << colorMuted << "\n Processing"
              << (selectedIndices.size() > 1 ? " tasks" : " task")
              << " for " << operationColor << operationName
              << colorMuted << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << colorMuted << ":cancel)\n";

    std::string coloredProcess = std::string(operationColor) + operationName + std::string(UI::Palette::BoldReset);

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isUnmount ? UMOUNT_THREAD_CAP : MOUNT_THREAD_CAP;
    size_t numThreads = std::max(size_t(2), std::min({selectedIndices.size(), cap, poolSize}));

    if ((selectedIndices.size() + numThreads - 1) / numThreads > 100)
        numThreads = (selectedIndices.size() + 99) / 100;

    std::vector<std::vector<int>> indexChunks(numThreads);
    for (size_t i = 0; i < selectedIndices.size(); ++i)
        indexChunks[i % numThreads].push_back(selectedIndices[i]);

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
        selectedIndices.size(),
        &isProcessingComplete,
        &verbose,
        std::string(coloredProcess)
    );

    for (const auto& idxChunk : indexChunks) {
		futures.emplace_back(pool.enqueue([&files, &operationFiles, &operationFails, 
										   &skippedMessages, &completedTasks, &failedTasks, 
										   idxChunk, isUnmount]() { 
			if (g_operationCancelled.load()) return;

			std::vector<std::string> chunkStr;
			chunkStr.reserve(idxChunk.size());
			
			for (int idx : idxChunk) {
				if (idx > 0 && idx <= static_cast<int>(files.size())) {
					chunkStr.push_back(files[idx - 1]); 
				}
			}

			if (isUnmount)
				// Add '&' before completedTasks and failedTasks
				unmountISO(chunkStr, operationFiles, operationFails, &completedTasks, &failedTasks, false);
			else
				// Add '&' before completedTasks and failedTasks
				mountIsoFiles(chunkStr, operationFiles, skippedMessages, operationFails, &completedTasks, &failedTasks, false);
		}));
	}

    for (auto& future : futures)
        future.wait();

    if (completedTasks == 0 && isUnmount) operationBreak = false;

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();
}

/**
 * @brief Groups file indices into chunks for parallel processing.
 * * Optimizes concurrency by balancing workload while preventing filesystem race conditions.
 * For non-delete operations, files with the same base name (e.g., multi-part images) 
 * are grouped into the same chunk to ensure sequential processing by a single thread.
 * * @param processedIndices Set of indices selected by the user.
 * @param isoFiles Master list of file paths used for name-collision logic.
 * @param numThreads Desired concurrency level.
 * @param isDelete Flag indicating if the operation is a deletion (bypasses name grouping).
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
 * @brief Handles bulk copy, move, or remove operations with thread safety.
 *
 * Orchestrates filesystem lifecycle events on selected image files. This version
 * utilizes an index-based chunking strategy to remain memory-efficient even when 
 * processing thousands of files.
 *
 * @section memory_safety Memory Safety
 * - **Zero-Copy Setup**: Avoids duplicating the master ISO list by using reference 
 * captures combined with future synchronization.
 * - **Thread Isolation**: Each worker constructs its own local string vector from 
 * provided indices, ensuring thread-local data ownership during I/O.
 *
 * @param input Raw user input (e.g., "1-3, 5").
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

    std::vector<std::string> successfulDestPaths;
    std::mutex destPathsMutex;

    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");

    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = std::string(isDelete ? UI::Palette::Red : (isCopy ? UI::Palette::Green : UI::Palette::Yellow));

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

    size_t totalBytes = 0;
    {
        std::vector<std::string> filesToProcess;
        filesToProcess.reserve(processedIndices.size());
        for (const auto& chunk : indexChunks)
            for (int idx : chunk)
                filesToProcess.push_back(isoFiles[idx - 1]);
        totalBytes = getTotalFileSize(filesToProcess);
    }

    size_t totalTasks = processedIndices.size();

    if (isCopy || isMove) {
        size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;
    }

    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    std::string colorMuted = isOrig ? std::string(UI::Palette::BoldReset) : std::string(theme->muted);

    std::cout << "\n" << colorMuted << " Processing "
              << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process
              << colorMuted << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << colorMuted << ":cancel)\n";

    std::string coloredProcess =
        isDelete ? std::string(UI::Palette::Red)    + process + std::string(UI::Palette::BoldReset) :
        isMove   ? std::string(UI::Palette::Yellow) + process + std::string(UI::Palette::BoldReset) :
        isCopy   ? std::string(UI::Palette::Green)  + process + std::string(UI::Palette::BoldReset) :
        process;

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes,
                               totalBytes, &completedTasks, &failedTasks,
                               totalTasks, &isProcessingComplete, &verbose, std::string(coloredProcess));

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        futures.emplace_back(pool.enqueue([chunk, &isoFiles, &operationIsos, &operationErrors,
                                       userDestDir, isMove, isCopy, isDelete, // Captured by value
                                       &completedBytes, &completedTasks, &failedTasks,
										   overwriteExisting, &successfulDestPaths, &destPathsMutex]() {
			
			if (g_operationCancelled.load(std::memory_order_relaxed)) return;

			std::vector<std::string> isoFilesInChunk;
			isoFilesInChunk.reserve(chunk.size());
			for (int idx : chunk) {
				if (idx > 0 && idx <= static_cast<int>(isoFiles.size())) {
					isoFilesInChunk.push_back(isoFiles[idx - 1]);
				}
			}

			handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors,
								   userDestDir, isMove, isCopy, isDelete,
								   &completedBytes, &completedTasks, &failedTasks,
								   overwriteExisting, &successfulDestPaths, &destPathsMutex);
		}));
    }

    for (auto& future : futures)
        future.wait();

    if (completedTasks == 0) umountMvRmBreak = false;
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();

    if (completedTasks.load() > 0 && !isDelete) {
        std::string exactPaths;
        for (const auto& destPath : successfulDestPaths) {
            if (!exactPaths.empty()) exactPaths += ';';
            exactPaths += destPath;
        }
        if (!exactPaths.empty())
            updateDatabaseAfterOperations(exactPaths, newISOFound);
    }
    clear_history();
}

/**
 * @brief Processes user input to convert disk images to ISO with real-time tracking.
 *
 * Performs multi-threaded conversion of supported image types (CCD, MDF, NRG, CHD, DAA).
 * Includes an inline size estimation pass for an accurate byte-level progress bar.
 *
 * @section threading_concurrency Threading & Concurrency
 * - **Mixed Capture Strategy**: Captures primitive flags (modeMdf, etc.) by value 
 * to ensure immutable state within workers, while capturing output sets by reference 
 * for shared result aggregation.
 * - **Database Integration**: Triggers a targeted database update post-conversion 
 * using exact output paths, ensuring new ISOs are indexed without a full scan.
 *
 * @param input Raw user selection string.
 * @param fileList Master vector of non-ISO image paths.
 * @param modeMdf Flag for Alcohol 120% MDF images.
 * @param modeNrg Flag for Nero NRG images.
 * @param modeChd Flag for MAME CHD images.
 * @param modeDaa Flag for PowerISO DAA/GBI images.
 * @param processedErrors Set to record parsing errors.
 * @param successOuts Result messages for successes.
 * @param skippedOuts Result messages for skips.
 * @param failedOuts Result messages for failures.
 * @param verbose Extra progress details.
 * @param needsClrScrn Flag to request terminal clear.
 * @param newISOFound Atomic flag for filesystem changes.
 */
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList,
                               const bool& modeMdf, const bool& modeNrg, const bool& modeChd,
                               const bool& modeDaa,
                               std::unordered_set<std::string>& processedErrors,
                               std::unordered_set<std::string>& successOuts,
                               std::unordered_set<std::string>& skippedOuts,
                               std::unordered_set<std::string>& failedOuts,
                               bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound)
{
    std::vector<std::string> successfulOutputPaths;
    std::mutex outPathsMutex;

    setupSignalHandlerCancellations();

    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    g_operationCancelled.store(false);
    std::unordered_set<int> processedIndices;

    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))) {
        tokenizeInput(input, fileList, processedErrors, processedIndices);
    } else return;

    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n" << (isOrig ? UI::Palette::Red : theme->secondary)
                  << "No valid input provided." << UI::Palette::BoldReset << "\n";
        pressEnterToContinue();
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

    // build only for size calculation, then immediately release
    size_t totalBytes = 0;
    {
        std::vector<std::string> filesToProcess;
        filesToProcess.reserve(processedIndices.size());
        for (const auto& chunk : indexChunks)
            for (size_t idx : chunk)
                filesToProcess.push_back(fileList[idx - 1]);
        totalBytes = calculateTotalBytesForConversions(filesToProcess, modeMdf, modeNrg, modeChd, modeDaa);
    }

    std::string colorMuted = isOrig ? std::string(UI::Palette::BoldReset) : std::string(theme->muted);

    size_t totalTasks = processedIndices.size();
    std::string suffix = (totalTasks > 1 ? " conversions" : " conversion");

    std::string operation;
    if (modeMdf)      operation = std::string(UI::Palette::Orange) + "mdf2iso" + std::string(colorMuted) + suffix;
    else if (modeNrg) operation = std::string(UI::Palette::Orange) + "nrg2iso" + std::string(colorMuted) + suffix;
    else if (modeChd) operation = std::string(UI::Palette::Orange) + "chd2iso" + std::string(colorMuted) + suffix;
    else if (modeDaa) operation = std::string(UI::Palette::Orange) + "daa2iso" + std::string(colorMuted) + suffix;
    else              operation = std::string(UI::Palette::Orange) + "ccd2iso" + std::string(colorMuted) + suffix;

    clearScrollBuffer();

    std::cout << "\n" << colorMuted << " Processing "
              << operation << colorMuted << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << colorMuted << ":cancel)\n";

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes,
        totalBytes, &completedTasks, &failedTasks, totalTasks, &isProcessingComplete, &verbose, operation);

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        futures.emplace_back(pool.enqueue([chunk, &fileList, 
                                       &successOuts, &skippedOuts, &failedOuts,
                                       modeMdf, modeNrg, modeChd, modeDaa, // Captured by VALUE
                                       &completedBytes, &completedTasks, &failedTasks,
										   &successfulOutputPaths, &outPathsMutex]() {
			
			// Cancellation check
			if (g_operationCancelled.load(std::memory_order_relaxed)) return;

			std::vector<std::string> imageFilesInChunk;
			imageFilesInChunk.reserve(chunk.size());
			for (size_t idx : chunk) {
				// Safe index check
				if (idx > 0 && idx <= fileList.size()) {
					imageFilesInChunk.push_back(fileList[idx - 1]);
				}
			}

			// Perform conversion
			convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts,
						 modeMdf, modeNrg, modeChd, modeDaa,
						 &completedBytes, &completedTasks, &failedTasks,
						 &successfulOutputPaths, &outPathsMutex);
		}));
    }

    for (auto& future : futures)
        future.wait();

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();

    if (!successfulOutputPaths.empty()) {
        std::string exactPaths;
        for (const auto& outPath : successfulOutputPaths) {
            if (!exactPaths.empty()) exactPaths += ';';
            exactPaths += outPath;
        }
        updateDatabaseAfterOperations(exactPaths, newISOFound);
    }
}
