// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.write2usb.util.drainingState;

DrainingThreadManager g_drainingManager;

void DrainingThreadManager::add(std::thread&& t) {

	std::lock_guard<std::mutex> lock(mtx);
	threads.emplace_back(std::move(t));

}

void DrainingThreadManager::joinAll() {

	std::lock_guard<std::mutex> lock(mtx);
	for (auto& t : threads) {
		if (t.joinable()) {
			t.join();
		}
	}
	threads.clear();

}

void DrainingThreadManager::reserveDevice(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	drainingDevices.insert(device);
	g_drainingCancelled.store(true, std::memory_order_release);

}

void DrainingThreadManager::releaseDevice(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	const auto it = drainingDevices.find(device);
	if (it != drainingDevices.end()) drainingDevices.erase(it);
	g_drainingCancelled.store(!drainingDevices.empty(), std::memory_order_release);

}

bool DrainingThreadManager::isDeviceDraining(const std::string& device) {

	std::lock_guard lock(devicesMutex);
	return drainingDevices.contains(device);

}
