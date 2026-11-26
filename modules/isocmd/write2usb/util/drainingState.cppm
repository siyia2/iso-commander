// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.drainingState;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/drainingState ----

// ---------- Used exclusively for write2usb devices ------------

/**
 * @brief A thread-safe manager for tracking and joining background tasks.
 * * This structure acts as a registry for detached-style background threads,
 * allowing the application to track them and ensure they are joined gracefully
 * during the application's shutdown sequence. This prevents crashes related
 * to global object destruction while background threads are still active.
 */
struct DrainingThreadManager {
	std::vector<std::thread> threads; ///< Internal storage for registered threads.
	std::mutex mtx;                  ///< Mutex to ensure thread-safe registration and access.

	/**
	 * @brief Adds a thread to the manager for lifecycle tracking.
	 * * @param t An rvalue reference to the std::thread to be managed.
	 * The manager takes ownership of the thread.
	 */
	void add(std::thread&& t);

	/**
	 * @brief Blocks until all registered threads have completed their execution.
	 * * Iterates through the registered threads, verifies they are joinable,
	 * and performs a join operation. The internal vector is cleared
	 * upon completion to prevent duplicate joins.
	 */
	void joinAll();

	// The registry lock is separate from the thread-list lock: joinAll() may
	// wait for a drain which must be able to release its reservation.
	void reserveDevice(const std::string& device);
	void releaseDevice(const std::string& device);
	bool isDeviceDraining(const std::string& device);

private:
	std::mutex devicesMutex;
	std::multiset<std::string> drainingDevices;
};

// Global instances
extern DrainingThreadManager g_drainingManager;
inline std::atomic<bool> g_drainingCancelled{false};

// ---------- End of write2usb device draining manager ------------

} // extern "C++"
