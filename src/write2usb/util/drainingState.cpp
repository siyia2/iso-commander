// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Registration, joining and reservation tracking for USB cleanup threads.
 */

import std;

import isocmd.write2usb.util.drainingState;

DrainingThreadManager g_drainingManager;

/// Transfers the thread handle into the protected list.
void DrainingThreadManager::add(std::thread&& t) {

	std::lock_guard<std::mutex> lock(mtx);
	threads.emplace_back(std::move(t));

}

/// Joins registered threads without holding the separate reservation mutex.
void DrainingThreadManager::joinAll() {

	std::lock_guard<std::mutex> lock(mtx);
	for (auto& t : threads) {
		if (t.joinable()) {
			t.join();
		}
	}
	threads.clear();

}

/// Records a reservation before control returns to the USB selection UI.
void DrainingThreadManager::reserveDevice(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	drainingDevices.insert(device);
	g_drainingCancelled.store(true, std::memory_order_release);

}

/// Releases a single reservation and publishes the remaining drain state.
void DrainingThreadManager::releaseDevice(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	const auto it = drainingDevices.find(device);
	if (it != drainingDevices.end()) drainingDevices.erase(it);
	g_drainingCancelled.store(!drainingDevices.empty(), std::memory_order_release);

}

/// Performs a synchronized lookup of the exact device path.
bool DrainingThreadManager::isDeviceDraining(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	return drainingDevices.contains(device);

}
