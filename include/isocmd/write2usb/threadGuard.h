// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREADGUARD_H
#define THREADGUARD_H

// C++ Standard Library Headers
#include <atomic>
#include <thread>

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

#endif // THREADGUARD_H
