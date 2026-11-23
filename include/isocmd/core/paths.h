// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CORE_PATHS_H
#define CORE_PATHS_H

// C++ Standard Library Headers
#include <cstdint>
#include <cstdlib>
#include <string>

namespace GlobalState {

    // File Paths
    inline const std::string databaseDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/share/isocmd/database/";
    inline const std::string databaseFilename  = "iso_commander_database.txt";
    inline const std::string databaseFilePath  = databaseDirectory + databaseFilename;
    inline const std::string historyFilePath   = databaseDirectory + "iso_commander_path_database.txt";
    inline const std::string filterHistoryFilePath = databaseDirectory + "iso_commander_filter_database.txt";

    inline const std::string configDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/isocmd/";
    inline const std::string configPath = configDirectory + "config";

    // Configuration Limits
    inline int MAX_HISTORY_LINES         = 30;
    inline int MAX_HISTORY_PATTERN_LINES = 15;

    inline constexpr std::uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

} // namespace GlobalState

#endif // CORE_PATHS_H
