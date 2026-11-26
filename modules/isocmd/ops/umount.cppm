// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ops.umount;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

void unmountISO(const std::vector<std::string>& isoDirs, std::atomic<std::size_t>* completedTasks, std::atomic<std::size_t>* failedTasks, bool silentMode);

} // extern "C++"
