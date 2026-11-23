// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DRAININGSTATE_H
#define DRAININGSTATE_H

// C++ Standard Library Headers
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ---------- Used exclusively for the [FLUSHING] device indicator ------------

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
};

// Global instances
inline DrainingThreadManager g_drainingManager;
inline std::mutex g_drainingDevicesMutex;
inline std::unordered_set<std::string> g_drainingDevices;
inline std::atomic<bool> g_drainingCancelled{false};

// ---------- End of [FLUSHING] device indicator block ------------

#endif // DRAININGSTATE_H
