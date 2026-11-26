// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.threadGuard;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @struct ThreadGuard
 * @brief scope-bound wrapper that stops and joins a background thread on destruction.
 */
struct ThreadGuard {
	std::thread&       thread;
	std::atomic<bool>& active;

	ThreadGuard(std::thread& t, std::atomic<bool>& a) : thread(t), active(a) {}

	ThreadGuard(const ThreadGuard&)            = delete;
	ThreadGuard& operator=(const ThreadGuard&) = delete;

	~ThreadGuard() {

		stop();

	}

	void stop() {

		active.store(false);
		if (thread.joinable()) thread.join();

	}
};

} // extern "C++"
