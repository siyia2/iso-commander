// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Cooperative shutdown guard for USB worker threads.
 */

export module isocmd.write2usb.util.threadGuard;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @struct ThreadGuard
 * @brief Stops and joins a background thread on scope exit.
 *
 * Borrows the thread and its cooperative run flag; both must outlive the guard.
 * The worker must observe the flag and eventually return. This guard does not
 * interrupt blocking I/O or detach the thread.
 */
struct ThreadGuard {
	std::thread&       thread; ///< Borrowed worker thread.
	std::atomic<bool>& active; ///< Borrowed cooperative run flag.

	/**
	 * @brief Associates the guard with a worker and its run flag.
	 * @param t Thread to join when stopped.
	 * @param a Flag cleared to request worker shutdown.
	 */
	ThreadGuard(std::thread& t, std::atomic<bool>& a) : thread(t), active(a) {}

	ThreadGuard(const ThreadGuard&)            = delete;
	ThreadGuard& operator=(const ThreadGuard&) = delete;

	/// Requests shutdown and joins the worker if it is still joinable.
	~ThreadGuard() {

		stop();

	}

	/**
	 * @brief Clears the run flag and waits for the worker to exit.
	 * @note May be called again after a successful join; no join is then needed.
	 *       Must be called from a thread other than the guarded worker.
	 */
	void stop() {

		active.store(false);
		if (thread.joinable()) thread.join();

	}
};

} // extern "C++"
