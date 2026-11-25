// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.lists;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/lists ----

namespace GlobalState {

	// Global vector state
	extern std::vector<std::string> globalIsoFileList;
	extern std::vector<std::string> binImgFilesCache;
	extern std::vector<std::string> mdfMdsFilesCache;
	extern std::vector<std::string> nrgFilesCache;
	extern std::vector<std::string> chdFilesCache;
	extern std::vector<std::string> daaGbiFilesCache;

} // namespace GlobalState

} // extern "C++"
