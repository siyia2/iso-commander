// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.paths;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/paths ----

namespace GlobalState {

    // File Paths
    inline const std::string databaseDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "").append("/.local/share/isocmd/database/");
    inline const std::string databaseFilename  = "iso_commander_database.txt";
    inline const std::string databaseFilePath  = std::string(databaseDirectory).append(databaseFilename);
    inline const std::string historyFilePath   = std::string(databaseDirectory).append("iso_commander_path_database.txt");
    inline const std::string filterHistoryFilePath = std::string(databaseDirectory).append("iso_commander_filter_database.txt");

    inline const std::string configDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "").append("/.config/isocmd/");
    inline const std::string configPath = std::string(configDirectory).append("config");

    // Configuration Limits
    inline int MAX_HISTORY_LINES         = 30;
    inline int MAX_HISTORY_PATTERN_LINES = 15;

    inline constexpr std::uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

} // namespace GlobalState

} // extern "C++"
