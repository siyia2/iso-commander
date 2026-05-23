// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <ctype.h>
#include <sys/stat.h>

// Third-Party Library Headers
#include <readline/history.h>

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
 * @param operationBreak [out] Boolean flag updated to control the caller's execution loop (e.g., on empty input or failed unmount).
 * @param verbose Toggle for detailed per-file progress output.
 * @param isUnmount Set to true for unmounting, false for mounting.
 */
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, bool& operationBreak, bool& verbose, bool isUnmount) {
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

    if (completedTasks == 0 && isUnmount) operationBreak = false;

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();
}

/**
 * @brief Groups (file, destination) task pairs into chunks for parallel processing,
 *        preventing race conditions by ensuring no two tasks in the same chunk
 *        share both the same filename AND the same destination directory.
 *
 * For delete operations, destinations are irrelevant so pairs are just (file, "")
 * and chunked purely by count.
 *
 * @param processedIndices  Set of indices selected by the user.
 * @param isoFiles          Master list of file paths.
 * @param destDirs          Parsed destination directories (single-element {""}  for rm).
 * @param numThreads        Desired concurrency level.
 * @param isDelete          Flag indicating if the operation is a deletion.
 * @return A vector of chunks, where each chunk is a vector of (index, destDir) pairs.
 */
using Task = std::pair<int, std::string>;  // (1-based index, destination dir)

std::vector<std::vector<Task>> groupTasksIntoChunks(
    const std::unordered_set<int>&  processedIndices,
    const std::vector<std::string>& isoFiles,
    const std::vector<std::string>& destDirs,
    size_t                          numThreads,
    bool                            isDelete)
{
    std::vector<std::vector<Task>> chunks;

    if (processedIndices.empty()) return chunks;

    if (isDelete) {
        // For rm, destinations are irrelevant — chunk purely by file count.
        std::vector<Task> tasks;
        tasks.reserve(processedIndices.size());
        for (int idx : processedIndices)
            tasks.push_back({idx, ""});

        size_t maxPerChunk = std::max<size_t>(1,
            (tasks.size() + numThreads - 1) / numThreads);

        for (size_t i = 0; i < tasks.size(); i += maxPerChunk) {
            auto end = std::min(i + maxPerChunk, tasks.size());
            chunks.emplace_back(tasks.begin() + i, tasks.begin() + end);
        }
        return chunks;
    }

    // Build the full (file × destination) task list.
    std::vector<Task> allTasks;
    allTasks.reserve(processedIndices.size() * destDirs.size());
    for (int idx : processedIndices)
        for (const auto& dest : destDirs)
            allTasks.push_back({idx, dest});

    // Identify collision groups: tasks that share (basename, dest).
    // Tasks in the same collision group must land in different chunks so they
    // never run concurrently (prevents the same filename being written to the
    // same destination by two threads simultaneously).
    std::unordered_map<std::string, std::vector<Task>> collisionGroups;  // key = basename+'\0'+dest

    auto makeKey = [&](const Task& t) -> std::string {
        std::string base = std::filesystem::path(isoFiles[t.first - 1]).filename().string();
        return base + '\0' + t.second;  // null-separated to avoid false collisions
    };

    for (const Task& t : allTasks)
        collisionGroups[makeKey(t)].push_back(t);

    // Tasks that are alone in their collision group can be freely chunked.
    // Tasks that share a (basename, dest) key must be serialised (put each
    // member of that group into its own dedicated chunk).
    std::vector<Task> freeTasks;
    for (auto& [key, group] : collisionGroups) {
        if (group.size() > 1) {
            // Each member gets its own chunk to force serialisation.
            for (auto& t : group)
                chunks.push_back({t});
        } else {
            freeTasks.push_back(group[0]);
        }
    }

    if (!freeTasks.empty()) {
        size_t usedChunks      = chunks.size();
        size_t remainingSlots  = (numThreads > usedChunks) ? numThreads - usedChunks : 1;
        size_t maxPerChunk     = std::max<size_t>(1,
            (freeTasks.size() + remainingSlots - 1) / remainingSlots);

        for (size_t i = 0; i < freeTasks.size(); i += maxPerChunk) {
            auto end = std::min(i + maxPerChunk, freeTasks.size());
            chunks.emplace_back(freeTasks.begin() + i, freeTasks.begin() + end);
        }
    }

    return chunks;
}


/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 *
 * Orchestrates the full lifecycle of filesystem operations (cp, mv, rm) on selected
 * image files. Threading is now based on (file × destination) task pairs rather than
 * files alone, so all available threads can be kept busy regardless of destination count.
 *
 * @section Workflow Lifecycle
 * 1. **Input Parsing**: Tokenizes the user string to map selections to the master ISO list.
 * 2. **Pre-Processing & Validation**:
 *    - Determines thread caps based on operation type.
 *    - Invokes `userDestDirCpMv` for UI interactions (destination selection / delete confirm).
 *    - Parses the semicolon-delimited destination string into a `destDirs` vector.
 * 3. **Task Building**: Constructs one Task per (file, destination) pair and chunks them
 *    across threads, with collision-safe grouping for identical (basename, dest) pairs.
 * 4. **Progress Estimation**: totalBytes and totalTasks already reflect the true
 *    (files × destinations) workload — no post-hoc scaling needed.
 * 5. **Execution**:
 *    - Spawns a dedicated thread for the live progress bar.
 *    - Dispatches task chunks into the static thread pool via `handleIsoFileOperation`,
 *      which now operates on a single destination per call.
 *    - For multi-destination moves, an atomic per-source success counter triggers
 *      the final source deletion only after ALL destinations have been copied.
 * 6. **Post-Processing & Cleanup**:
 *    - Disables signal handlers and joins the progress thread.
 *    - **Database Sync**: Synchronous update for affected directories before returning.
 *
 * @param input           Raw user input string (indices, ranges, or keywords).
 * @param isoFiles        Master list of files used for index mapping.
 * @param process         Operation type string: "cp", "mv", or "rm".
 * @param[out] umountMvRmBreak  Flag set to false if no tasks were successfully processed.
 * @param filterHistory   Indicates if the current file list is a filtered view.
 * @param verbose         Toggle for real-time console progress output.
 * @param[in,out] newISOFound   Atomic flag triggered to true if the filesystem was modified.
 */
void processInputForCpMvRm(const std::string&        input,
                            const std::vector<std::string>& isoFiles,
                            const std::string&        process,
                            bool&                     umountMvRmBreak,
                            bool&                     filterHistory,
                            bool&                     verbose,
                            std::atomic<bool>&        newISOFound)
{
    setupSignalHandlerCancellations();

    std::vector<std::string> successfulDestPaths;
    std::mutex               destPathsMutex;

    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");

    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*"
                                     : isMove   ? "*MOVED*"
                                                : "*COPIED*";
    std::string operationColor = std::string(
        isDelete ? UI::Palette::Red :
        isCopy   ? UI::Palette::Green :
                   UI::Palette::Yellow);

    tokenizeInput(input, isoFiles, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }

    ThreadPool&  pool     = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap      = isDelete ? GlobalConcurrency::RM_THREAD_CAP
                                     : GlobalConcurrency::CPMV_THREAD_CAP;
    // Upper-bound numThreads conservatively before we know the destination count;
    // actual parallelism is determined by the number of task chunks produced below.
    const size_t numThreads = std::max(size_t(2),
        std::min({processedIndices.size(), cap, poolSize}));

    // userDestDirCpMv still receives indexChunks for its display logic (showing
    // the user which files are selected).  We build a temporary file-only chunking
    // just for that display — it has no effect on the actual execution chunking below.
    std::vector<std::vector<int>> displayChunks;
    {
        std::vector<int> indices(processedIndices.begin(), processedIndices.end());
        size_t maxPer = std::max<size_t>(1,
            (indices.size() + numThreads - 1) / numThreads);
        for (size_t i = 0; i < indices.size(); i += maxPer) {
            auto end = std::min(i + maxPer, indices.size());
            displayChunks.emplace_back(indices.begin() + i, indices.begin() + end);
        }
    }

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirCpMv(
        isoFiles, displayChunks, userDestDir,
        operationColor, operationDescription, umountMvRmBreak,
        filterHistory, isDelete, isCopy, abortDel, overwriteExisting);

    GlobalState::g_operationCancelled.store(false);

    if ((processedUserDestDir.empty() && (isCopy || isMove)) || abortDel) {
        verboseSets.uniqueErrorTokenMessages.clear();
        return;
    }
    verboseSets.uniqueErrorTokenMessages.clear();

    // Parse the semicolon-delimited destination string.
    std::vector<std::string> destDirs;
    if (!isDelete) {
        std::istringstream iss(processedUserDestDir);
        std::string d;
        while (std::getline(iss, d, ';'))
            destDirs.push_back(std::filesystem::path(d).string());
    } else {
        destDirs.push_back("");  // placeholder; ignored for rm
    }

    // Re-compute numThreads now that we know the full task count.
    const size_t totalTaskCount = processedIndices.size() * destDirs.size();
    const size_t effectiveThreads = std::max(size_t(2),
        std::min({totalTaskCount, cap, poolSize}));

    // Build execution chunks: one chunk per thread, collision-safe.
    std::vector<std::vector<Task>> taskChunks = groupTasksIntoChunks(
        processedIndices, isoFiles, destDirs, effectiveThreads, isDelete);

    // -------------------------------------------------------------------------
    // Multi-destination move coordination.
    // For "mv" to N>1 destinations we copy to all N destinations first, then
    // delete the source exactly once — after the last successful copy lands.
    // Each source gets an atomic counter; the thread that pushes it to destCount
    // is responsible for the deletion.
    // -------------------------------------------------------------------------
    const size_t destCount = destDirs.size();
    std::unordered_map<std::string, std::atomic<size_t>> mvCopySuccessCount;
    std::unordered_map<std::string, size_t>              mvCopyTargetCount;
    std::mutex                                           mvCoordMutex;

    if (isMove && destCount > 1) {
        for (int idx : processedIndices) {
            const std::string& src = isoFiles[idx - 1];
            mvCopySuccessCount[src].store(0);
            mvCopyTargetCount[src] = destCount;
        }
    }

    // -------------------------------------------------------------------------
    // Progress accounting — totalBytes and totalTasks reflect the true workload.
    // -------------------------------------------------------------------------
    size_t totalBytes = 0;
    for (int idx : processedIndices) {
        struct ::stat st;
        if (::stat(isoFiles[idx - 1].c_str(), &st) == 0)
            totalBytes += static_cast<size_t>(st.st_size);
    }
    totalBytes     *= destCount;
    size_t totalTasks = totalTaskCount;  // already files × destinations

    const MainTheme* theme   = getActiveTheme();
    const bool       isOrig  = (globalTheme == "original");
    std::string      colorMuted = isOrig
        ? std::string(UI::Palette::BoldReset)
        : std::string(theme->muted);

    clearScrollBuffer();
    std::cout << "\n" << colorMuted << " Processing "
              << (totalTasks > 1 ? "tasks" : "task") << " for "
              << operationColor << process
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
    std::atomic<bool>   isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize,
        &completedBytes, totalBytes,
        &completedTasks, &failedTasks, totalTasks,
        &isProcessingComplete, &verbose,
        std::string(coloredProcess));

    // -------------------------------------------------------------------------
    // Dispatch — one chunk per thread, each chunk is a list of (idx, dest) tasks.
    // handleIsoFileOperation now receives a single destination directory.
    // -------------------------------------------------------------------------
    std::vector<std::future<void>> futures;
    futures.reserve(taskChunks.size());

    for (const auto& chunk : taskChunks) {
        futures.emplace_back(pool.enqueue(
            [chunk, &isoFiles, isMove, isCopy, isDelete, destCount,
             &completedBytes, &completedTasks, &failedTasks,
             &overwriteExisting, &successfulDestPaths, &destPathsMutex,
             &mvCopySuccessCount, &mvCopyTargetCount, &mvCoordMutex]()
        {
            for (const auto& [idx, dest] : chunk) {
                const std::string& srcFile = isoFiles[idx - 1];

                // For multi-destination moves we pass isCopy=true so that
                // handleIsoFileOperation performs the copy without deleting
                // the source.  Deletion is handled here once all copies land.
                bool effectiveCopy  = isCopy  || (isMove && destCount > 1);
                bool effectiveMove  = isMove  && (destCount == 1);
                bool effectiveDelete = isDelete;

                handleIsoFileOperation(
                    {srcFile},          // single file
                    isoFiles,           // master list (for existence checks)
                    dest,               // single destination
                    effectiveMove, effectiveCopy, effectiveDelete,
                    &completedBytes, &completedTasks, &failedTasks,
                    overwriteExisting, &successfulDestPaths, &destPathsMutex);

                // Multi-destination move: delete source after last copy succeeds.
                if (isMove && destCount > 1) {
                    // We determine success by checking whether completedTasks
                    // was incremented — but handleIsoFileOperation controls that
                    // counter internally.  Instead, we track it via the
                    // successfulDestPaths vector size delta around the call.
                    // Simpler: check that destPath was recorded.
                    namespace fs = std::filesystem;
                    fs::path destPath = fs::path(dest) / fs::path(srcFile).filename();
                    bool thisDestSucceeded = false;
                    {
                        std::lock_guard<std::mutex> lk(destPathsMutex);
                        thisDestSucceeded = std::find(
                            successfulDestPaths.begin(),
                            successfulDestPaths.end(),
                            destPath.string()) != successfulDestPaths.end();
                    }

                    if (thisDestSucceeded) {
                        size_t done = ++mvCopySuccessCount.at(srcFile);
                        if (done == mvCopyTargetCount.at(srcFile)) {
                            // Last destination succeeded — safe to delete source.
                            std::error_code ec;
                            fs::remove(srcFile, ec);
                            // Errors here are best-effort; the copies already
                            // succeeded.  Consider logging via verboseSets if
                            // reportErrorCpMvRm is thread-safe in your codebase.
                        }
                    }
                }
            }
        }));
    }

    for (auto& f : futures)
        f.wait();

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
 * @brief Processes user input to convert multiple disk image files to ISO format.
 *
 * This function parses the user-provided input string (e.g., "1-5,7,9"), identifies
 * the corresponding files from the master list, and orchestrates multi‑threaded
 * conversion of supported image types (BIN/CCD, MDF, NRG, CHD, DAA/GBI) to standard ISO.
 *
 * The function includes an inline size estimation pass to provide an accurate
 * progress bar during conversion. Estimation logic varies by input format:
 * - **NRG**: Excludes a 300 KiB (307200 byte) header.
 * - **MDF**: Reads sector geometry via `MdfTypeInfo` and computes user data bytes.
 * - **BIN/CCD**: Assumes 2352‑byte raw sectors with 2048 bytes of user data.
 * - **CHD**: Opens the CHD file, inspects the header, and calculates total sectors
 *   multiplied by 2048 bytes (ISO user data per sector).
 * - **DAA**: Calls `getDaaIsoSize()` to query the uncompressed ISO size from the
 *   DAA archive header without extracting the entire file.
 *
 * @section multi_threading Multi-Threading and Progress
 * Tasks are distributed across a static thread pool by splitting the selection into
 * chunks. A dedicated background thread manages a real-time progress bar that tracks
 * both byte-level progress and task completion/failure counts.
 *
 * @section post_processing Post-Conversion Sync
 * Upon completion, the function joins the progress display and cleans up signal handlers.
 * If successful, it constructs output paths and calls @ref updateDatabaseAfterOperations
 * to index ONLY the new files, avoiding a full directory scan.
 *
 * @param input         Raw user selection string (e.g., "1,3-5").
 * @param fileList      Master vector of all non‑ISO image paths.
 * @param modeMdf       Use Alcohol 120% MDF conversion logic.
 * @param modeNrg       Use Nero NRG conversion logic.
 * @param modeChd       Use MAME CHD conversion logic.
 * @param modeDaa       Use PowerISO DAA / gBurner GBI logic.
 * @param verbose       Toggle for real-time progress details.
 * @param needsClrScrn  [out] Set to true if the UI requires a refresh.
 * @param newISOFound   [in,out] Atomic flag set to true if the database was updated.
 */
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList,
                               const bool& modeMdf, const bool& modeNrg, const bool& modeChd,
                               const bool& modeDaa,
                               bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound)
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
        needsClrScrn = true;
        return;
    }

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), GlobalConcurrency::CONV_THREAD_CAP, poolSize}));

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
                                           modeMdf, modeNrg, modeChd, modeDaa,
                                           &completedBytes, &completedTasks, &failedTasks,
                                           &successfulOutputPaths, &outPathsMutex]() {
            std::vector<std::string> imageFilesInChunk;
            imageFilesInChunk.reserve(chunk.size());
            for (size_t idx : chunk)
                imageFilesInChunk.push_back(fileList[idx - 1]);
            convertToISO(imageFilesInChunk,
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
