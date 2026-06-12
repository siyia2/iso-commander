// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <ctype.h>
#include <sys/stat.h>

// Project Headers
#include "../concurrency.h"
#include "../inputHandling.h"
#include "../mount.h"
#include "../pausePrompt.h"
#include "../process.h"
#include "../state.h"
#include "../verbose.h"
#include "../threadpool.h"
#include "../tokenize.h"
#include "../umount.h"

/**
 * @file operations.cpp
 * @brief High-level processing functions for mounting, file management (Cp/Mv/Rm), and format conversions.
 */

/**
 * @brief Orchestrates the mounting or unmounting of ISO files using a static thread pool and progress tracking.
 *
 * @param input Raw user input string (comma/space separated indices or "00" for all).
 * @param files The master list of available ISO file paths.
 * @param umountMvRmBreak [out] Boolean flag updated to control the caller's execution loop (e.g., on empty input or failed unmount).
 * @param verbose Toggle for detailed per-file progress output.
 * @param isUnmount Set to true for unmounting, false for mounting.
 */
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, bool& umountMvRmBreak, bool& verbose, bool isUnmount) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    std::unordered_set<int> indicesToProcess;
    std::vector<int> selectedIndices;

    if (input == "00") {
        for (int i = 1; i <= static_cast<int>(files.size()); ++i)
            indicesToProcess.insert(i);
    } else {
        tokenizeInput(input, files, indicesToProcess);
        if (indicesToProcess.empty()) {
            if (isUnmount) umountMvRmBreak = false;
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

    std::cout << color << "\n Processing"
              << (selectedIndices.size() > 1 ? " tasks" : " task")
              << " for " << operationColor << operationName
              << color << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << color << ":cancel)\n";

    std::string coloredProcess = std::string(operationColor) + operationName + std::string(UI::Palette::BoldReset);

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isUnmount ? GlobalConcurrency::UMOUNT_THREAD_CAP : GlobalConcurrency::MOUNT_THREAD_CAP;
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
		futures.emplace_back(pool.enqueue([&]() {
			std::vector<std::string> chunkStr;
			chunkStr.reserve(idxChunk.size());
			for (int idx : idxChunk)
				chunkStr.push_back(files[idx - 1]);
			if (isUnmount)
				unmountISO(chunkStr, &completedTasks, &failedTasks, false);
			else
				mountIsoFiles(chunkStr, &completedTasks, &failedTasks, false);
		}));
	}

    for (auto& future : futures)
        future.wait();

    if (completedTasks == 0 && isUnmount) umountMvRmBreak = false;

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

            std::vector<std::vector<int>> uniqueChunks(remainingThreads);
            for (size_t i = 0; i < uniqueNameFiles.size(); ++i)
                uniqueChunks[i % remainingThreads].push_back(uniqueNameFiles[i]);

            for (auto& c : uniqueChunks)
                if (!c.empty()) indexChunks.push_back(std::move(c));
        }
    } else {
        std::vector<std::vector<int>> deleteChunks(numThreads);
        for (size_t i = 0; i < processedIndicesVector.size(); ++i)
            deleteChunks[i % numThreads].push_back(processedIndicesVector[i]);

        for (auto& c : deleteChunks)
            if (!c.empty()) indexChunks.push_back(std::move(c));
    }

    return indexChunks;
}

/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 *
 * This function orchestrates the lifecycle of filesystem operations (cp, mv, rm) on selected
 * image files. It handles everything from user confirmation and destination selection to
 * multi-threaded execution and database synchronization.
 *
 * @section Workflow Lifecycle
 * 1. **Input Parsing**: Tokenizes the user string to map selections to the master ISO list.
 * 2. **Pre-Processing & Validation**:
 * - Determines thread caps based on operation type (e.g., `RM_THREAD_CAP` vs `CPMV_THREAD_CAP`).
 * - Invokes `userDestDirCpMv` to handle UI interactions, such as selecting destination
 * folders or confirming permanent deletion.
 * 3. **Progress Estimation**: Calculates total byte size and task count. For copies/moves to
 * multiple destinations, these metrics are scaled accordingly to ensure the progress bar
 * reflects the true workload.
 * 4. **Execution**:
 * - Spawns a dedicated thread for the live progress bar.
 * - Distributes file chunks into a static thread pool for parallel execution via `handleIsoFileOperation`.
 * 5. **Post-Processing & Cleanup**:
 * - Disables signal handlers and joins the progress thread.
 * - **Database Sync**: If files were moved or copied, a **synchronous** update is triggered
 * for the affected directories. This ensures the database is fully indexed before
 * returning control to the user.
 *
 * @param input Raw user input string (indices, ranges, or keywords).
 * @param isoFiles Master list of files used for index mapping.
 * @param process Operation type string: "cp", "mv", or "rm".
 * @param[out] umountMvRmBreak Flag set to false if no tasks were successfully processed.
 * @param filterHistory Indicates if the current file list is a filtered view.
 * @param verbose Toggle for detailed per-file progress output.
 */
void processInputForCpMvRm(const std::string& input,
						   const std::vector<std::string>& isoFiles,
						   const std::string& process,
						   bool& umountMvRmBreak, bool& filterHistory,
						   bool& verbose) {
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

    tokenizeInput(input, isoFiles, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isDelete ? GlobalConcurrency::RM_THREAD_CAP : GlobalConcurrency::CPMV_THREAD_CAP;
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), cap, poolSize}));

    std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirCpMv(isoFiles, indexChunks, userDestDir,
                                                       operationColor, operationDescription, umountMvRmBreak,
                                                       filterHistory, isDelete, isCopy, abortDel, overwriteExisting);

    GlobalState::g_operationCancelled.store(false);

    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
        verboseSets.uniqueErrorTokenMessages.clear();
        return;
    }
    verboseSets.uniqueErrorTokenMessages.clear();
    clearScrollBuffer();

    size_t totalBytes = 0;
	for (const auto& chunk : indexChunks) {
		for (int idx : chunk) {
			struct ::stat st;
			if (::stat(isoFiles[idx - 1].c_str(), &st) == 0)
				totalBytes += st.st_size;
		}
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

    std::cout << "\n" << color << " Processing "
              << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process
              << color << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << color << ":cancel)\n";

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
		futures.emplace_back(pool.enqueue([chunk, &isoFiles,
										   &userDestDir, isMove, isCopy, isDelete,
										   &completedBytes, &completedTasks, &failedTasks,
										   &overwriteExisting, &successfulDestPaths, &destPathsMutex]() {
			std::vector<std::string> isoFilesInChunk;
			isoFilesInChunk.reserve(chunk.size());
			for (int idx : chunk)
				isoFilesInChunk.push_back(isoFiles[idx - 1]);
			handleIsoFileOperation(isoFilesInChunk, isoFiles,
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
            updateDatabaseAfterOperations(exactPaths);
    }
}

/**
 * @brief Handles bulk image-to-ISO conversions with threading and progress visualization.
 *
 * Each selected file is enqueued as an independent task directly into the static
 * thread pool rather than being pre-chunked. This keeps all available threads busy
 * regardless of whether the file count divides evenly across threads — the pool's
 * internal queue naturally balances load as threads free up.
 *
 * @param input           Raw user input string (indices, ranges, or keywords).
 * @param fileList        Master list of image files used for index mapping.
 * @param modeMdf         True if converting MDF images.
 * @param modeNrg         True if converting NRG images.
 * @param modeChd         True if converting CHD images.
 * @param modeDaa         True if converting DAA/GBI images.
 * @param verbose         Toggle for detailed per-file progress output.
 * @param needsClrScrn    Set to true if the screen should be cleared on return.
 */
void processInputForConversions(const std::string& input,
                                 std::vector<std::string>& fileList,
                                 const bool& modeMdf, const bool& modeNrg,
                                 const bool& modeChd, const bool& modeDaa,
                                 bool& verbose
                                 )
{
    std::vector<std::string> successfulOutputPaths;
    std::mutex outPathsMutex;

    setupSignalHandlerCancellations();

    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    GlobalState::g_operationCancelled.store(false);
    std::unordered_set<int> processedIndices;

    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))) {
        tokenizeInput(input, fileList, processedIndices);
    } else return;

    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n" << (isOrig ? UI::Palette::Red : theme->secondary)
                  << "No valid input provided." << UI::Palette::BoldReset << "\n";
        pressEnterToContinue();
        return;
    }

    ThreadPool& pool = getStaticThreadPool();

    // Calculate total bytes across all selected files.
    size_t totalBytes = 0;
    {
        std::vector<std::string> filesToProcess;
        filesToProcess.reserve(processedIndices.size());
        for (int idx : processedIndices)
            filesToProcess.push_back(fileList[idx - 1]);
        totalBytes = calculateTotalBytesForConversions(
            filesToProcess, modeMdf, modeNrg, modeChd, modeDaa);
    }

    const size_t totalTasks = processedIndices.size();
    std::string colorMuted = isOrig
        ? std::string(UI::Palette::BoldReset)
        : std::string(theme->muted);

    std::string suffix = (totalTasks > 1 ? " conversions" : " conversion");
    std::string operation;
    if (modeMdf)      operation = std::string(UI::Palette::Orange) + "mdf2iso" + std::string(color) + suffix;
    else if (modeNrg) operation = std::string(UI::Palette::Orange) + "nrg2iso" + std::string(color) + suffix;
    else if (modeChd) operation = std::string(UI::Palette::Orange) + "chd2iso" + std::string(color) + suffix;
    else if (modeDaa) operation = std::string(UI::Palette::Orange) + "daa2iso" + std::string(color) + suffix;
    else              operation = std::string(UI::Palette::Orange) + "ccd2iso" + std::string(color) + suffix;

    clearScrollBuffer();
    std::cout << "\n" << color << " Processing "
              << operation << color << "... ("
              << UI::Palette::Red << "Ctrl+c"
              << color << ":cancel)\n";

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool>   isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize,
        &completedBytes, totalBytes,
        &completedTasks, &failedTasks, totalTasks,
        &isProcessingComplete, &verbose, operation);

    // Enqueue one task per file — the pool queue balances load naturally.
    std::vector<std::future<void>> futures;
    futures.reserve(processedIndices.size());

    for (int idx : processedIndices) {
        futures.emplace_back(pool.enqueue(
            [&fileList, idx, modeMdf, modeNrg, modeChd, modeDaa,
             &completedBytes, &completedTasks, &failedTasks,
             &successfulOutputPaths, &outPathsMutex]() {
                convertToISO({fileList[idx - 1]},
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
        updateDatabaseAfterOperations(exactPaths);
    }
}
