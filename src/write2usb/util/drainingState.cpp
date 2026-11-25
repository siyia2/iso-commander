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
