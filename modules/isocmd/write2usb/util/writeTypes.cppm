// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief USB task identity, settings, cancellation and progress synchronization types.
 */

export module isocmd.write2usb.util.writeTypes;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @brief Checks immediate or latched cancellation of a USB write.
 * @param token Batch stop token; an empty token checks only the global flag.
 * @return true if the global cancellation flag or the token requests a stop.
 * @note The UI latches cancellation in the batch stop state before resetting the
 *       global flag, so outstanding workers remain cancelled after UI recovery.
 */
bool isUsbWriteCancelled(const std::stop_token& token = {}) noexcept;

/// Information about an ISO image file.
struct IsoInfo {
	std::string path;          ///< Full filesystem path to the ISO file.
	std::string filename;      ///< Filename without the directory path.
	std::uint64_t size;        ///< ISO size in bytes.
	std::string sizeStr;       ///< Human-readable size (e.g., "4.20 GB").
};

/**
 * @brief Holds one USB task's identity, settings and live progress telemetry.
 *
 * Identity and settings are initialized before dispatch; only atomic fields
 * are updated while workers and the UI share a row. Moving a row transfers
 * metadata and loads the atomic values individually, not as a coherent snapshot.
 * Rows must remain at stable addresses while workers use them.
 */
struct ProgressInfo {
	std::string filename;   ///< Display filename of the source image.
	std::string device;     ///< Target device path.
	std::string totalSize;  ///< Formatted source image size.
	std::string sourcePath; ///< Full source image path.

	std::stop_token cancelToken;        ///< Shared batch token; not reset after dispatch.
	std::optional<bool> bufferedWrites; ///< Settings snapshot; unset for standalone callers.
	bool verifyRequested = false;       ///< Readback requested; immutable during execution.

	std::atomic<bool> verifying{false};          ///< Readback started; retained in terminal states.
	std::atomic<std::uint64_t> bytesVerified{0}; ///< Bytes read back, not necessarily CRC-matched.
	std::atomic<bool> writeCompleted{false};     ///< Write and final flush succeeded.
	std::atomic<bool> completed{false};          ///< Write and any requested verification succeeded.
	std::atomic<bool> failed{false};             ///< A task failure was reported.
	std::atomic<std::uint64_t> bytesWritten{0};  ///< Writer-reported cumulative bytes.
	std::atomic<int> progress{0};                ///< Current write or readback percentage.
	std::atomic<double> speed{0.0};              ///< Live write speed, then write-only average (MiB/s).
	std::atomic<double> verificationSpeed{0.0};  ///< Live readback speed in MiB/s.

	/**
	 * @brief Creates a progress row with zeroed counters and unset result flags.
	 * @param filename Display name of the source image.
	 * @param device Target device path.
	 * @param totalSize Formatted source image size.
	 * @param sourcePath Full path of the source image.
	 */
	ProgressInfo(std::string filename, std::string device, std::string totalSize,
	             std::string sourcePath);

	/// Moves metadata and copies current atomic values by individual loads.
	ProgressInfo(ProgressInfo&& other) noexcept;

	/// Replaces metadata and atomic values; not for concurrent use on active rows.
	ProgressInfo& operator=(ProgressInfo&& other) noexcept;

	// Disable copying because atomics are not copyable.
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
	std::stop_source cancellation;              ///< Latched cancellation for this batch.
	std::atomic<std::size_t> completedTasks{0}; ///< Successful tasks.
	std::atomic<std::size_t> finishedTasks{0};  ///< Tasks that have finished or skipped the writer.
	std::mutex mutex;                           ///< Synchronizes condition-variable waits.
	std::condition_variable cv;                 ///< Wakes waiters when task state changes.
};

} // extern "C++"
