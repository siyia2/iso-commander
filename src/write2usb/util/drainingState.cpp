// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <utility>

// Project Headers
#include "isocmd/write2usb/drainingState.h"

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
