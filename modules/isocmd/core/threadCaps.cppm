// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.threadCaps;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/threadCaps ----

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

} // extern "C++"
