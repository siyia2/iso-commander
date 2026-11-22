// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SETTINGS_H
#define SETTINGS_H

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/ui/pausePrompts.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/ui/themes.h"
#include "isocmd/util/sort.h"

/**
 * DATA STRUCTURES
 */

/**
 * @struct ConfigEntry
 * @brief Metadata descriptor for a single configuration setting.
 *
 * Enumerated settings carry @c values (the list their validator checks and the
 * settings editor offers for Tab completion and the "Valid values:" hint);
 * numeric settings set @c numeric together with the @c minValue / @c maxValue
 * range their validator enforces. Build entries with @c enumEntry and
 * @c numericEntry so the validator and the editor metadata come from one place.
 */
struct ConfigEntry {
    std::string key;
    std::string defaultValue;
    std::string comment;
    std::string section;
    std::function<bool(const std::string&)> validate;

    const std::vector<std::string>* values = nullptr;  // enumerated settings (static storage)
    bool numeric = false;                              // free-form number
    int  minValue = 0;                                 // numeric range, inclusive
    int  maxValue = 0;
};

/**
 * @brief Result of a single @c editSetting screen.
 */
enum class EditNav { Advance, Updated, Next, Prev, GoTo, Abort, Handled };

/**
 * @namespace ConfigCaches
 * @brief Global storage for configuration state and file system paths.
 */
namespace ConfigCaches {
    inline std::map<std::string, std::string> g_configCache;
    inline std::string g_cachedPath;
}

/**
 * @namespace ConfigValues
 * @brief Single definition of every enumerated setting's allowed values.
 *
 * Keep each list in alphabetical order: the settings editor prints and
 * completes them in this order.
 */
namespace ConfigValues {
    inline const std::vector<std::string> base    = {"ash", "black", "charcoal", "gray", "silver", "steel", "white"};
    inline const std::vector<std::string> skin    = {"amber", "black", "cyan", "gray", "green", "purple", "rose", "white"};
    inline const std::vector<std::string> theme   = {"classic", "crimson", "dracula", "forest", "high_contrast",
                                                     "midnight", "mono", "neon", "ocean", "original", "paper",
                                                     "retro", "sakura", "sunset", "tokyo"};
    inline const std::vector<std::string> onOff   = {"off", "on"};
    inline const std::vector<std::string> display = {"compact", "full"};
}

/**
 * VALIDATION HELPERS & UTILITIES
 */

// True if @p v is one of @p values.
inline bool isOneOf(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

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

/**
 * @brief Builds an enumerated setting whose validator and editor metadata share @p values.
 *
 * @p values must have static storage duration (use the @c ConfigValues lists).
 */
inline ConfigEntry enumEntry(std::string key, std::string def, std::string comment,
                             std::string section, const std::vector<std::string>& values) {
    const std::vector<std::string>* p = &values;
    ConfigEntry e{std::move(key), std::move(def), std::move(comment), std::move(section),
                  [p](const std::string& v) { return isOneOf(*p, v); }};
    e.values = p;
    return e;
}

/**
 * @brief Builds a numeric setting whose validator and editor range hint share [min, max].
 */
inline ConfigEntry numericEntry(std::string key, std::string def, std::string comment,
                                std::string section, int min, int max) {
    ConfigEntry e{std::move(key), std::move(def), std::move(comment), std::move(section),
                  [min, max](const std::string& v) { return isNum(v, min, max); }};
    e.numeric  = true;
    e.minValue = min;
    e.maxValue = max;
    return e;
}

/**
 * CONFIGURATION DEFINITIONS
 * Static list of all supported settings, defaults, and sections.
 */
inline const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    // --- Appearance Settings ---
    enumEntry("base", "white",
              "Default state and input color (Tab-complete)",
              "Appearance Settings",
              ConfigValues::base),
    enumEntry("skin", "white",
              "Menu and header color (Tab-complete)",
              "",
              ConfigValues::skin),
    enumEntry("theme", "original",
              "List and prompt theme (Tab-complete)",
              "",
              ConfigValues::theme),

    // --- General Settings ---
    enumEntry("auto_update", "off",
              "Refresh ISO DB on startup using folder path history",
              "General Settings",
              ConfigValues::onOff),
    enumEntry("filenames_only", "off",
              "Display only filenames instead of full paths",
              "",
              ConfigValues::onOff),
    numericEntry("live_filter_limit", "10000",
                 "Maximum number of entries for live filtering (0 to disable)",
                 "", 0, 100000),
    numericEntry("pagination", "25",
                 "Items per page in list view (0 to disable)",
                 "", 0, 1000),

    // --- History Settings ---
    numericEntry("folder_path_history_lines", "30",
                 "Max unique FolderPaths to persist in history (0 to disable)",
                 "History Settings", 0, 5000),
    numericEntry("filter_history_lines", "15",
                 "Max unique FilterTerms to persist in history (0 to disable)",
                 "", 0, 1000),

    // --- Display Modes ---
    enumEntry("mount_list", "compact",
              "Display mode for mount operations",
              "Display Modes",
              ConfigValues::display),
    enumEntry("umount_list", "full",
              "Display mode for unmount operations",
              "",
              ConfigValues::display),
    enumEntry("cp_mv_rm_list", "full",
              "Display mode for file operations",
              "",
              ConfigValues::display),
    enumEntry("write2usb_list", "compact",
              "Display mode for write2usb operations",
              "",
              ConfigValues::display),
    enumEntry("convert2iso_lists", "compact",
              "Display mode for convert2iso operations",
              "",
              ConfigValues::display),

    // --- Thread Configuration ---
    numericEntry("combined_thread_cap", "16",
                 "Global thread pool size limit (requires restart to apply)",
                 "Thread Configuration", 1, 256),
    numericEntry("thread_cap_for_mount", "8",
                 "Max concurrent mounting tasks within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_umount", "8",
                 "Max concurrent unmounting tasks within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_cp_mv", "4",
                 "Max concurrent copy/move tasks within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_rm", "8",
                 "Max concurrent removal tasks within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_convert2iso", "4",
                 "Max concurrent ISO conversions within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_database_cleanup", "4",
                 "Max concurrent threads for ISO DB cleanup within the global pool",
                 "", 1, 128),
    numericEntry("thread_cap_for_list_sorting", "2",
                 "Max concurrent threads for list sorting within the global pool",
                 "", 1, 64),
    numericEntry("thread_cap_for_list_filtering", "2",
                 "Max concurrent threads for list filtering within the global pool",
                 "", 1, 64),
};

/**
 * CONFIGURATION EFFECTS & PERSISTENCE (settingsEffect.cpp)
 */
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config);
void printConfigError(const std::string& configPath);
bool flushCache(const std::string& configPath);
void applyConfigEffects(const std::map<std::string, std::string>& cache,
                        const std::string& onlyKey = {});
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap);
const ConfigEntry* findConfigEntry(const std::string& key);

namespace settings_ops {

    inline std::string resolveCurrent(const std::string& key, const ConfigEntry* entry) {
        auto it = ConfigCaches::g_configCache.find(key);
        if (it != ConfigCaches::g_configCache.end()) {
            return it->second;
        }
        return entry->defaultValue;
    }

    template <typename ThemeType>
    bool validate(const ConfigEntry* entry, const std::string& newVal,
                  const ThemeType& tc, const std::function<void()>& drawEdit) {
        if (!entry->validate || entry->validate(newVal)) {
            return true;
        }

        auto render = [&] {
            SyncOutput sync;
            drawEdit();
            std::cout << "\n" << tc.error << "Invalid value: "
                      << tc.warning << "'" << newVal << "'" << tc.reset << "\n";
        };
        pressEnterToTryWithRepaint(render);
        return false;
    }

    template <typename ThemeT>
    bool commitSetting(const std::string& configPath, const std::string& key,
                       const std::string& newVal, ThemeT& tc,
                       std::string& current, const std::function<void()>& drawEditCallback) {
        ConfigCaches::g_configCache[key] = newVal;
        applyConfigEffects(ConfigCaches::g_configCache, key);

        current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : "";
        bool saved = flushCache(configPath);

        tc = resolveOptionsTheme();

        auto render = [&] {
            SyncOutput sync;
            drawEditCallback();
            if (saved) {
                std::cout << std::format("\n{}✓ Updated and saved to: {}{}\n",
                                         tc.path, tc.reset, configPath);
            } else {
                std::cout << tc.warning << "\nWarning: " << tc.error
                          << "Setting update is memory-only (Disk write failed)."
                          << tc.reset << "\n";
            }
        };
        pressEnterToContinueWithRepaint(render);

        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }

        return saved;
    }

} // namespace settings_ops

/**
 * INTERACTIVE EDITOR DECLARATIONS (settingsEditor.cpp & related UI/input helpers)
 */
void helpSettingsEditor();
void syncCache(const std::string& filePath);
int pgup_handler(int, int);
int pgdn_handler(int, int);

EditNav editSetting(const std::string& configPath, const std::string& key,
                    const std::vector<std::string>& inputErrors,
                    size_t pos, size_t total);

void interactiveConfigEditor(const std::string& configPath);

#endif // SETTINGS_H
