// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>

export module isocmd.core.threadCaps;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

//=======================================
//     GLOBAL CONCURRENCY SETTINGS
//=======================================
namespace GlobalThreadCaps {

	// Global cap for static threads
	extern size_t MAX_USEFUL_THREADS;

	// Operation thread caps for static pool

	// High I/O
	extern size_t CPMV_THREAD_CAP;
	extern size_t CONV_THREAD_CAP;

	// Moderate I/O
	extern size_t MOUNT_THREAD_CAP;
	extern size_t CLEAN_THREAD_CAP;

	// Low I/O
	extern size_t UMOUNT_THREAD_CAP;
	extern size_t RM_THREAD_CAP;

	// Low I/O but fast
	extern size_t SORT_THREAD_CAP;
	extern size_t FILTER_THREAD_CAP;

}

} // extern "C++"
