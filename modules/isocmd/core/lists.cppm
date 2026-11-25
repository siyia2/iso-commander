// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.lists;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/lists ----

namespace GlobalState {

    // Global vector state
    inline std::vector<std::string> globalIsoFileList;
    inline std::vector<std::string> binImgFilesCache;
    inline std::vector<std::string> mdfMdsFilesCache;
    inline std::vector<std::string> nrgFilesCache;
    inline std::vector<std::string> chdFilesCache;
    inline std::vector<std::string> daaGbiFilesCache;

} // namespace GlobalState

} // extern "C++"
