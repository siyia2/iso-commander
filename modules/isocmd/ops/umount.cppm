// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ops.umount;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ops/umount ----

void unmountISO(const std::vector<std::string>& isoDirs, std::atomic<std::size_t>* completedTasks, std::atomic<std::size_t>* failedTasks, bool silentMode);

} // extern "C++"
