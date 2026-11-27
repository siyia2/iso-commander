// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.globalMutexes;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

//=======================================
//          GLOBAL  MUTEXES
//=======================================
namespace GlobalMutexes {

	// Protects verbose result sets.
	inline std::mutex globalSetsMutex;

	// Protects file counts in searchImage.cpp and searchIso.cpp.
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
