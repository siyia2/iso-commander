// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Drain-thread ownership and device reservations after cancellation.
 */

export module isocmd.write2usb.util.drainingState;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @brief Tracks unfinished USB writes and their device reservations.
 *
 * Cancelled operations may still have outstanding I/O. Drain threads finish
 * cleanup in the background, and joinAll() waits for registered threads.
 * Reservations are counted by device path so overlapping reservations cannot
 * release a device prematurely.
 */
struct DrainingThreadManager {
	std::vector<std::thread> threads; ///< Internal storage for registered threads.
	std::mutex mtx;                   ///< Serializes registration and joining of drain threads.

	/// Takes ownership of a drain thread for later joining.
	void add(std::thread&& t);

	/**
	 * @brief Joins all registered drain threads and clears the thread list.
	 * @note Holds the thread-list mutex while joining; drain threads must not
	 *       register or join threads through this manager during that wait.
	 */
	void joinAll();

	// The registry lock is separate from the thread-list lock: joinAll() may
	// wait for a drain which must be able to release its reservation.
	/**
	 * @brief Adds one reservation and marks cancellation cleanup as pending.
	 * @param device Device path used as the reservation key.
	 */
	void reserveDevice(const std::string& device);

	/**
	 * @brief Removes one matching reservation, if present.
	 * @param device Device path used when reserving the target.
	 * @note Clears the global draining flag only when no reservations remain.
	 */
	void releaseDevice(const std::string& device);

	/**
	 * @brief Checks whether the device path has any outstanding reservations.
	 * @param device Device path to look up.
	 * @return true if reserved by this process; false otherwise.
	 * @note Does not query kernel writeback, mounts or other processes.
	 */
	bool isDeviceDraining(const std::string& device);

private:
	std::mutex devicesMutex;                    ///< Protects the reservation registry.
	std::multiset<std::string> drainingDevices; ///< One entry per outstanding reservation.
};

/// Process-wide manager for outstanding USB cleanup tasks.
extern DrainingThreadManager g_drainingManager;

/// Indicates whether any device reservations remain in the manager.
inline std::atomic<bool> g_drainingCancelled{false};

} // extern "C++"
