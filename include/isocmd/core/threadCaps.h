// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREADCAPS_H
#define THREADCAPS_H

// C++ Standard Library Headers
#include <cstddef>

//=======================================
//     GLOBAL CONCURRENCY SETTINGS
//=======================================
namespace GlobalThreadCaps {

    // Global cap for static threads
    inline std::size_t MAX_USEFUL_THREADS = 16;

    // Operation thread caps for static pool

    // High I/O
    inline std::size_t CPMV_THREAD_CAP   = 4;
    inline std::size_t CONV_THREAD_CAP   = 4;

    // Moderate I/O
    inline std::size_t MOUNT_THREAD_CAP  = 8;
    inline std::size_t CLEAN_THREAD_CAP  = 4;

    // Low I/O
    inline std::size_t UMOUNT_THREAD_CAP = 8;
    inline std::size_t RM_THREAD_CAP     = 8;

    // Low I/O but fast
    inline std::size_t SORT_THREAD_CAP   = 2;
    inline std::size_t FILTER_THREAD_CAP = 2;

}

#endif // THREADCAPS_H
