// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @struct ConfigEntry
 * @brief Metadata for a single configuration setting.
 */
struct ConfigEntry {
    std::string key;           ///< The key string in the config file (e.g., "pagination")
    std::string defaultValue;  ///< Fallback value if the key is missing
    std::string comment;       ///< Description written above the key in the file
    std::string section;       ///< If not empty, starts a new section header in the file
    std::function<bool(const std::string&)> validate; ///< Validation logic for the value
};

// --- Validation Helpers ---
auto isOnOff = [](const std::string& v) { return v == "on" || v == "off"; };
auto isDisplay = [](const std::string& v) { return v == "full" || v == "compact"; };
auto isNum = [](const std::string& v, int min, int max) {
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
 * @brief Canonical list of all supported configuration settings with validation.
 */
static const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    {"skin", "white", "Menu accent color (green/cyan/white/purple/amber/rose)", "Theme Settings",
        [](const std::string& v){ 
            return v == "green" || v == "cyan" || v == "white" || 
                   v == "purple" || v == "amber" || v == "rose"; 
        }},
    {"theme", "original", "List and prompt color theme (original/classic/high_contrast/neon/ocean/sunset/forest/midnight/mono/retro/crimson/dracula/tokyo)", "",
        [](const std::string& v){
            static const std::unordered_set<std::string> valid = {
                "original","classic","high_contrast","neon","ocean","tokyo",
                "sunset","forest","midnight","mono","retro","crimson","dracula"
            };
            return valid.count(v) > 0;
        }
    },

    {"auto_update", "off", "Enable background metadata updates from folder path history (on/off)", "General Settings", isOnOff},
    {"filenames_only", "on", "Display only filenames instead of full paths (on/off)", "", isOnOff},
    {"pagination", "25", "Items per page in list view (0 to disable)", "", [](const std::string& v){ return isNum(v, 0, 1000); }},

    {"folder_path_history_lines", "30", "Max unique folder paths to persist in history", "History Settings", [](const std::string& v){ return isNum(v, 1, 5000); }},
    {"filter_history_lines", "15", "Max unique search filters to persist in history", "", [](const std::string& v){ return isNum(v, 1, 1000); }},

    {"mount_list", "compact", "Display mode for mount operations (full/compact)", "Display Modes", isDisplay},
    {"umount_list", "full", "Display mode for unmount operations (full/compact)", "", isDisplay},
    {"cp_mv_rm_list", "compact", "Display mode for file operations (full/compact)", "", isDisplay},
    {"write_list", "compact", "Display mode for write operations (full/compact)", "", isDisplay},
    {"conversion_lists", "compact", "Display mode for conversion operations (full/compact)", "", isDisplay},

    {"combined_thread_cap", "32", "Global thread pool limit; excess tasks are queued", "Thread Configuration", [](const std::string& v){ return isNum(v, 1, 256); }},
    {"thread_cap_for_cp_mv", "8", "Max concurrent copy/move tasks using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_conversions", "8", "Max concurrent ISO conversions using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_mount", "16", "Max concurrent mounting tasks using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_umount", "32", "Max concurrent unmounting tasks using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_database_cleanup", "16", "Max concurrent DB maintenance tasks using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_rm", "32", "Max concurrent removal tasks using the global pool", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"thread_cap_for_list_sorting", "4", "Max concurrent UI list sorting using the global pool", "", [](const std::string& v){ return isNum(v, 1, 64); }},
    {"thread_cap_for_list_filtering", "4", "Max concurrent UI list filtering using the global pool", "", [](const std::string& v){ return isNum(v, 1, 64); }},
};

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Utility to remove leading/trailing whitespace from strings.
 */
static std::string trim(std::string str) {
    if(str.empty()) return str;
    str.erase(0, str.find_first_not_of(" \t"));
    size_t last = str.find_last_not_of(" \t");
    if (last != std::string::npos) str.erase(last + 1);
    return str;
}

/**
 * @brief Thin wrapper that resolves the four most-used theme colors in one call.
 *
 * Callers receive four std::string_view values covering the common semantic
 * roles (label/muted, accent/success, warning/value, error/secondary).
 * Centralising this lookup eliminates the repetitive isOrig ternary that
 * previously appeared in every command handler.
 */
struct ThemeColors {
    std::string_view label;     ///< Neutral label / muted text
    std::string_view accent;    ///< Success / enable / positive highlight
    std::string_view warning;   ///< Value / numeric highlight
    std::string_view error;     ///< Error / disable / negative highlight
};

static ThemeColors resolveTheme() {
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    if (isOrig) {
        return {
            originalColors::boldAlt,   ///< label: Standard bold white/reset
            originalColors::green,  ///< accent: Vibrant Kelly Green
            originalColors::yellow, ///< warning: Pure Yellow
            originalColors::red     ///< error: Bright Red
        };
    }

    return {
        theme->muted,               ///< Neutral label / muted text
        theme->accent,              ///< Success / enable / positive highlight
        theme->warning,             ///< Value / numeric highlight
        theme->secondary            ///< Error / disable / negative highlight
    };
}

/**
 * @brief Prints a standardised config-file access error then flushes to stderr.
 */
static void printConfigError(const std::string& configPath) {
    auto [label, accent, warning, error] = resolveTheme();
    std::cerr << "\n" << error
              << "Error: Unable to access configuration file: "
              << warning << "'" << configPath << "'"
              << error << ".\033[J\n" << originalColors::boldAlt;
}

/**
 * @brief Prints the standard "↵ to continue…" pause and waits for Enter.
 */
static void pauseForInput() {
    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Commits a mutated cache to disk, printing an error on failure.
 * @return true if the write succeeded.
 */
static bool flushCache(const std::string& configPath) {
    if (writeConfig(configPath, g_configCache)) return true;
    printConfigError(configPath);
    return false;
}

// ---------------------------------------------------------------------------
// Core Config I/O
// ---------------------------------------------------------------------------

/**
 * @brief Logic to write the current configuration state back to disk.
 */
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config) {
    std::ofstream outFile(configPath);
    if (!outFile) return false;

    outFile << "############################################################\n# ISO COMMANDER'S CONFIGURATION FILE                        #\n############################################################\n\n";
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        if (!entry.section.empty()) outFile << "\n# --- " << entry.section << " ---\n";
        auto it = config.find(entry.key);
        outFile << "# " << entry.comment << "\n" << entry.key << " = " << (it != config.end() ? it->second : entry.defaultValue) << "\n";
    }
    return true;
}

/**
 * @brief Reads the configuration file into a key-value map.
 */
std::map<std::string, std::string> readConfig(const std::string& configPath) {
    std::map<std::string, std::string> config;
    std::ifstream inFile(configPath);
    if (inFile.is_open()) {
        std::string line;
        while (std::getline(inFile, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            size_t equalPos = line.find('=');
            if (equalPos != std::string::npos) {
                config[trim(line.substr(0, equalPos))] = trim(line.substr(equalPos + 1));
            }
        }
        inFile.close();
    }
    return config;
}

/**
 * @brief Self-Healing logic: Checks for missing keys OR bad values and resets them.
 */
static bool ensureDefaults(std::map<std::string, std::string>& configMap, const std::string& configPath) {
    bool needsUpdate = false;
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        auto it = configMap.find(entry.key);
        if (it == configMap.end() || (entry.validate && !entry.validate(it->second))) { 
            configMap[entry.key] = entry.defaultValue; 
            needsUpdate = true; 
        }
    }
    if (needsUpdate) return writeConfig(configPath, configMap);
    return true;
}

/**
 * @brief Internal helper to ensure the memory cache is ready.
 * Eliminates redundant disk reads by checking if we already have the data.
 */
void syncCache(const std::string& filePath) {
    if (g_configCache.empty() || g_cachedPath != filePath) {
        g_configCache = readConfig(filePath);
        ensureDefaults(g_configCache, filePath);
        g_cachedPath = filePath;
    }
}

/**
 * @brief Synchronizes configuration map values with internal global variables.
 */
static void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap) {
    auto getVal = [&](const std::string& key, size_t defaultVal) -> size_t {
        auto it = configMap.find(key);
        if (it == configMap.end()) return defaultVal;
        try { 
            int v = std::stoi(it->second); 
            return (v >= 0) ? static_cast<size_t>(v) : defaultVal; 
        } catch (...) { return defaultVal; }
    };
    
    MAX_HISTORY_LINES         = getVal("folder_path_history_lines",    30);
    MAX_HISTORY_PATTERN_LINES = getVal("filter_history_lines",         15);
    MAX_USEFUL_THREADS        = getVal("combined_thread_cap",               32);
    CPMV_THREAD_CAP           = getVal("thread_cap_for_cp_mv",             8);
    CONV_THREAD_CAP           = getVal("thread_cap_for_conversions",       8);
    MOUNT_THREAD_CAP          = getVal("thread_cap_for_mount",            16);
    CLEAN_THREAD_CAP          = getVal("thread_cap_for_database_cleanup", 16);
    UMOUNT_THREAD_CAP         = getVal("thread_cap_for_umount",           32); 
    RM_THREAD_CAP             = getVal("thread_cap_for_rm",               32);
    SORT_THREAD_CAP           = getVal("thread_cap_for_list_sorting",      4);
    FILTER_THREAD_CAP         = getVal("thread_cap_for_list_filtering",    4);
}

// ---------------------------------------------------------------------------
// Read-only accessors
// ---------------------------------------------------------------------------

/**
 * @brief Optimized: Checks if auto_update is enabled using cache.
 */
bool readUserConfigUpdates(const std::string& filePath) {
    syncCache(filePath);
    auto it = g_configCache.find("auto_update");
    return (it != g_configCache.end() && it->second == "on");
}

/**
 * @brief Optimized: Loads pagination setting into the global ITEMS_PER_PAGE via cache.
 */
bool paginationSet(const std::string& filePath) {
    syncCache(filePath);
    auto it = g_configCache.find("pagination");
    
    if (it != g_configCache.end()) {
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            if (entry.key == "pagination") {
                if (entry.validate && entry.validate(it->second)) {
                    try { 
                        ITEMS_PER_PAGE = std::stoi(it->second); 
                        return true; 
                    } catch (...) { return false; }
                }
                break; 
            }
        }
    }
    return false;
}

/**
 * @brief Main entry point for loading all config settings at app startup.
 * Performs the initial population of the memory cache.
 */
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    fs::path configFilePath(filePath);
    if (!fs::exists(configFilePath.parent_path()) && !configFilePath.parent_path().empty()) 
        fs::create_directories(configFilePath.parent_path());
 
    syncCache(filePath);
 
    displayConfig::toggleFullListMount       = (g_configCache["mount_list"]       == "full");
    displayConfig::toggleFullListUmount      = (g_configCache["umount_list"]      == "full");
    displayConfig::toggleFullListCpMvRm      = (g_configCache["cp_mv_rm_list"]    == "full");
    displayConfig::toggleFullListWrite       = (g_configCache["write_list"]       == "full");
    displayConfig::toggleFullListConversions = (g_configCache["conversion_lists"] == "full");
    displayConfig::toggleNamesOnly           = (g_configCache["filenames_only"]   == "on");
 
    skin        = g_configCache["skin"];
    color       = getskin();
    globalTheme = g_configCache["theme"];
 
    applyThreadCapsAndHistoryLimits(g_configCache);
    
    return g_configCache;
}

// ---------------------------------------------------------------------------
// Command Handlers
// ---------------------------------------------------------------------------

/**
 * @brief Command handler for changing pagination.
 * Updates both memory cache and disk.
 */
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    size_t colonPos = inputSearch.find(':');
    if (colonPos == std::string::npos) {
        auto [label, accent, warning, error] = resolveTheme();
        std::cout << "\n" << error << "Error: Invalid number (0-1000 required)\033[J\n" << originalColors::boldAlt;
        pauseForInput();
        return;
    }

    std::string valueStr = inputSearch.substr(colonPos + 1);
    if (!isNum(valueStr, 0, 1000)) {
        auto [label, accent, warning, error] = resolveTheme();
        std::cout << "\n" << error << "Error: Invalid number (0-1000 required)\033[J\n" << originalColors::boldAlt;
        pauseForInput();
        return;
    }

    syncCache(configPath);
    int val = std::stoi(valueStr);
    g_configCache["pagination"] = valueStr;

    if (flushCache(configPath)) {
        auto [label, accent, warning, error] = resolveTheme();
        if (val > 0) {
            std::cout << "\n" << label << "Pagination status updated: Max entries per page set to "
                      << warning << val << label << ".\033[J\n" << originalColors::boldAlt;
        } else {
            std::cout << "\n" << label << "Pagination status updated: "
                      << error << "Disabled" << label << ".\033[J\n" << originalColors::boldAlt;
        }
    }

    pauseForInput();
}

/**
 * @brief Command handler for toggling filename display mode.
 * Updates both memory cache and disk.
 */
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    if (inputSearch != "*flno:on" && inputSearch != "*flno:off") {
        auto [label, accent, warning, error] = resolveTheme();
        std::cerr << "\n" << error << "Error: Invalid command format.\033[J\n" << originalColors::boldAlt;
        pauseForInput();
        return;
    }

    bool isEnabling = (inputSearch == "*flno:on");
    syncCache(configPath);
    g_configCache["filenames_only"] = isEnabling ? "on" : "off";

    if (flushCache(configPath)) {
        displayConfig::toggleNamesOnly = isEnabling;
        auto [label, accent, warning, error] = resolveTheme();
        std::cout << "\n" << label << "Filename-only lists have been "
                  << (isEnabling ? accent : error)
                  << (isEnabling ? "enabled" : "disabled")
                  << label << ".\033[J\n" << originalColors::boldAlt;
    }

    pauseForInput();
}

/**
 * @brief Command handler for UI appearance settings (menu colors and list themes).
 * Synchronizes the choice to the configuration file and updates live global variables.
 */
void updateUIAppearance(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    std::string key, value;

    if (inputSearch.substr(0, 6) == "*skin:") {
        key   = "skin";
        value = inputSearch.substr(6);
    } else if (inputSearch.substr(0, 7) == "*theme:") {
        key   = "theme";
        value = inputSearch.substr(7);
    }

    // Validate via the canonical CONFIG_ORDERED_DEFAULTS entry
    bool isValid = false;
    if (!key.empty()) {
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            if (entry.key == key) { isValid = entry.validate(value); break; }
        }
    }

    if (!isValid) {
        auto [label, accent, warning, error] = resolveTheme();
        std::cerr << "\n" << error << "Error: Invalid command or unsupported value.\033[J\n" << originalColors::boldAlt;
        pauseForInput();
        return;
    }

    syncCache(configPath);
    g_configCache[key] = value;

    if (flushCache(configPath)) {
        if (key == "skin") {
            skin  = value;
            color = getskin();
        } else {
            globalTheme = value;
        }
        // Resolve theme AFTER applying the new value so feedback uses the new colors.
        auto [label, accent, warning, error] = resolveTheme();
        std::string_view settingLabel = (key == "skin") ? "Skin color" : "UI theme";
        std::cout << "\n" << label << settingLabel << " set to: "
          << (key == "skin" ? color : accent) << value << label << ".\033[J\n" << originalColors::boldAlt;
    }

    pauseForInput();
}

const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"}, {'u', "umount_list"}, {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"}, {'w', "write_list"}
};

/**
 * @brief Validates if the input string is a properly formatted display mode command.
 */
bool isValidInput(const std::string& input) {
    if (input.size() < 4 || input[0] != '*') return false;
    
    std::string prefix = input.substr(1, 2);
    if (prefix != "cl" && prefix != "fl") return false;

    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) return false;

    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) {
        if (settingMap.find(c) == settingMap.end()) return false;
    }
    return true;
}

/**
 * @brief Command handler for switching list views between Compact and Full.
 * Updates both memory cache and disk.
 */
void setDisplayMode(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    std::string command     = inputSearch.substr(1, 2); 
    size_t      underscorePos = inputSearch.find('_');
    
    if (underscorePos == std::string::npos) { pauseForInput(); return; }

    std::string settingsStr = inputSearch.substr(underscorePos + 1);
    std::string newValue    = (command == "cl") ? "compact" : "full";
    bool        isFull      = (newValue == "full");
    
    syncCache(configPath);
    std::vector<std::string> updatedLabels;

    for (char c : settingsStr) {
        auto it = settingMap.find(c);
        if (it == settingMap.end()) continue;

        const std::string& key = it->second;
        g_configCache[key] = newValue;
		if (key == "mount_list") {
			displayConfig::toggleFullListMount = isFull;
			updatedLabels.push_back(std::string(originalColors::green) + "mount");
		}
		else if (key == "umount_list") {
			displayConfig::toggleFullListUmount = isFull;
			updatedLabels.push_back(std::string(originalColors::yellow) + "unmount");
		}
		else if (key == "cp_mv_rm_list") {
			displayConfig::toggleFullListCpMvRm = isFull;
			// Combining multiple views into one string
			updatedLabels.push_back(
				std::string(originalColors::green)  + "cp"+ std::string(originalColors::boldAlt) +"/" + 
				std::string(originalColors::yellow) + "mv"+ std::string(originalColors::boldAlt) + "/" + 
				std::string(originalColors::red)    + "rm"
			);
		}
		else if (key == "conversion_lists") {
			displayConfig::toggleFullListConversions = isFull;
			updatedLabels.push_back(std::string(originalColors::orange) + "conversions");
		}
		else if (key == "write_list") {
			displayConfig::toggleFullListWrite = isFull;
			updatedLabels.push_back(std::string(originalColors::yellow) + "write");
		}
    }

    if (flushCache(configPath) && !updatedLabels.empty()) {
        auto [label, accent, warning, error] = resolveTheme();
        std::cout << "\n" << label << "Display mode set to "
                  << accent << newValue << label << " for:\033[J\n" << originalColors::boldAlt;
        for (const auto& lbl : updatedLabels)
            std::cout << "  " << label << "- " << originalColors::boldAlt << lbl << "\n";
    }

    pauseForInput();
}

/**
 * @brief Bulk updater for history and thread settings.
 * Updates both memory cache and disk.
 */
void updateConfigSettings(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    static const std::unordered_map<std::string, std::string> keyMap = {
        {"filterhist", "filter_history_lines"}, {"pathhist", "folder_path_history_lines"},
        {"max", "combined_thread_cap"}, {"mount", "thread_cap_for_mount"}, {"umount", "thread_cap_for_umount"},
        {"conv", "thread_cap_for_conversions"}, {"cpmv", "thread_cap_for_cp_mv"}, {"rm", "thread_cap_for_rm"},
        {"clean", "thread_cap_for_database_cleanup"}, {"sort", "thread_cap_for_list_sorting"}, {"filter", "thread_cap_for_list_filtering"}
    };

    size_t eqPos = inputSearch.find('=');
    if (eqPos != std::string::npos) {
        std::string shortName = inputSearch.substr(5, eqPos - 5);
        std::string valueStr  = inputSearch.substr(eqPos + 1);
        auto it = keyMap.find(shortName);

        if (it != keyMap.end()) {
            bool valid = false;
            for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
                if (entry.key == it->second) { valid = entry.validate(valueStr); break; }
            }

            auto [label, accent, warning, error] = resolveTheme();

            if (valid) {
                syncCache(configPath);
                g_configCache[it->second] = valueStr;

                if (flushCache(configPath)) {
                    applyThreadCapsAndHistoryLimits(g_configCache);
                    std::cout << "\n" << accent << it->second
                              << label  << " updated to: "
                              << warning << valueStr << "originalColors::boldAlt\n";
                }
            } else {
                std::cout << "\n" << error << "Error: Value "
                          << warning << "'" << valueStr << "'"
                          << error  << " is out of range or invalid.originalColors::boldAlt\n";
            }
        }
    }

    pauseForInput();
}

/**
 * @brief UI Function: Displays the current config file content to the user.
 */
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();

    fs::path p(configPath);
    if (!fs::exists(p.parent_path()) && !p.parent_path().empty()) 
        fs::create_directories(p.parent_path());

    syncCache(configPath);

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        printConfigError(configPath);
        return;
    }
	
	const ListTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");
	
    std::string_view accent = isOriginal ? originalColors::cyan : theme->accent;
    std::string_view warning = isOriginal ? originalColors::yellow : theme->warning;
    std::string_view label = isOriginal ? originalColors::green : theme->muted;
       
	std::cout << "\n" << accent << "==== Current Configuration ====\n\n" << originalColors::boldAlt;

	std::string line; 
	int lineNumber = 1;

	while (std::getline(configFile, line)) {
		std::string trimmed = trim(line);
		if (!trimmed.empty() && trimmed[0] != '#') {
			std::cout << label << lineNumber++ << ". "
					  << originalColors::boldAlt << trimmed << originalColors::boldAlt << "\n";
		}
	}
	configFile.close();

	// Using warning (Yellow) and boldAlt (White) for the footer
	std::cout << "\n" << warning << "Path: " << originalColors::boldAlt << configPath 
          << color << "\n\n↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
