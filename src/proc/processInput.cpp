// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/stat.h>

// Project Headers
#include "isocmd/core/concurrency.h"
#include "isocmd/ops/convert.h"
#include "isocmd/ops/cpMvRm.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/ops/mount.h"
#include "isocmd/ui/pausePrompts.h"
#include "isocmd/proc/process.h"
#include "isocmd/core/state.h"
#include "isocmd/ui/verbose.h"
#include "isocmd/core/threadpool.h"
#include "isocmd/input/tokenize.h"
#include "isocmd/ops/umount.h"

namespace fs = std::filesystem;

/**
 * @brief High-level processing functions for mounting, file management
 *        (Cp/Mv/Rm), and format conversions.
 */

/**
 * @brief Resolves raw user input into a list of 1-based file indices to process.
 *
 * Expands `"00"` to all indices in @p files, or tokenizes @p input via
 * `tokenizeInput`. Returns an empty vector when no valid indices are selected.
 *
 * @param input Raw user input string (comma/space separated indices or `"00"`).
 * @param files Master list of available ISO file paths.
 * @return Vector of selected 1-based file indices.
 */
static std::vector<int> resolveSelectedIndices(const std::string& input,
                                             const std::vector<std::string>& files) {
    std::unordered_set<int> indicesToProcess;

    if (input == "00") {
        for (int i = 1; i <= static_cast<int>(files.size()); ++i) {
            indicesToProcess.insert(i);
        }
    } else {
        tokenizeInput(input, files, indicesToProcess);
        if (indicesToProcess.empty()) {
            return {};
        }
    }

    std::vector<int> selectedIndices;
    selectedIndices.reserve(indicesToProcess.size());
    for (int index : indicesToProcess) {
        selectedIndices.push_back(index);
    }
    return selectedIndices;
}

/**
 * @brief Computes the worker thread count and partitions indices into chunks.
 *
 * Bounds the thread count to at least `2` and at most
 * `min(selectionCount, perOperationCap, poolSize)`, then increases thread count
 * if needed so no chunk exceeds `100` indices. Distributes indices round-robin
 * across the resulting chunks.
 *
 * @param selectionCount Total number of selected ISO indices.
 * @param poolSize       Number of worker threads available in the pool.
 * @param perOperationCap Maximum concurrency allowed for the active operation.
 * @param selectedIndices Selected 1-based file indices to partition.
 * @return Vector of index chunks, one per worker task.
 */
static std::vector<std::vector<int>> partitionIndexChunks(size_t selectionCount,
                                                          size_t poolSize,
                                                          size_t perOperationCap,
                                                          const std::vector<int>& selectedIndices) {
    size_t numThreads = std::max(size_t(2), std::min({selectionCount, perOperationCap, poolSize}));

    if ((selectionCount + numThreads - 1) / numThreads > 100) {
        numThreads = (selectionCount + 99) / 100;
    }

    std::vector<std::vector<int>> indexChunks(numThreads);
    for (size_t i = 0; i < selectionCount; ++i) {
        indexChunks[i % numThreads].push_back(selectedIndices[i]);
    }
    return indexChunks;
}

/**
 * @brief Builds and prints the formatted operation status header.
 *
 * @param taskCount      Number of tasks being processed.
 * @param operationColor ANSI color code for the active operation.
 * @param operationName  Operation label (`"umount"` or `"mount"`).
 * @return Formatted header string that was written to `std::cout`.
 */
static std::string printOperationHeader(size_t taskCount,
                                        const std::string& operationColor,
                                        const std::string& operationName) {
    std::ostringstream hdr;
    hdr << color << "\n Processing"
        << (taskCount > 1 ? " tasks" : " task")
        << " for " << operationColor << operationName
        << color << "... ("
        << UI::Palette::Red << "Ctrl+c"
        << color << ":cancel)\n";
    const std::string header = hdr.str();
    std::cout << header;
    return header;
}

/**
 * @brief Enqueues and waits for all worker chunks to mount or unmount ISOs.
 *
 * Maps each index chunk to its corresponding file path in @p files and executes
 * either `unmountISO` or `mountIsoFiles` on @p pool, blocking until every
 * submitted future completes.
 *
 * @param pool           Shared thread pool used to execute worker tasks.
 * @param indexChunks    Partitioned 1-based file indices per worker.
 * @param files          Master list of available ISO file paths.
 * @param completedTasks Atomic counter incremented for each completed file.
 * @param failedTasks    Atomic counter incremented for each failed file.
 * @param isUnmount      `true` to unmount ISOs, `false` to mount ISOs.
 */
static void runWorkerChunks(ThreadPool& pool,
                            const std::vector<std::vector<int>>& indexChunks,
                            const std::vector<std::string>& files,
                            std::atomic<size_t>& completedTasks,
                            std::atomic<size_t>& failedTasks,
                            bool isUnmount) {
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& idxChunk : indexChunks) {
        futures.emplace_back(pool.enqueue([idxChunk, &files, &completedTasks, &failedTasks, isUnmount]() {
            std::vector<std::string> chunkStr;
            chunkStr.reserve(idxChunk.size());
            for (int idx : idxChunk) {
                chunkStr.push_back(files[idx - 1]);
            }
            if (isUnmount) {
                unmountISO(chunkStr, &completedTasks, &failedTasks, false);
            } else {
                mountIsoFiles(chunkStr, &completedTasks, &failedTasks, false);
            }
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
}

/**
 * @brief Orchestrates the mounting or unmounting of ISO files using a static
 *        thread pool and progress tracking.
 *
 * Splits the selected indices into at most
 * min(selection, per-operation cap, pool size) chunks (with a floor of 2
 * threads, and a per-chunk cap of 100 indices), enqueues one worker per chunk
 * on the shared static pool, and drives a live progress bar on a dedicated
 * thread. Blocks until every worker finishes and the progress thread has been
 * joined. On unmount, sets @p umountMvRmBreak to false if no task completed,
 * so the caller's loop can react. Removes /mnt/ISOs after processing if it
 * has become empty.
 *
 * @param input           Raw user input string (comma/space separated indices
 *                        or "00" for all).
 * @param files           The master list of available ISO file paths.
 * @param umountMvRmBreak [out] Set to `false` when the selection is empty or
 *                        when an unmount operation completes zero tasks, so the
 *                        caller's execution loop can react.
 * @param verbose         Toggle for detailed per-file progress output; forwarded
 *                        to the progress-display thread, which may update it
 *                        based on a user prompt.
 * @param isUnmount       Set to true for unmounting, false for mounting.
 */
void processInputForMountOrUmount(const std::string& input,
                                  const std::vector<std::string>& files,
                                  bool& umountMvRmBreak,
                                  bool& verbose,
                                  bool isUnmount) {
    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    const std::vector<int> selectedIndices = resolveSelectedIndices(input, files);
    if (selectedIndices.empty()) {
        if (isUnmount) {
            umountMvRmBreak = false;
        }
        return;
    }

    const std::string operationColor = std::string(isUnmount ? UI::Palette::Yellow : UI::Palette::Green);
    const std::string operationName = isUnmount ? "umount" : "mount";
    const std::string header = printOperationHeader(selectedIndices.size(), operationColor, operationName);
    const std::string coloredProcess = operationColor + operationName + std::string(UI::Palette::BoldReset);

    ThreadPool& pool = getStaticThreadPool();
    const size_t cap = isUnmount ? GlobalConcurrency::UMOUNT_THREAD_CAP : GlobalConcurrency::MOUNT_THREAD_CAP;
    const std::vector<std::vector<int>> indexChunks =
        partitionIndexChunks(selectedIndices.size(), pool.threadCount(), cap, selectedIndices);

    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(
        displayProgressBarWithSize,
        nullptr, static_cast<size_t>(0),
        &completedTasks, &failedTasks, selectedIndices.size(),
        &isProcessingComplete, &verbose,
        coloredProcess, header);

    runWorkerChunks(pool, indexChunks, files, completedTasks, failedTasks, isUnmount);

    if (completedTasks == 0 && isUnmount) {
        umountMvRmBreak = false;
    }

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();

    std::error_code ec;
    fs::remove("/mnt/ISOs", ec);
}

/**
 * @brief Groups file indices into chunks for parallel processing, preventing
 *        race conditions by grouping identical filenames.
 *
 * When @p isDelete is false, indices whose files share a base filename are
 * kept in the same chunk so concurrent workers cannot race on the same
 * destination name (e.g. two copies into the same directory that would
 * collide). Remaining unique-name indices are then spread evenly across the
 * leftover threads. When @p isDelete is true, no such collision is possible,
 * so indices are simply round-robined across @p numThreads chunks.
 *
 * @param processedIndices Set of indices selected by the user.
 * @param isoFiles         Master list of file paths.
 * @param numThreads       Desired concurrency level.
 * @param isDelete         True for deletion (skips name-collision grouping).
 * @return A vector of chunks, where each chunk is a vector of indices.
 */
static std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete)
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
 * @brief Calculates the total byte size of all files contained within the index chunks.
 *
 * Iterates through each chunk of file indices, queries the filesystem using `stat`,
 * and accumulates the file sizes for progress tracking.
 *
 * @param indexChunks Vector of index chunks grouping the file indices to process.
 * @param isoFiles    Master list of files used for index resolution.
 * @return size_t     Total size in bytes of all targeted files.
 */
static size_t calculateTotalBytes(const std::vector<std::vector<int>>& indexChunks,
                                  const std::vector<std::string>& isoFiles) {
    size_t totalBytes = 0;
    for (const auto& chunk : indexChunks) {
        for (int idx : chunk) {
            struct ::stat st;
            if (::stat(isoFiles[idx - 1].c_str(), &st) == 0)
                totalBytes += st.st_size;
        }
    }
    return totalBytes;
}

/**
 * @brief Generates a color-coded process string for terminal progress display.
 *
 * Associates specific ANSI color codes from the UI palette depending on whether
 * the operation is a deletion, move, or copy.
 *
 * @param process  Operation type string ("cp", "mv", or "rm").
 * @param isDelete Flag indicating if the operation is a deletion.
 * @param isMove   Flag indicating if the operation is a move.
 * @param isCopy   Flag indicating if the operation is a copy.
 * @return std::string Formatted and colored process string.
 */
static std::string getColoredProcess(const std::string& process, bool isDelete, bool isMove, bool isCopy) {
    if (isDelete) return std::string(UI::Palette::Red)    + process + std::string(UI::Palette::BoldReset);
    if (isMove)   return std::string(UI::Palette::Yellow) + process + std::string(UI::Palette::BoldReset);
    if (isCopy)   return std::string(UI::Palette::Green)  + process + std::string(UI::Palette::BoldReset);
    return process;
}

/**
 * @brief Enqueues file operation tasks into the static thread pool and waits
 *        for completion.
 *
 * Distributes the provided @p indexChunks across the pool, invoking
 * `handleIsoFileOperation` for each chunk with the shared @p options and
 * @p progress state, then blocks until every submitted future completes.
 *
 * @param pool        Reference to the static thread pool instance.
 * @param indexChunks Grouped index chunks of files to process.
 * @param options     Immutable operation configuration (paths, flags).
 * @param progress    Shared mutable progress counters and output-path collection.
 */
static void executeFileOperations(ThreadPool& pool,
                                  const std::vector<std::vector<int>>& indexChunks,
                                  const FileOperationOptions& options,
                                  FileOperationProgress& progress) {
    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        futures.emplace_back(pool.enqueue([chunk, &options, &progress]() {
            handleIsoFileOperation(options, chunk, progress);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
}

namespace {

    /**
     * @brief Computes the target worker thread count for a bulk operation.
     *
     * Clamps the requested task count against the operation-specific concurrency cap
     * and the available thread pool capacity, with a minimum floor of 2 threads.
     *
     * @param[in] taskCount Number of selected file indices to process.
     * @param[in] isDelete  True when executing a `rm` operation; false for `cp` or `mv`.
     * @param[in] poolSize  Total number of threads available in the shared pool.
     * @return Worker thread count to allocate for chunking.
     */
    size_t computeTargetThreadCount(size_t taskCount, bool isDelete, size_t poolSize) {
        const size_t cap = isDelete ? GlobalConcurrency::RM_THREAD_CAP : GlobalConcurrency::CPMV_THREAD_CAP;
        return std::max(size_t(2), std::min({taskCount, cap, poolSize}));
    }

    /**
     * @brief Calculates total workload size in bytes and tasks across destinations.
     *
     * Scales base byte and task totals by the number of semicolon-delimited destination
     * directories when running a copy (`cp`) or move (`mv`) operation.
     *
     * @param[in]  indexChunks          Grouped file indices per worker chunk.
     * @param[in]  isoFiles             Master list of ISO file paths.
     * @param[in]  processedUserDestDir Semicolon-separated destination directory string.
     * @param[in]  taskCount            Base count of selected indices.
     * @param[in]  isCopyOrMove         True for `cp` or `mv`; false for `rm`.
     * @param[out] totalBytes           Computed total byte size across all tasks and destinations.
     * @param[out] totalTasks           Computed total task count across all destinations.
     */
    void computeWorkloadTotals(const std::vector<std::vector<int>>& indexChunks,
                                      const std::vector<std::string>& isoFiles,
                                      const std::string& processedUserDestDir,
                                      size_t taskCount,
                                      bool isCopyOrMove,
                                      size_t& totalBytes,
                                      size_t& totalTasks) {
        totalBytes = calculateTotalBytes(indexChunks, isoFiles);
        totalTasks = taskCount;

        if (isCopyOrMove) {
            const size_t destCount = static_cast<size_t>(
                std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';')) + 1;
            totalBytes *= destCount;
            totalTasks *= destCount;
        }
    }

    /**
     * @brief Builds and prints the status header shown before progress tracking starts.
     *
     * @param[in] totalTasks     Total number of tasks to be processed.
     * @param[in] operationColor ANSI color code for the active operation.
     * @param[in] process        Operation name string ("cp", "mv", or "rm").
     * @return Formatted header string passed to the progress display thread.
     */
    std::string printCpMvRmOperationHeader(size_t totalTasks,
                                                  const std::string& operationColor,
                                                  const std::string& process) {
        std::ostringstream hdr;
        hdr << "\n" << color << " Processing "
            << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process
            << color << "... ("
            << UI::Palette::Red << "Ctrl+c"
            << color << ":cancel)\n";
        const std::string header = hdr.str();
        std::cout << header;
        return header;
    }

    /**
     * @brief Synchronizes the database with destination paths after non-delete operations.
     *
     * Joins non-empty destination paths with `;` and invokes `updateDatabaseAfterOperations`
     * when at least one task succeeded on a `cp` or `mv` operation.
     *
     * @param[in] successfulDestPaths Destination paths produced by completed tasks.
     * @param[in] completedTaskCount  Count of tasks that completed successfully.
     * @param[in] isDelete            True for `rm` (skips database update); false otherwise.
     */
    void syncDatabaseForDestinations(const std::vector<std::string>& successfulDestPaths,
                                            size_t completedTaskCount,
                                            bool isDelete) {
        if (completedTaskCount == 0 || isDelete) {
            return;
        }

        std::string exactPaths;
        for (const auto& destPath : successfulDestPaths) {
            if (!exactPaths.empty()) {
                exactPaths += ';';
            }
            exactPaths += destPath;
        }

        if (!exactPaths.empty()) {
            updateDatabaseAfterOperations(exactPaths);
        }
    }

} // namespace

/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 *
 * Orchestrates the lifecycle of filesystem operations (`cp`, `mv`, `rm`) on selected
 * image files, from user confirmation and destination selection through multi-threaded
 * execution and database synchronization.
 *
 * @par Workflow
 * 1. **Input parsing:** Tokenizes the user string to map selections onto the master ISO list.
 * 2. **Pre-processing & validation:** Determines thread caps and triggers destination selection/confirmation.
 * 3. **Progress estimation:** Computes total byte size and task counts, scaling by destination count if needed.
 * 4. **Execution:** Spawns a progress bar thread and distributes workloads into the thread pool.
 * 5. **Post-processing & cleanup:** Cleans up signals, joins threads, and triggers database sync if required.
 *
 * @param[in]     input           Raw user input string (indices, ranges, or keywords).
 * @param[in]     isoFiles        Master list of files used for index mapping.
 * @param[in]     process         Operation type string: "cp", "mv", or "rm".
 * @param[out]    umountMvRmBreak Set to false if no tasks were successfully processed.
 * @param[in,out] filterHistory   Indicates if the current file list is a filtered view.
 * @param[out]    verbose         Toggle for detailed per-file progress output; receives user prompt answers.
 */
void processInputForCpMvRm(const std::string& input,
                           const std::vector<std::string>& isoFiles,
                           const std::string& process,
                           bool& umountMvRmBreak, bool& filterHistory,
                           bool& verbose) {
    setupSignalHandlerCancellations();

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");

    std::unordered_set<int> processedIndices;
    tokenizeInput(input, isoFiles, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }

    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = std::string(isDelete ? UI::Palette::Red : (isCopy ? UI::Palette::Green : UI::Palette::Yellow));

    ThreadPool& pool = getStaticThreadPool();
    const size_t numThreads = computeTargetThreadCount(processedIndices.size(), isDelete, pool.threadCount());
    std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

    bool overwriteExisting = false;
    std::string userDestDir;
    bool abortDel = false;

    CpMvPromptContext promptCtx{
        .isoFiles = isoFiles,
        .indexChunks = indexChunks,
        .operationColor = operationColor,
        .operationDescription = operationDescription,
        .isDelete = isDelete,
        .isCopy = isCopy,
        .userDestDir = userDestDir,
        .umountMvRmBreak = umountMvRmBreak,
        .filterHistory = filterHistory,
        .abortDel = abortDel,
        .overwriteExisting = overwriteExisting,
    };
    const std::string processedUserDestDir = userDestDirCpMv(promptCtx);

    // CpMvPromptContext holds copies of these in/out values; propagate the
    // results back to this function's variables.
    umountMvRmBreak   = promptCtx.umountMvRmBreak;
    filterHistory     = promptCtx.filterHistory;
    abortDel          = promptCtx.abortDel;
    overwriteExisting = promptCtx.overwriteExisting;

    GlobalState::g_operationCancelled.store(false);
    verboseSets.uniqueErrorTokenMessages.clear();

    if ((processedUserDestDir.empty() && (isCopy || isMove)) || abortDel) {
        return;
    }

    clearScrollBuffer();

    size_t totalBytes = 0;
    size_t totalTasks = 0;
    computeWorkloadTotals(indexChunks, isoFiles, processedUserDestDir,
                          processedIndices.size(), isCopy || isMove,
                          totalBytes, totalTasks);

    const std::string header = printCpMvRmOperationHeader(totalTasks, operationColor, process);
    const std::string coloredProcess = getColoredProcess(process, isDelete, isMove, isCopy);

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes,
                               totalBytes, &completedTasks, &failedTasks,
                               totalTasks, &isProcessingComplete, &verbose,
                               coloredProcess, header);

    std::vector<std::string> successfulDestPaths;
    std::mutex destPathsMutex;

    FileOperationOptions options{
        isoFiles,
        processedUserDestDir,
        isMove,
        isCopy,
        isDelete,
        overwriteExisting
    };

    FileOperationProgress progress{
        completedBytes,
        completedTasks,
        failedTasks,
        successfulDestPaths,
        destPathsMutex
    };

    executeFileOperations(pool, indexChunks, options, progress);

    const size_t finalCompletedTasks = completedTasks.load();
    if (finalCompletedTasks == 0) {
        umountMvRmBreak = false;
    }

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();

    syncDatabaseForDestinations(successfulDestPaths, finalCompletedTasks, isDelete);
}

/**
 * @brief Builds the colorized operation label (e.g. "mdf2iso") for the header.
 *
 * Falls back to "ccd2iso" when no other mode flag is set.
 *
 * @param modeMdf True if converting MDF images.
 * @param modeNrg True if converting NRG images.
 * @param modeChd True if converting CHD images.
 * @param modeDaa True if converting DAA/GBI images.
 * @return Orange-highlighted operation name followed by the base color code.
 */
static std::string buildOperationLabel(const bool modeMdf, const bool modeNrg,
                                       const bool modeChd, const bool modeDaa)
{
    const std::pair<bool, const char*> modes[] = {
        {modeMdf, "mdf2iso"},
        {modeNrg, "nrg2iso"},
        {modeChd, "chd2iso"},
        {modeDaa, "daa2iso"},
    };

    const char* name = "ccd2iso";
    for (const auto& [enabled, label] : modes) {
        if (enabled) { name = label; break; }
    }

    return std::string(UI::Palette::Orange) + name + std::string(color);
}

/**
 * @brief Worker loop executed by each pool thread.
 *
 * Repeatedly claims the next file index from the shared atomic counter and
 * converts that file until no indices remain, keeping the load balanced
 * regardless of per-file size.
 *
 * @param fileList  Master list of image files.
 * @param indices   1-based indices of the selected files.
 * @param nextIndex Shared atomic counter of the next unclaimed position.
 * @param mode      Conversion mode flags.
 * @param batchData Shared progress counters and output-path collection.
 */
static void conversionWorker(const std::vector<std::string>& fileList,
                             const std::vector<int>& indices,
                             std::atomic<size_t>& nextIndex,
                             const ConversionMode& mode,
                             const ConversionBatchData& batchData)
{
    for (size_t i; (i = nextIndex.fetch_add(1)) < indices.size();) {
        convertToISO(
            {fileList[indices[i] - 1]},
            mode,
            batchData);
    }
}

/**
 * @brief Joins the successful output paths with ';' and triggers a
 *        synchronous database update.
 *
 * Does nothing if no conversion succeeded.
 *
 * @param successfulOutputPaths Output paths of successfully converted files.
 */
static void updateDatabaseForSuccessfulPaths(
    const std::vector<std::string>& successfulOutputPaths)
{
    if (successfulOutputPaths.empty()) return;

    std::string exactPaths;
    for (const auto& outPath : successfulOutputPaths) {
        if (!exactPaths.empty()) exactPaths += ';';
        exactPaths += outPath;
    }
    updateDatabaseAfterOperations(exactPaths);
}

/**
 * @brief Handles bulk image-to-ISO conversions with threading and progress
 *        visualization.
 *
 * Orchestrates the lifecycle of image conversions (mdf2iso, nrg2iso, chd2iso,
 * daa2iso, ccd2iso) on selected image files, with multi-threaded execution and
 * database synchronization.
 *
 * @par Workflow
 * 1. **Input parsing:** tokenizes the user string to map selections onto the
 *    master file list.
 * 2. **Pre-processing & validation:** determines the thread count as the
 *    smallest of the selected file count, CONV_THREAD_CAP, and the static
 *    pool size (with a floor of 2).
 * 3. **Progress estimation:** computes the total byte size of the selected
 *    files via calculateTotalBytesForConversions() and the task count (one per
 *    file).
 * 4. **Execution:** spawns a dedicated thread for the live progress bar and
 *    enqueues at most numThreads workers into the static thread pool. Each
 *    worker pulls the next file index from a shared atomic counter and calls
 *    convertToISO() until none remain. This caps concurrency at
 *    CONV_THREAD_CAP while keeping load balanced, regardless of whether the
 *    file count divides evenly or individual files vary widely in size.
 * 5. **Post-processing & cleanup:** disables signal handlers, joins the
 *    progress thread, and — if any conversion succeeded — triggers a
 *    synchronous database update for the output paths before returning.
 *
 * @param input    Raw user input string (indices, ranges, or keywords).
 * @param fileList Master list of image files used for index mapping (passed
 *                 non-const for compatibility with `tokenizeInput`).
 * @param modeMdf  True if converting MDF images.
 * @param modeNrg  True if converting NRG images.
 * @param modeChd  True if converting CHD images.
 * @param modeDaa  True if converting DAA/GBI images.
 * @param verbose  Toggle for detailed per-file progress output; also receives
 *                 the user's answer to the verbose prompt.
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

    if (!(input.empty() || std::all_of(input.begin(), input.end(), [](unsigned char ch) {
    return std::isspace(ch);}))) {
        tokenizeInput(input, fileList, processedIndices);
    } else return;

    if (processedIndices.empty()) {
        auto render = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << "\n" << (isOrig ? UI::Palette::Red : theme->secondary)
                      << "No valid input provided." << UI::Palette::BoldReset << "\n";
        };

        pressEnterToReturnWithRepaint(render);
        return;
    }

    ThreadPool& pool = getStaticThreadPool();
    const size_t numThreads = std::max(size_t(2),
        std::min({processedIndices.size(), GlobalConcurrency::CONV_THREAD_CAP,
                  pool.threadCount()}));

    std::vector<std::string> filesToProcess;
    filesToProcess.reserve(processedIndices.size());
    for (int idx : processedIndices)
        filesToProcess.push_back(fileList[idx - 1]);
    const size_t totalBytes = calculateTotalBytesForConversions(
        filesToProcess, modeMdf, modeNrg, modeChd, modeDaa);

    const size_t totalTasks = processedIndices.size();
    const std::string suffix = (totalTasks > 1 ? "tasks" : "task");
    const std::string operation =
        buildOperationLabel(modeMdf, modeNrg, modeChd, modeDaa);

    clearScrollBuffer();
    std::ostringstream hdr;
    hdr << "\n" << color << " Processing " << suffix << " for "
        << operation << color << "... ("
        << UI::Palette::Red << "Ctrl+c"
        << color << ":cancel)\n";
    const std::string header = hdr.str();
    std::cout << header;

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool>   isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize,
        &completedBytes, totalBytes,
        &completedTasks, &failedTasks, totalTasks,
        &isProcessingComplete, &verbose, operation, header);

    const std::vector<int> indices(processedIndices.begin(), processedIndices.end());
    std::atomic<size_t> nextIndex(0);

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    const ConversionMode mode{
        .mdf = modeMdf,
        .nrg = modeNrg,
        .chd = modeChd,
        .daa = modeDaa
    };

    const ConversionBatchData batchData{
        .completedBytes = &completedBytes,
        .completedTasks = &completedTasks,
        .failedTasks = &failedTasks,
        .successfulOutputPaths = &successfulOutputPaths,
        .outPathsMutex = &outPathsMutex
    };

    for (size_t t = 0; t < numThreads; ++t) {
        futures.emplace_back(pool.enqueue(
            [&fileList, &indices, &nextIndex, mode, batchData]() {
                conversionWorker(fileList, indices, nextIndex, mode, batchData);
            }));
    }

    for (auto& future : futures)
        future.wait();

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();

    updateDatabaseForSuccessfulPaths(successfulOutputPaths);
}
