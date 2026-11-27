// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.globalMutexes;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

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
	inline std::mutex editorMutex;
}

} // extern "C++"
