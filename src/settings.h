// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SETTINGS_H
#define SETTINGS_H

// C++ Standard Library Headers
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <iostream>
#include <unordered_set>

/**
 * DATA STRUCTURES
 */

/**
 * @struct ConfigEntry
 * @brief Metadata descriptor for a single configuration setting.
 */
struct ConfigEntry {
    std::string key;
    std::string defaultValue;
    std::string comment;
    std::string section;
    std::function<bool(const std::string&)> validate;
};

/**
 * VALIDATION HELPERS & UTILITIES
 */

// Validates boolean-like on/off configuration values.
inline auto isOnOff = [](const std::string& v) { 
    return v == "on" || v == "off"; 
};

// Validates display mode configuration values.
inline auto isDisplay = [](const std::string& v) { 
    return v == "full" || v == "compact"; 
};

/**
 * @brief Validates numeric configuration values within a specified range.
 */
inline auto isNum = [](const std::string& v, int min, int max) -> bool {
    try { 
        size_t pos;
        int n = std::stoi(v, &pos); 
        if (pos != v.length()) return false;
        return n >= min && n <= max; 
    } catch (...) { 
        return false; 
    }
};

/**
 * @brief Removes leading and trailing whitespace from a string.
 */
inline std::string trim(std::string str) {
    if (str.empty()) return str;
    str.erase(0, str.find_first_not_of(" \t"));
    size_t last = str.find_last_not_of(" \t");
    if (last != std::string::npos) str.erase(last + 1);
    return str;
}

/**
 * DISK I/O & ERROR REPORTING
 */

inline void printConfigError(const std::string& configPath) {
    auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
    std::cerr << "\n" << error
              << "Error: Unable to access configuration file: "
              << warning << "'" << configPath << "'"
              << error << ".\033[J\n" << reset;
}

// Forward declaration for the actual writing logic
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config);

/**
 * @brief Commits the current in-memory configuration cache to disk.
 */
inline bool flushCache(const std::string& configPath) {
    if (writeConfig(configPath, GlobalCaches::g_configCache)) return true;
    printConfigError(configPath);
    return false;
}

/**
 * INTERACTIVE EDITOR & APPLICATION LOGIC
 */

void interactiveConfigEditor(const std::string& configPath);
bool editSetting(const std::string& configPath, const std::string& key);
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap);

/**
 * CONFIGURATION DEFINITIONS
 * Static list of all supported settings, defaults, and sections.
 */

inline const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    // --- Theme Settings ---
    {
        "skin", "white",
        "Menu accent color (green/cyan/white/purple/amber/rose)",
        "Theme Settings",
        [](const std::string& v) { 
            return v == "green" || v == "cyan" || v == "white" || 
                   v == "purple" || v == "amber" || v == "rose"; 
        }
    },
    {
        "theme", "original",
        "List and prompt color theme (original/classic/high_contrast/neon/ocean/sunset/forest/midnight/mono/retro/crimson/dracula/tokyo)",
        "",
        [](const std::string& v) {
            static const std::unordered_set<std::string> valid = {
                "original", "classic", "high_contrast", "neon", "ocean", "tokyo",
                "sunset", "forest", "midnight", "mono", "retro", "crimson", "dracula"
            };
            return valid.count(v) > 0;
        }
    },

    // --- General Settings ---
    { "auto_update", "off", "Enable background metadata updates (on/off)", "General Settings", isOnOff },
    { "filenames_only", "off", "Display only filenames instead of full paths (on/off)", "", isOnOff },
    { "pagination", "25", "Items per page (0 to disable)", "", [](const std::string& v) { return isNum(v, 0, 1000); } },

    // --- History Settings ---
    { "folder_path_history_lines", "30", "Max unique FolderPaths to persist", "History Settings", [](const std::string& v) { return isNum(v, 1, 5000); } },
    { "filter_history_lines", "15", "Max unique FilterTerms to persist", "", [](const std::string& v) { return isNum(v, 1, 1000); } },

    // --- Display Modes ---
    { "mount_list", "compact", "Display mode for mount operations", "Display Modes", isDisplay },
    { "umount_list", "full", "Display mode for unmount operations", "", isDisplay },
    { "cp_mv_rm_list", "full", "Display mode for file operations", "", isDisplay },
    { "write2usb_list", "compact", "Display mode for write2usb operations", "", isDisplay },
    { "convert2iso_lists", "compact", "Display mode for convert2iso operations", "", isDisplay },

    // --- Thread Configuration ---
    { "combined_thread_cap", "32", "Global thread pool limit", "Thread Configuration", [](const std::string& v) { return isNum(v, 1, 256); } },
    { "thread_cap_for_mount", "16", "Max concurrent mounting tasks", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_umount", "32", "Max concurrent unmounting tasks", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_cp_mv", "8", "Max concurrent copy/move tasks", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_rm", "32", "Max concurrent removal tasks", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_convert2iso", "8", "Max concurrent ISO conversions", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_database_cleanup", "16", "Max threads for DB cleanup", "", [](const std::string& v) { return isNum(v, 1, 128); } },
    { "thread_cap_for_list_sorting", "4", "Max threads for UI sorting", "", [](const std::string& v) { return isNum(v, 1, 64); } },
    { "thread_cap_for_list_filtering", "4", "Max threads for UI filtering", "", [](const std::string& v) { return isNum(v, 1, 64); } }
};

#endif // SETTINGS_H
