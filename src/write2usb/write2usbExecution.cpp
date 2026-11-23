// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/input/inputHandling.h"
#include "isocmd/ui/pausePrompts.h"
#include "isocmd/util/sort.h"
#include "isocmd/core/cancellation.h"
#include "isocmd/ui/themes.h"
#include "isocmd/threadpool/threadpool.h"
#include "isocmd/input/tokenize.h"
#include "isocmd/write2usb/write2usbUI.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Global scope: external declarations
// ---------------------------------------------------------------------------

void helpMappings();
void displayErrors();

// --------------------------------------------------------------------------- //
// namespace WriteExecution  (write task preparation)                          //
// --------------------------------------------------------------------------- //

namespace WriteExecution {

    /**
     * @brief Builds the per-task ProgressInfo vector from (IsoInfo, device)
     * pairs, in the order they would appear after a stable, case-insensitive
     * natural sort by ISO filename (device path as tie-break).
     *
     * The caller's vector is not modified or copied. A vector of indices into
     * @p validPairsIn is stable-sorted instead, and the pairs are visited in
     * that order; only the four fields ProgressInfo carries (filename, device,
     * sizeStr, sourcePath) are copied, directly into the progress vector.
     *
     * The progress vector is reserved to exactly @p validPairsIn.size() before
     * filling, so its backing storage never reallocates after construction
     * (ProgressInfo holds atomics and is therefore non-movable — the reserve is
     * load-bearing, not just an optimisation).
     *
     * The returned shared_ptr is captured by every pooled write task and by the
     * cancellation drain, so the entries outlive this function whenever a task
     * or drain thread still needs them.
     *
     * @par Ordering
     * Primary key: IsoInfo::filename via naturalCompare (case-insensitive,
     * numeric-aware: "archlinux-9.iso" before "archlinux-10.iso"). Secondary
     * key: the device path under ordinary std::string ordering. Equal
     * (filename, device) keys keep their input order.
     *
     * @param validPairsIn Validated (IsoInfo, device) pairs targeted for writing.
     *                     Not modified.
     * @return A shared pointer to a new vector of ProgressInfo entries, one per
     *         input pair, in stable sorted order.
     *
     * @see ::performWriteOperation
     */
    std::shared_ptr<std::vector<ProgressInfo>>
    prepareWriteData(const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn) {
        std::vector<size_t> order(validPairsIn.size());
        std::iota(order.begin(), order.end(), size_t{0});

        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const int c = naturalCompare(validPairsIn[a].first.filename,
                                         validPairsIn[b].first.filename);
            if (c != 0) return c < 0;
            return validPairsIn[a].second < validPairsIn[b].second;
        });

        auto progressPtr = std::make_shared<std::vector<ProgressInfo>>();
        auto& progress   = *progressPtr;
        progress.reserve(validPairsIn.size());

        for (size_t idx : order) {
            const auto& [iso, device] = validPairsIn[idx];
            progress.push_back(ProgressInfo{iso.filename, device, iso.sizeStr, iso.path});
        }
        return progressPtr;
    }

} // namespace WriteExecution

// --------------------------------------------------------------------------- //
// Write execution (public API)                                                //
// --------------------------------------------------------------------------- //

/**
 * @brief Detaches in-flight write tasks into per-device background drain
 * threads after cancellation.
 *
 * Called by performWriteOperation() after Ctrl+C
 * (@c GlobalState::g_operationCancelled). Transfers the outstanding futures
 * to background drain threads managed by @c g_drainingManager, allowing
 * writes and pending cache flushes to finish after the operation returns to
 * the prompt.
 *
 * One managed background thread is started per future. Each waits on its
 * future and decrements the active drain count on completion. When the last
 * active drain thread finishes, @c g_drainingCancelled is cleared.
 *
 * Only devices whose tasks had neither completed nor failed when
 * cancellation was handled are counted as active drains. Tasks that have
 * already returned from writeIsoToDevice() need no further draining.
 *
 * The progress vector is shared rather than copied or moved. Each drain
 * thread captures the shared pointer to keep the per-operation records alive
 * until its future completes.
 *
 * @param futures        Per-task futures, taken by value and moved into the
 *                       background drain threads.
 * @param progressData   Per-task progress records. Read only: .completed /
 *                       .failed are inspected to decide which tasks still
 *                       need draining.
 * @param progressDataPtr Shared ownership of the progress records; captured
 *                       by each thread to keep the operation data alive until
 *                       that task's drain completes.
 *
 * @note The caller's future vector is consumed by this call; ownership of
 *       each future is transferred to its background drain thread.
 *
 * @note @p futures and @p progressData must have the same size, and each
 *       thread indexes both. A mismatch is an out-of-bounds read.
 *
 * @note May add multiple threads to @c g_drainingManager in one call
 *       (one per future).
 *
 * @note Only for the cancellation path. The non-cancelled path in
 *       performWriteOperation waits on the futures inline instead, because
 *       nothing needs to outlive the function in that case.
 *
 * @see g_drainingCancelled, g_drainingManager
 */
void drainManagerForWrite2UsbUI(
    std::vector<std::future<void>> futures,
    const std::vector<ProgressInfo>& progressData,
    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr)
{
    static std::atomic<size_t> activeDrains{0};

    std::vector<bool> needsDrain;
    needsDrain.reserve(progressData.size());

    size_t newlyDraining = 0;
    for (size_t i = 0; i < progressData.size(); ++i) {
        const bool pending =
            !progressData[i].completed.load() && !progressData[i].failed.load();
        needsDrain.push_back(pending);
        if (pending) {
            ++newlyDraining;
        }
    }

    if (newlyDraining > 0) {
        activeDrains.fetch_add(newlyDraining, std::memory_order_relaxed);
        g_drainingCancelled.store(true, std::memory_order_release);
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        g_drainingManager.add(std::thread([
            f = std::move(futures[i]),
            tracked = needsDrain[i],
            progressDataPtr
        ]() mutable {
            (void)progressDataPtr;

            if (f.valid()) {
                f.wait();
            }

            if (tracked) {
                if (activeDrains.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    g_drainingCancelled.store(false, std::memory_order_release);
                }
            }
        }));
    }
}

/**
 * @brief Executes validated ISO-to-device writes and manages the progress
 * screen plus completion / cancellation handling.
 *
 * Creates the per-task progress vector with WriteExecution::prepareWriteData, which
 * establishes the ordering by natural, case-insensitive ISO filename sort,
 * device path as tie-breaker. Task index and display-row index stay aligned.
 *
 * Submits one writeIsoToDevice call per task to the global ThreadPool. A
 * shared WriteSync state coordinates task completion between worker threads,
 * the main thread and the WriteScreen keeper.
 *
 * @par Normal completion
 * Waits until all tasks finish, disables further SIGINT handling, and paints
 * the result screen. The returned screen stays alive so the caller can
 * display a prompt and keep resize-triggered repainting. All valid futures
 * are waited on before returning.
 *
 * @par Cancellation (Ctrl+C)
 * The cancellation flag is part of the wait predicate. The signal handler
 * cannot safely notify the condition variable, so WriteScreen chains its own
 * SIGINT handler in front of the cancellation handler and relays the signal
 * through its self-pipe; the relay thread then notifies the condition
 * variable and the main thread wakes immediately, with no periodic recheck.
 * Only if that relay could not be installed does the wait fall back to
 * rechecking every 500 ms.
 *
 * When cancellation is detected, the result screen is painted and terminal
 * input state is restored. Any tasks still running are transferred to
 * drainManagerForWrite2UsbUI, which keeps their futures alive while they
 * finish in the background. This allows outstanding writes and OS page-cache
 * flushing to continue. The drain manager removes each device from
 * g_drainingDevices when its drain completes.
 *
 * The result screen reflects task states at the time it is painted; tasks
 * that continue running after cancellation may finish later. The successful
 * count in the summary is the count recorded at that time.
 *
 * @par Lifetime and ownership
 * The progress vector and synchronization state are held by shared pointers
 * captured by worker tasks and retained by the screen. Cancellation transfers
 * the outstanding futures and the required shared state to the drain manager,
 * preventing those objects from being destroyed while background tasks still
 * depend on them.
 *
 * The caller owns the returned shared_ptr<WriteScreen>. It should retain the
 * screen while waiting for user input and stop the screen when
 * resize-triggered repainting is no longer needed, or allow the screen to be
 * destroyed when its shared ownership ends.
 *
 * @param validPairsIn Validated (IsoInfo, device) pairs to write. Not
 *                     modified.
 *
 * @return A shared pointer to the live WriteScreen, allowing the caller to
 *         retain the result screen while waiting for user input.
 */
std::shared_ptr<WriteScreen> performWriteOperation(
        const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn) {
    // One call builds the sorted per-task progress vector and establishes the
    // ordering invariant (task i ↔ progress row i). No sorted-pair vector is
    // materialised, so only one shared pointer travels through the tasks and drain.
    auto progressDataPtr = WriteExecution::prepareWriteData(validPairsIn);
    auto& progressData   = *progressDataPtr;

    const size_t totalTasks = progressData.size();

    auto syncPtr = std::make_shared<WriteSync>();
    auto& finishedTasks   = syncPtr->finishedTasks;
    auto& progressCvMutex = syncPtr->mutex;
    auto& progressCv      = syncPtr->cv;

    ThreadPool& pool = getStaticThreadPool();

    disableInput();
    clearScrollBuffer();

    auto screen = createWriteScreen(progressDataPtr, syncPtr, getWriteTheme());
    startWriteScreen(*screen);

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < totalTasks; ++i) {
        futures.push_back(pool.enqueue([progressDataPtr, syncPtr, i]() {
            auto& progress = (*progressDataPtr)[i];
            const bool success = writeIsoToDevice(
                progress.sourcePath, progress.device, i, *progressDataPtr);

            if (success) {
                progress.completed.store(true);
                syncPtr->completedTasks.fetch_add(1);
            } else if (!GlobalState::g_operationCancelled.load()) {
                progress.failed.store(true);
            }

            syncPtr->finishedTasks.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(syncPtr->mutex);
            }
            // notify_all: the screen's keeper thread and the main thread wait
            // on this same condition variable, and notify_one could wake the
            // wrong one.
            syncPtr->cv.notify_all();
        }));
    }

    // Wait for all tasks to finish. Each task's notify_all() wakes this
    // thread on completion. Ctrl+C is also a wakeup: a SIGINT handler cannot
    // notify a condition variable itself, but WriteScreen chains into it and
    // relays the signal through its self-pipe, so its relay thread notifies
    // this same condition variable after g_operationCancelled is set. The
    // wait is therefore unbounded, with no periodic recheck.
    //
    // Only if that relay could not be set up (pipe/handler failure) do we
    // fall back to a 500 ms recheck so cancellation still works.
    {
        std::unique_lock<std::mutex> lock(progressCvMutex);
        const auto done = [&] {
            return finishedTasks.load() == totalTasks ||
                   GlobalState::g_operationCancelled.load();
        };

        if (writeScreenCancelWakeable(*screen)) {
            progressCv.wait(lock, done);
        } else {
            while (!done())
                progressCv.wait_for(lock, std::chrono::milliseconds(500), done);
        }
    }

    signal(SIGINT, SIG_IGN);

    finishWriteScreen(*screen);

    flushStdin();
    restoreInput();

    if (GlobalState::g_operationCancelled.load()) {
        drainManagerForWrite2UsbUI(
            std::move(futures),
            progressData,
            progressDataPtr);
    } else {
        for (auto& future : futures) {
            if (future.valid()) {
                future.wait();
            }
        }
    }

    return screen;
}

/**
 * @brief Entry point for the write-to-USB workflow.
 *
 * Orchestrates the full flashing process:
 * 1. Parses the raw @p input string into unique ISO indices.
 * 2. Resolves filesystem metadata (size, name) for each selected ISO,
 *    skipping any files that are inaccessible or no longer present.
 * 3. Prompts the user for target device mappings via collectDeviceMappings.
 * 4. Clears staged pending indices and paths upon successful confirmation.
 * 5. Executes the flashing process via performWriteOperation, which returns
 *    the live WriteScreen.
 * 6. Keeps that screen alive while waiting for the user to press Enter, so
 *    the result screen still re-truncates and re-wraps when the terminal is
 *    resized, then stops it.
 *
 * @param input          Raw selection string from the main menu (e.g. "1 3-5").
 * @param isoFiles       Full ordered list of available ISO paths.
 */
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles) {
    clearScrollBuffer();
    std::unordered_set<int> indicesToProcess;

    setupSignalHandlerCancellations();
    GlobalState::g_operationCancelled.store(false);

    tokenizeInput(input, isoFiles, indicesToProcess);
    if (indicesToProcess.empty()) {
        return;
    }

    std::vector<IsoInfo> selectedIsos;
    for (int idx : indicesToProcess) {
        const std::string& path = isoFiles[idx - 1];
        std::filesystem::path fsPath(path);
        std::error_code ec;
        auto size = std::filesystem::file_size(fsPath, ec);
        if (ec) continue;
        selectedIsos.emplace_back(IsoInfo{
            path,
            fsPath.filename().string(),
            size,
            formatFileSize(size),
            static_cast<size_t>(idx)
        });
    }

    if (selectedIsos.empty()) {
        return;
    }

    auto validPairs = collectDeviceMappings(selectedIsos);
    if (validPairs.empty()) {
        return;
    }

    auto screen = performWriteOperation(validPairs);

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    setWriteScreenPrompt(*screen, std::string(color) + "\n↵ to continue...");

    pressEnterToContinueWithRepaint([&] { repaintWriteScreen(*screen); });

    stopWriteScreen(*screen);
}
