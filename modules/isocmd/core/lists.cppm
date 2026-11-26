// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.lists;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

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
