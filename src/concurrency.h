// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONCURRENCY_H
#define CONCURRENCY_H

#include <thread>
#include <algorithm>
#include <mutex>

//=======================================
// GLOBAL CONCURRENCY SETTINGS AND MUTEX
//=======================================
namespace GlobalConcurrency {

    // Thread Management
    inline unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());

    // Global cap for static threads
    inline size_t MAX_USEFUL_THREADS = 32;

    // Operation thread caps for static pool
    
    // High I/O
    inline size_t CPMV_THREAD_CAP   = 8;
    inline size_t CONV_THREAD_CAP   = 8;

    // Moderate I/O
    inline size_t MOUNT_THREAD_CAP  = 16;
    inline size_t CLEAN_THREAD_CAP  = 16;

    // Low I/O
    inline size_t UMOUNT_THREAD_CAP = 32;
    inline size_t RM_THREAD_CAP     = 32;

    // Low I/O but fast
    inline size_t SORT_THREAD_CAP   = 4;
    inline size_t FILTER_THREAD_CAP = 4;

    // Mutex Protection For Verbose Sets
    inline std::mutex globalSetsMutex;

} // namespace GlobalConcurrency

#endif // CONCURRENCY_H
