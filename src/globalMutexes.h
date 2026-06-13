// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GLOBALMUTEXES_H
#define GLOBALMUTEXES_H

// C++ Standard Library Headers
#include <mutex>

//=======================================
//          GLOBAL  MUTEXES
//=======================================
namespace GlobalMutexes {

    // Mutex Protection For Verbose Sets
    inline std::mutex globalSetsMutex;

    // Mutex Protection For file counts in search.cpp
    inline std::mutex couNtMutex;

    // --- Global State Mutexes ---
    inline std::mutex updateListMutex;
    inline std::mutex binImgCacheMutex;
    inline std::mutex mdfMdsCacheMutex;
    inline std::mutex nrgCacheMutex;
    inline std::mutex chdCacheMutex;
    inline std::mutex daaGbiCacheMutex;
}

#endif // GLOBALMUTEXES_H
