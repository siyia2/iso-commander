// SPDX-License-Identifier: GPL-3.0-or-later

#include <signal.h>

import std;
import isocmd.core.cancellation;
import isocmd.input.inputHandling;
import isocmd.input.tokenize;
import isocmd.settings.settings;
import isocmd.threadpool.threadpool;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.display.themes;
import isocmd.util.sort;
import isocmd.write2usb.util.drainingState;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;

// Constructor
ProgressInfo::ProgressInfo(std::string filename, std::string device, std::string totalSize,
             std::string sourcePath)
    : filename(std::move(filename)),
      device(std::move(device)),
      totalSize(std::move(totalSize)),
      sourcePath(std::move(sourcePath)) {}

// Move Constructor
ProgressInfo::ProgressInfo(ProgressInfo&& other) noexcept
    : filename(std::move(other.filename)),
      device(std::move(other.device)),
      totalSize(std::move(other.totalSize)),
      sourcePath(std::move(other.sourcePath)),
      cancelToken(std::move(other.cancelToken)),
      bufferedWrites(other.bufferedWrites),
      verifyRequested(other.verifyRequested),
      verifying(other.verifying.load()),
      bytesVerified(other.bytesVerified.load()),
      writeCompleted(other.writeCompleted.load()),
      completed(other.completed.load()),
      failed(other.failed.load()),
      bytesWritten(other.bytesWritten.load()),
      progress(other.progress.load()),
      speed(other.speed.load()),
      verificationSpeed(other.verificationSpeed.load()) {}

// Move Assignment
ProgressInfo& ProgressInfo::operator=(ProgressInfo&& other) noexcept {

	if (this != &other) {
		filename = std::move(other.filename);
		device = std::move(other.device);
		totalSize = std::move(other.totalSize);
		sourcePath = std::move(other.sourcePath);
		cancelToken = std::move(other.cancelToken);
		bufferedWrites = other.bufferedWrites;
		verifyRequested = other.verifyRequested;
		verifying.store(other.verifying.load());
		bytesVerified.store(other.bytesVerified.load());
		writeCompleted.store(other.writeCompleted.load());
		completed.store(other.completed.load());
		failed.store(other.failed.load());
		bytesWritten.store(other.bytesWritten.load());
		progress.store(other.progress.load());
		speed.store(other.speed.load());
		verificationSpeed.store(other.verificationSpeed.load());
	}
	return *this;

}

namespace fs = std::filesystem;

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

		std::vector<std::size_t> order(validPairsIn.size());
		std::iota(order.begin(), order.end(), std::size_t{0});

		std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {

			const int c = naturalCompare(validPairsIn[a].first.filename,
			                             validPairsIn[b].first.filename);
			if (c != 0) return c < 0;
			return validPairsIn[a].second < validPairsIn[b].second;

		});

		auto progressPtr = std::make_shared<std::vector<ProgressInfo>>();
		auto& progress   = *progressPtr;
		progress.reserve(validPairsIn.size());

		for (std::size_t idx : order) {
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
 * (the batch cancellation token has already been latched). Transfers the outstanding futures
 * to background drain threads managed by @c g_drainingManager, allowing
 * writes and pending cache flushes to finish after the operation returns to
 * the prompt.
 *
 * One managed background thread is started per pending future. Each waits
 * on its future and releases the device reservation on completion. When the last
 * active drain thread finishes, @c g_drainingCancelled is cleared.
 *
 * Only devices whose futures are not ready are reserved. A task may publish
 * a completion or failure flag before its mount cleanup has finished.
 *
 * The progress vector is shared rather than copied or moved. Each drain
 * thread captures the shared pointer to keep the per-operation records alive
 * until its future completes.
 *
 * @param futures        Per-task futures, taken by value and moved into the
 *                       background drain threads.
 * @param progressData   Per-task progress records, used to identify the reserved device.
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

	// Progress flags may be set before the writer's RAII cleanup completes.
	// Future readiness, not those flags, determines whether a device is safe
	// to reuse. Register pending devices before returning to the UI.
	for (std::size_t i = 0; i < futures.size(); ++i) {
		if (!futures[i].valid() ||
		    futures[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		    continue;

		const auto& device = progressData[i].device;
		g_drainingManager.reserveDevice(device);
		g_drainingManager.add(std::thread([
		    f = std::move(futures[i]), i, progressDataPtr
		]() mutable {

			f.wait();
			g_drainingManager.releaseDevice((*progressDataPtr)[i].device);

		}));
	}

}

namespace {

	void submitWriteTasks(ThreadPool& pool,
	                      const std::shared_ptr<std::vector<ProgressInfo>>& progressDataPtr,
	                      const std::shared_ptr<WriteSync>& syncPtr,
	                      WriteExecution::Writer writer,
	                      std::vector<std::future<void>>& futures) {

		for (std::size_t i = 0; i < progressDataPtr->size(); ++i) {
			futures.push_back(pool.enqueue([progressDataPtr, syncPtr, i, writer]() {

				auto& progress = (*progressDataPtr)[i];
				bool success = false;
				try {
					if (!isUsbWriteCancelled(progress.cancelToken))
					    success = writer(progress.sourcePath, progress.device, i, *progressDataPtr);
				} catch (...) {
					// A throwing writer must still publish task completion and wake waiters.
				}

				if (success) {
					progress.writeCompleted.store(true);
					progress.completed.store(true);
					syncPtr->completedTasks.fetch_add(1);
				} else if (!isUsbWriteCancelled(progress.cancelToken)) {
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
	void waitForWriteTasks(WriteSync& sync, const WriteScreen& screen, std::size_t totalTasks) {

		std::unique_lock<std::mutex> lock(sync.mutex);
		const auto done = [&] {

			return sync.finishedTasks.load() == totalTasks ||
			       GlobalState::g_operationCancelled.load();

		};

		if (writeScreenCancelWakeable(screen)) {
			sync.cv.wait(lock, done);
		} else {
			while (!done())
			    sync.cv.wait_for(lock, std::chrono::milliseconds(500), done);
		}

	}

} // namespace

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
 * the result screen. Batch elapsed time spans task dispatch through the last
 * writer returning, including optional verification on every device. Parallel
 * durations overlap; they are not summed. The returned screen stays alive so the caller can
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
 * When cancellation is detected, the batch stop state is latched permanently
 * before the result screen is painted and terminal
 * input state is restored. Any tasks still running are transferred to
 * drainManagerForWrite2UsbUI, which keeps their futures alive while they
 * finish in the background.
 *
 * The result screen reflects task states at the time it is painted; tasks
 * that are completing in-flight I/O and cleanup may return later. The successful
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
 * @param writer Function used to execute each write task.
 * @param verify        Enable readback verification for all writers in the batch.
 * @param[out] allSucceeded Optional synchronous result; false on failure or cancellation.
 *                         Never retained by the worker or drain threads.
 *
 * @return A shared pointer to the live WriteScreen, allowing the caller to
 *         retain the result screen while waiting for user input.
 */
std::shared_ptr<WriteScreen> WriteExecution::performWithWriter(
        const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn, Writer writer,
        bool* allSucceeded, bool verify) {

	// One call builds the sorted per-task progress vector and establishes the
	// ordering invariant (task i ↔ progress row i). No sorted-pair vector is
	// materialised, so only one shared pointer travels through the tasks and drain.
	auto progressDataPtr = WriteExecution::prepareWriteData(validPairsIn);
	auto& progressData   = *progressDataPtr;

	const std::size_t totalTasks = progressData.size();

	auto syncPtr = std::make_shared<WriteSync>();
	const bool buffered = write2usbBufferedEnabled();
	for (auto& row : progressData) {
		row.verifyRequested = verify;
		row.bufferedWrites = buffered;
		row.cancelToken = syncPtr->cancellation.get_token();
	}

	ThreadPool& pool = getStaticThreadPool();

	disableInput();
	clearScrollBuffer();

	auto screen = createWriteScreen(progressDataPtr, syncPtr, getWriteTheme());
	startWriteScreen(*screen);

	std::vector<std::future<void>> futures;
	futures.reserve(totalTasks);
	const auto batchStart = std::chrono::steady_clock::now();
	submitWriteTasks(pool, progressDataPtr, syncPtr, writer, futures);
	waitForWriteTasks(*syncPtr, *screen, totalTasks);

	// Each writer returns only after its write and optional verification.
	// Freeze the batch wall-clock time before rendering or cancellation draining.
	const auto batchElapsed = std::chrono::steady_clock::now() - batchStart;
	signal(SIGINT, SIG_IGN);
	const bool cancelled = GlobalState::g_operationCancelled.load();
	if (cancelled) syncPtr->cancellation.request_stop();

	finishWriteScreen(*screen, batchElapsed);

	flushStdin();
	restoreInput();

	if (cancelled) {
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

	if (allSucceeded) {
		*allSucceeded = !cancelled &&
		                syncPtr->completedTasks.load() == totalTasks;
	}

	return screen;

}

std::shared_ptr<WriteScreen> performWriteOperation(
        const std::vector<std::pair<IsoInfo, std::string>>& validPairsIn, bool* allSucceeded, bool verify) {

	return WriteExecution::performWithWriter(validPairsIn, writeIsoToDevice, allSucceeded, verify);

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
			static_cast<std::size_t>(idx)
		});
	}

	if (selectedIsos.empty()) {
		return;
	}

	auto validPairs = collectDeviceMappings(selectedIsos);
	if (validPairs.empty()) {
		return;
	}

	auto screen = performWriteOperation(validPairs, nullptr, write2usbChecksumEnabled());

	signal(SIGINT, SIG_IGN);
	disable_ctrl_d();

	setWriteScreenPrompt(*screen, std::string(color) + "\n↵ to continue...");

	pressEnterToContinueWithRepaint([&] { repaintWriteScreen(*screen); });

	stopWriteScreen(*screen);

}
