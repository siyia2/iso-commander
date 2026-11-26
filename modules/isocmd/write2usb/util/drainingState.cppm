// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.drainingState;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// ---------- Used exclusively for write2usb devices ------------

/**
 * @brief Tracks unfinished USB writes and their device reservations.
 *
 * Cancelled operations may still have outstanding I/O. Drain threads finish
 * cleanup in the background, and joinAll() waits for them before shutdown.
 */
struct DrainingThreadManager {
	std::vector<std::thread> threads; ///< Internal storage for registered threads.
	std::mutex mtx;                  ///< Mutex to ensure thread-safe registration and access.

	/// Takes ownership of a drain thread for later joining.
	void add(std::thread&& t);

	/// Waits for all registered drain threads and clears the thread list.
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


extern DrainingThreadManager g_drainingManager;
inline std::atomic<bool> g_drainingCancelled{false};

// ---------- End of write2usb device draining manager ------------

} // extern "C++"
