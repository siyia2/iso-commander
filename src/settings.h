// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file settings.h
 * @brief Configuration management system for ISO Commander.
 * 
 * This header defines all configuration structures, validation helpers,
 * default settings, and core configuration I/O utilities used throughout
 * the application. Settings are stored in a simple key=value format with
 * support for sections, comments, and automatic validation.
 */

#include "./headers.h"

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @struct ConfigEntry
 * @brief Metadata descriptor for a single configuration setting.
 * 
 * Each ConfigEntry defines a complete specification for one configuration
 * key, including its default value, human-readable description, optional
 * section grouping, and validation logic to ensure data integrity.
 */
struct ConfigEntry {
    std::string key;
    std::string defaultValue;
    std::string comment;
    std::string section;
    std::function<bool(const std::string&)> validate;
};

// ============================================================================
// Validation Helpers
// ============================================================================
// These lightweight lambda validators are used by ConfigEntry definitions
// to ensure configuration values conform to expected formats and ranges.

/**
 * @brief Validates boolean-like on/off configuration values.
 * @param v The string value to validate.
 * @return true if value is "on" or "off", false otherwise.
 */
inline auto isOnOff = [](const std::string& v) { 
    return v == "on" || v == "off"; 
};

/**
 * @brief Validates display mode configuration values.
 * @param v The string value to validate.
 * @return true if value is "full" or "compact", false otherwise.
 */
inline auto isDisplay = [](const std::string& v) { 
    return v == "full" || v == "compact"; 
};

/**
 * @brief Validates numeric configuration values within a specified range.
 * 
 * Performs strict validation ensuring the entire string represents a valid
 * integer within the inclusive [min, max] range. Rejects values with
 * trailing non-numeric characters or out-of-range numbers.
 * 
 * @param v   The string value to validate.
 * @param min Minimum acceptable value (inclusive).
 * @param max Maximum acceptable value (inclusive).
 * @return true if value is a valid integer within range, false otherwise.
 */
inline auto isNum = [](const std::string& v, int min, int max) -> bool {
    try { 
        size_t pos;
        int n = std::stoi(v, &pos); 
        if (pos != v.length()) return false;  // Reject trailing characters
        return n >= min && n <= max; 
    } catch (...) { 
        return false;  // Handle non-numeric or overflow values
    }
};

// ============================================================================
// String Utilities
// ============================================================================

/**
 * @brief Removes leading and trailing whitespace from a string.
 * 
 * Strips spaces and tab characters from both ends of the input string.
 * Returns the input unchanged if it's empty or contains no whitespace.
 * 
 * @param str The string to trim (taken by value for modification).
 * @return A new string with leading/trailing whitespace removed.
 */
inline std::string trim(std::string str) {
    if (str.empty()) return str;
    str.erase(0, str.find_first_not_of(" \t"));
    size_t last = str.find_last_not_of(" \t");
    if (last != std::string::npos) str.erase(last + 1);
    return str;
}

// ============================================================================
// Error Reporting
// ============================================================================

/**
 * @brief Displays a standardized configuration file access error message.
 * 
 * Uses the current theme colors to output a formatted error message
 * to stderr indicating that the configuration file could not be accessed.
 * 
 * @param configPath The filesystem path to the inaccessible config file.
 */
inline void printConfigError(const std::string& configPath) {
    auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
    std::cerr << "\n" << error
              << "Error: Unable to access configuration file: "
              << warning << "'" << configPath << "'"
              << error << ".\033[J\n" << reset;
}

// ============================================================================
// Disk I/O Operations
// ============================================================================

/**
 * @brief Commits the current in-memory configuration cache to disk.
 * 
 * Attempts to write the current state of g_configCache to the specified
 * configuration file. On failure, displays an error message via
 * printConfigError().
 * 
 * @param configPath Filesystem path to the configuration file.
 * @return true if the write operation succeeded, false otherwise.
 */
inline bool flushCache(const std::string& configPath) {
    if (writeConfig(configPath, g_configCache)) return true;
    printConfigError(configPath);
    return false;
}

// ============================================================================
// Configuration Definitions
// ============================================================================

/**
 * @brief Canonical ordered list of all supported configuration settings.
 * 
 * This vector defines every configuration key recognized by the application,
 * organized by section with default values, descriptions, and validation
 * logic. It serves as both documentation and the authoritative source for
 * generating, validating, and self-healing configuration files.
 * 
 * Settings are grouped into the following sections:
 *   - Theme Settings: Visual appearance (skin colors, list themes)
 *   - General Settings: Core behavior (auto-update, display preferences)
 *   - History Settings: History persistence limits
 *   - Display Modes: List view preferences per operation type
 *   - Thread Configuration: Concurrency limits for various operations
 */
inline const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    // --- Theme Settings ---
    {
        "skin",                                     // key
        "white",                                    // defaultValue
        "Menu accent color (green/cyan/white/purple/amber/rose)",  // comment
        "Theme Settings",                           // section
        [](const std::string& v) {                  // validate
            return v == "green" || v == "cyan" || v == "white" || 
                   v == "purple" || v == "amber" || v == "rose"; 
        }
    },
    {
        "theme",
        "original",
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
        "Max unique FolderPaths to persist in history",
        "History Settings",
        [](const std::string& v) { return isNum(v, 1, 5000); }
    },
    {
        "filter_history_lines",
        "15",
        "Max unique FilterTerms to persist in history",
        "",
        [](const std::string& v) { return isNum(v, 1, 1000); }
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
        "32",
        "Global thread pool limit; excess tasks are queued; changes take effect on restart",
        "Thread Configuration",
        [](const std::string& v) { return isNum(v, 1, 256); }
    },
    {
        "thread_cap_for_mount",
        "16",
        "Max concurrent mounting tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_umount",
        "32",
        "Max concurrent unmounting tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_cp_mv",
        "8",
        "Max concurrent copy/move tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_rm",
        "32",
        "Max concurrent removal tasks within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_convert2iso",
        "8",
        "Max concurrent ISO conversions within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_database_cleanup",
        "16",
        "Max concurrent threads for ISO DB cleanup within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 128); }
    },
    {
        "thread_cap_for_list_sorting",
        "4",
        "Max concurrent threads for UI list sorting within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 64); }
    },
    {
        "thread_cap_for_list_filtering",
        "4",
        "Max concurrent threads for UI list filtering within the global pool",
        "",
        [](const std::string& v) { return isNum(v, 1, 64); }
    },
};

// ============================================================================
// Function Declarations
// ============================================================================

/**
 * @brief Launches the interactive configuration editor interface.
 * 
 * Provides a user-friendly terminal-based menu for viewing and modifying
 * all configuration settings with real-time validation and immediate
 * application of changes.
 * 
 * @param configPath Filesystem path to the configuration file.
 */
void interactiveConfigEditor(const std::string& configPath);

/**
 * @brief Edits a single configuration setting interactively.
 * 
 * Prompts the user to modify a specific setting, displaying its current
 * value, description, and valid options. Validates input before applying
 * the change and provides immediate visual feedback.
 * 
 * @param configPath Filesystem path to the configuration file.
 * @param key        The configuration key to edit.
 */
bool editSetting(const std::string& configPath,const std::string& key);

/**
 * @brief Applies thread pool and history limit settings to global variables.
 * 
 * Reads thread capacity and history line limit values from the configuration
 * map and updates the corresponding global variables used throughout the
 * application to control concurrency and history behavior.
 * 
 * @param configMap The current configuration key-value map.
 */
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap);
