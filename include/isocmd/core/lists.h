// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LISTS_H
#define LISTS_H

// C++ Standard Library Headers
#include <string>
#include <vector>

namespace GlobalState {

    // Global vector state
    inline std::vector<std::string> globalIsoFileList;
    inline std::vector<std::string> binImgFilesCache;
    inline std::vector<std::string> mdfMdsFilesCache;
    inline std::vector<std::string> nrgFilesCache;
    inline std::vector<std::string> chdFilesCache;
    inline std::vector<std::string> daaGbiFilesCache;

} // namespace GlobalState

#endif // LISTS_H
