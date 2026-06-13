// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SETTINGS_H
#define SETTINGS_H

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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
 * @namespace ConfigCaches
 * @brief Global storage for configuration state and file system paths.
 */
namespace ConfigCaches {
    inline std::map<std::string, std::string> g_configCache;
	inline std::string g_cachedPath;
}

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
 * @brief Validates that a string represents a plain non-negative integer within [min, max].
 * Rejects leading zeros, signs, decimals, or any non-digit characters.
 */
inline auto isNum = [](const std::string& v, int min, int max) -> bool {
    if (v.empty() || !std::all_of(v.begin(), v.end(), ::isdigit)) return false;
    if (v.size() > 1 && v[0] == '0') return false;
    try {
        int n = std::stoi(v);
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

// Forward declaration for the actual writing logic
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config);


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
        "skin",                                     // key
        "white",                                    // defaultValue
        "Menu and header accent color (green/cyan/white/purple/amber/rose/gray)",  // comment
        "Theme Settings",                           // section
        [](const std::string& v) {                  // validate
            return v == "green" || v == "cyan" || v == "white" ||
                   v == "purple" || v == "amber" || v == "rose" || v == "gray";
        }
    },
    {
        "theme",
        "original",
        "List and prompt color theme (original/classic/high_contrast/neon/ocean/sunset/forest/\n#                                midnight/mono/retro/crimson/dracula/tokyo/paper/sakura)",
        "",
        [](const std::string& v) {
            static const std::unordered_set<std::string> valid = {
                "original", "classic", "high_contrast", "neon", "ocean", "tokyo", "paper",
                "sunset", "forest", "midnight", "mono", "retro", "crimson", "dracula" , "sakura"
            };
            return valid.count(v) > 0;
        }
    },

    // --- General Settings ---
    {
        "auto_update",
        "off",
        "Enable background metadata updates from folder path history on startup (on/off)",
        "General Settings",
        isOnOff
    },
    {
        "filenames_only",
        "off",
        "Display only filenames instead of full paths (on/off)",
        "",
        isOnOff
    },
    {
        "pagination",
        "25",
        "Items per page in list view (0 to disable)",
        "",
        [](const std::string& v) { return isNum(v, 0, 1000); }
    },

    // --- History Settings ---
    {
        "folder_path_history_lines",
        "30",
        "Max unique FolderPaths to persist in history (0 to disable)",
        "History Settings",
        [](const std::string& v) { return isNum(v, 0, 5000); }
    },
    {
        "filter_history_lines",
        "15",
        "Max unique FilterTerms to persist in history (0 to disable)",
        "",
        [](const std::string& v) { return isNum(v, 0, 1000); }
    },

    // --- Display Modes ---
    {
        "mount_list",
        "compact",
        "Display mode for mount operations (full/compact)",
        "Display Modes",
        isDisplay
    },
    {
        "umount_list",
        "full",
        "Display mode for unmount operations (full/compact)",
        "",
        isDisplay
    },
    {
        "cp_mv_rm_list",
        "full",
        "Display mode for file operations (full/compact)",
        "",
        isDisplay
    },
    {
        "write2usb_list",
        "compact",
        "Display mode for write2usb operations (full/compact)",
        "",
        isDisplay
    },
    {
        "convert2iso_lists",
        "compact",
        "Display mode for convert2iso operations (full/compact)",
        "",
        isDisplay
    },

    // --- Thread Configuration ---
    {
        "combined_thread_cap",
        "16",
        "Global thread pool size limit (requires restart to apply)",
        "Thread Configuration",
        [](const std::string& v) { return isNum(v, 1, 256); }
    },
    {
        "thread_cap_for_mount",
        "8",
        "Max concurrent mounting tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_umount",
        "8",
        "Max concurrent unmounting tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_cp_mv",
        "4",
        "Max concurrent copy/move tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_rm",
        "8",
        "Max concurrent removal tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_convert2iso",
        "4",
        "Max concurrent ISO conversions within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_database_cleanup",
        "4",
        "Max concurrent threads for ISO DB cleanup within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_list_sorting",
        "2",
        "Max concurrent threads for UI list sorting within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 64); }
    },
    {
        "thread_cap_for_list_filtering",
        "2",
        "Max concurrent threads for UI list filtering within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 64); }
    },
};

#endif // SETTINGS_H
