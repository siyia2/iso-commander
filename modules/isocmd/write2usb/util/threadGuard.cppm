// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.threadGuard;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/threadGuard ----

/**
 * @struct ThreadGuard
 * @brief RAII wrapper that stops and joins a background thread on destruction.
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
