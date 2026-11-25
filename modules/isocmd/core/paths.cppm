// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.paths;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/paths ----

namespace GlobalState {

    // File Paths
    extern const std::string databaseDirectory;
    extern const std::string databaseFilename;
    extern const std::string databaseFilePath;
    extern const std::string historyFilePath;
    extern const std::string filterHistoryFilePath;

    extern const std::string configDirectory;
    extern const std::string configPath;

    // Configuration Limits
    inline int MAX_HISTORY_LINES         = 30;
    inline int MAX_HISTORY_PATTERN_LINES = 15;

    inline constexpr std::uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

} // namespace GlobalState

} // extern "C++"
