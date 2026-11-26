// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ops.mount;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

void mountIsoFiles(const std::vector<std::string>& isoFiles, std::atomic<std::size_t>* completedTasks, std::atomic<std::size_t>* failedTasks, bool silentMode);

} // extern "C++"
