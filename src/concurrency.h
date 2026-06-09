// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONCURRENCY_H
#define CONCURRENCY_H

// C++ Standard Library Headers
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <thread>

//=======================================
// GLOBAL CONCURRENCY SETTINGS AND MUTEX
//=======================================
namespace GlobalConcurrency {

    // Thread Management
    inline unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());

    // Global cap for static threads
    inline size_t MAX_USEFUL_THREADS = 16;

    // Operation thread caps for static pool

    // High I/O
    inline size_t CPMV_THREAD_CAP   = 4;
    inline size_t CONV_THREAD_CAP   = 4;

    // Moderate I/O
    inline size_t MOUNT_THREAD_CAP  = 8;
    inline size_t CLEAN_THREAD_CAP  = 4;

    // Low I/O
    inline size_t UMOUNT_THREAD_CAP = 8;
    inline size_t RM_THREAD_CAP     = 8;

    // Low I/O but fast
    inline size_t SORT_THREAD_CAP   = 2;
    inline size_t FILTER_THREAD_CAP = 2;

    // Mutex Protection For Verbose Sets
    inline std::mutex globalSetsMutex;

    // Mutex Protection For file counts in search.cpp
    inline std::mutex couNtMutex;

    // Mutex protection for file traversal results and error messages
    inline std::mutex traverseFilesMutex;
    inline std::mutex traverseErrorsMutex;

    // Ensures the traversal cancellation message is inserted only once across all threads
    inline std::atomic<bool> g_CancelledMessageAdded{false};

} // namespace GlobalConcurrency

#endif // CONCURRENCY_H
