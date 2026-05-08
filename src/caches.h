// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CACHES_H
#define CACHES_H

// C++ Standard Library Headers
#include <map>
#include <mutex>
#include <unordered_map>

// Project Headers
#include "./stringManipulation.h"

namespace GlobalCaches {
    // Thread-local caches
    inline thread_local std::unordered_map<
        std::string, 
        std::string, 
        StringViewHash, 
        std::equal_to<>
    > transformationCache;
    inline thread_local std::unordered_map<
        std::string, 
        std::tuple<std::string, std::string, std::string>, 
        StringViewHash, 
        std::equal_to<>
    > cachedParsesForUmount;
    
    inline std::map<std::string, std::string> g_configCache;
	inline std::string g_cachedPath;
	
	// --- Mutexes ---
    inline std::mutex updateListMutex;
    inline std::mutex binImgCacheMutex;
    inline std::mutex mdfMdsCacheMutex;
    inline std::mutex nrgCacheMutex;
    inline std::mutex chdCacheMutex;
    inline std::mutex daaGbiCacheMutex;

    // Global vector caches
    inline std::vector<std::string> globalIsoFileList;
    inline std::vector<std::string> binImgFilesCache;
    inline std::vector<std::string> mdfMdsFilesCache;
    inline std::vector<std::string> nrgFilesCache;
    inline std::vector<std::string> chdFilesCache;
    inline std::vector<std::string> daaGbiFilesCache;
}

#endif // CACHES_H
