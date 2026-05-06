// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GLOBALS_H
#define GLOBALS_H

// C++ Standard Library Headers
#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>


//==============================
// GLOBAL APPLICATION STATE
//==============================
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
    inline int MAX_HISTORY_LINES         = 100;
    inline int MAX_HISTORY_PATTERN_LINES = 50;
    
    inline constexpr uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

    // State Management
    inline std::atomic<bool> isoListDirty{true}; 
    inline std::atomic<bool> g_operationCancelled{false};
    inline bool needSortingAfterflno      = false;
    inline size_t ITEMS_PER_PAGE          = 25;
    inline int lockFileDescriptor         = -1;

} // namespace GlobalState

#endif // GLOBALS_H
