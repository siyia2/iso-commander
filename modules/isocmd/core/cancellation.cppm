// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.cancellation;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

namespace GlobalState {

	/// Set (e.g. by the SIGINT handler) to ask long-running operations to stop.
	inline std::atomic<bool> g_operationCancelled{false};

} // namespace GlobalState

} // extern "C++"
