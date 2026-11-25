// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <signal.h>

import std;

import isocmd.core.cancellation;
import isocmd.core.threadCaps;
import isocmd.input.inputHandling;
import isocmd.input.tokenize;
import isocmd.ops.mount;
import isocmd.ops.umount;
import isocmd.proc.process;
import isocmd.select.select;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.themes;

namespace fs = std::filesystem;

/**
 * @brief High-level processing functions for mounting and unmounting ISOs.
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
    const size_t cap = isUnmount ? GlobalThreadCaps::UMOUNT_THREAD_CAP : GlobalThreadCaps::MOUNT_THREAD_CAP;
    const std::vector<std::vector<int>> indexChunks =
        partitionIndexChunks(selectedIndices.size(), pool.threadCount(), cap, selectedIndices);

    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread = launchProgressBar(
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
