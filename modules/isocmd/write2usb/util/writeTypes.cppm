// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.writeTypes;

import std;
import isocmd.core.cancellation;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/writeTypes ----

// The global flag is the signal-safe front end. The UI latches the batch's
// stop state before allowing the global flag to be reset.
inline bool isUsbWriteCancelled(const std::stop_token& token = {}) noexcept {
	// Read the UI flag first: observing its reset must not be followed by
	// a stale pre-latch token check.
	return GlobalState::g_operationCancelled.load() || token.stop_requested();
}

// --- Data Structures ---

/**
 * @brief Information about an ISO image file.
 */
struct IsoInfo {
	std::string path;          /** Full filesystem path to the ISO file. */
	std::string filename;      /** Name of the file without path. */
	std::uint64_t size;             /** Size of the ISO in bytes. */
	std::string sizeStr;       /** Human-readable size string (e.g., "4.2 GB"). */
	std::size_t originalIndex;      /** Original sort index for maintaining list order. */
};

/**
 * @brief Thread-safe state tracker for an ongoing write process.
 * @details Utilizes atomics for progress updates and move-only semantics to
 * prevent accidental copying of atomic states.
 */
struct ProgressInfo {
	std::string filename;
	std::string device;
	std::string totalSize;
	std::string sourcePath;

	std::stop_token cancelToken; // Shared by the batch; never reset after dispatch.
	std::optional<bool> bufferedWrites; // Snapshot before dispatch; unset for standalone callers.
	bool verifyRequested = false; // Set before dispatch; immutable while running.
	std::atomic<bool> verifying{false};
	std::atomic<std::uint64_t> bytesVerified{0};

	std::atomic<bool> writeCompleted{false}; // All writes and final flush succeeded.
	std::atomic<bool> completed{false};      // Write and any requested verification succeeded.
	std::atomic<bool> failed{false};
	std::atomic<std::uint64_t> bytesWritten{0};
	std::atomic<int> progress{0};
	std::atomic<double> speed{0.0}; // Live write speed, then the final write-only average.
	std::atomic<double> verificationSpeed{0.0};

	// Constructor
	ProgressInfo(std::string filename, std::string device, std::string totalSize,
	             std::string sourcePath);

	// Move Constructor
	ProgressInfo(ProgressInfo&& other) noexcept;

	// Move Assignment
	ProgressInfo& operator=(ProgressInfo&& other) noexcept;

	// Disable Copying (Atomics cannot be copied)
	ProgressInfo(const ProgressInfo&) = delete;
	ProgressInfo& operator=(const ProgressInfo&) = delete;
};

/**
 * @brief Completion-tracking state shared between pooled write tasks and the UI.
 *
 * Holds the counters, mutex and condition variable that pooled write tasks
 * update and that the display thread and the main thread wait on. It is owned
 * through a @c std::shared_ptr that every task captures by value, so it stays
 * alive for tasks that outlive @ref performWriteOperation after cancellation
 * (a task must never reference these members from the function's stack).
 *
 * Neither copyable nor movable, because it contains atomics, a mutex and a
 * condition variable.
 */
struct WriteSync {
	std::stop_source cancellation;
	std::atomic<std::size_t>     completedTasks{0};
	std::atomic<std::size_t>     finishedTasks{0};   // success OR failure
	std::mutex              mutex;
	std::condition_variable cv;
};

} // extern "C++"
