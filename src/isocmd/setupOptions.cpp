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
        // Ensure the entire string was processed and it's not just a prefix
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
    {"menu_color", "white", "Menu accent color (green/cyan/white)", "Theme Settings",
        [](const std::string& v){ return v == "green" || v == "cyan" || v == "white"; }},
    {"ui_theme", "original", "List color theme (original/classic/high_contrast/neon/ocean/sunset/forest/midnight/mono/retro/crimson/dracula)", "",
        [](const std::string& v){
            static const std::unordered_set<std::string> valid = {
                "original","classic","high_contrast","neon","ocean",
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

    {"max_thread_cap", "32", "Global maximum concurrent threads allowed", "Thread Configuration", [](const std::string& v){ return isNum(v, 1, 256); }},
    {"threads_for_cp_mv", "8", "Threads allocated for copy/move tasks", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_conversions", "8", "Threads allocated for ISO file conversions", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_mount", "16", "Threads allocated for mounting tasks", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_umount", "32", "Threads allocated for unmounting tasks", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_database_cleanup", "16", "Threads allocated for DB maintenance", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_rm", "32", "Threads allocated for removal tasks", "", [](const std::string& v){ return isNum(v, 1, 128); }},
    {"threads_for_list_sorting", "4", "Threads allocated for UI list sorting", "", [](const std::string& v){ return isNum(v, 1, 64); }},
    {"threads_for_list_filtering", "4", "Threads allocated for UI list filtering", "", [](const std::string& v){ return isNum(v, 1, 64); }},
};

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
    
    MAX_HISTORY_LINES = getVal("folder_path_history_lines", 30);
    MAX_HISTORY_PATTERN_LINES = getVal("filter_history_lines", 15);
    MAX_USEFUL_THREADS = getVal("max_thread_cap", 32);
    CPMV_THREAD_CAP = getVal("threads_for_cp_mv", 8);
    CONV_THREAD_CAP = getVal("threads_for_conversions", 8);
    MOUNT_THREAD_CAP = getVal("threads_for_mount", 16);
    CLEAN_THREAD_CAP = getVal("threads_for_database_cleanup", 16);
    UMOUNT_THREAD_CAP = getVal("threads_for_umount", 32); 
    RM_THREAD_CAP = getVal("threads_for_rm", 32);
    SORT_THREAD_CAP = getVal("threads_for_list_sorting", 4);
    FILTER_THREAD_CAP = getVal("threads_for_list_filtering", 4);
}

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
 
    // Load and Self-Heal once into cache
    syncCache(filePath);
 
    // Synchronize Display Settings from Cache
    displayConfig::toggleFullListMount       = (g_configCache["mount_list"]       == "full");
    displayConfig::toggleFullListUmount      = (g_configCache["umount_list"]      == "full");
    displayConfig::toggleFullListCpMvRm      = (g_configCache["cp_mv_rm_list"]    == "full");
    displayConfig::toggleFullListWrite       = (g_configCache["write_list"]       == "full");
    displayConfig::toggleFullListConversions = (g_configCache["conversion_lists"] == "full");
    displayConfig::toggleNamesOnly           = (g_configCache["filenames_only"]   == "on");
 
    // Synchronize Theme Settings from Cache
    menuColor = g_configCache["menu_color"];
    color = getMenuColor();
    globalListTheme = g_configCache["ui_theme"];
 
    // Synchronize Threading & History Limits
    applyThreadCapsAndHistoryLimits(g_configCache);
    
    return g_configCache;
}

/**
 * @brief Command handler for changing pagination.
 * Updates both memory cache and disk.
 */
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    size_t underscorePos = inputSearch.find('_');
    if (underscorePos != std::string::npos) {
        std::string valueStr = inputSearch.substr(underscorePos + 1);
        
        if (isNum(valueStr, 0, 1000)) { 
            syncCache(configPath);
            int val = std::stoi(valueStr);
            
            // Update Cache
            g_configCache["pagination"] = valueStr;
            
            // Persistent Storage
            if (writeConfig(configPath, g_configCache)) {
                ITEMS_PER_PAGE = val;
                if (val > 0) {
                    std::cout << "\n\033[0;1mPagination status updated: Max entries per page set to \033[1;93m" 
                              << val << "\033[1;97m.\033[0m" << std::endl;
                } else {
                    std::cout << "\n\033[0;1mPagination status updated: \033[1;91mDisabled\033[0;1m." << std::endl;
                }
            } else {
                std::cerr << "\n\033[1;91mError: Unable to access configuration file.\n";
            }
        } else {
            std::cout << "\n\033[1;31mError: Invalid number (0-1000 required)\033[0m\n";
        }
    }
    
    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Command handler for toggling filename display mode.
 * Updates both memory cache and disk.
 */
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    if (inputSearch == "*flno_on" || inputSearch == "*flno_off") {
        bool isEnabling = (inputSearch == "*flno_on");
        syncCache(configPath);
        
        // Update Cache
        g_configCache["filenames_only"] = isEnabling ? "on" : "off";

        // Persistent Storage
        if (writeConfig(configPath, g_configCache)) {
            displayConfig::toggleNamesOnly = isEnabling;
            std::cout << "\n\033[0;1mFilename-only lists have been "
                      << (isEnabling ? "\033[1;92menabled" : "\033[1;91mdisabled")
                      << "\033[0;1m.\033[0m\n";
        } else {
            std::cerr << "\n\033[1;91mError: Unable to access configuration file.\n";
        }
    } else {
        std::cerr << "\n\033[1;31mError: Invalid command format.\033[0;1m\n";
    }

    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Command handler for UI appearance settings (menu colors and list themes).
 * Synchronizes the choice to the configuration file and updates live global variables.
 */
void updateUIAppearance(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    std::string key;
    std::string value;
    bool isValid = false;

    // Handle Menu Color: *menu:green, *menu:cyan, *menu:white
    if (inputSearch.substr(0, 6) == "*menu:") {
        key = "menu_color";
        value = inputSearch.substr(6); // Get everything after *menu:
        if (value == "green" || value == "cyan" || value == "white") {
            isValid = true;
        }
    } 
    // Handle List Themes: *theme:midnight, *theme:dracula, etc.
    else if (inputSearch.substr(0, 7) == "*theme:") {
        key = "ui_theme";
        value = inputSearch.substr(7); // Get everything after *theme:
        const std::unordered_set<std::string> validThemes = {
            "original", "classic", "high_contrast", "neon", "ocean", 
            "sunset", "forest", "midnight", "mono", "retro", "crimson", "dracula"
        };
        if (validThemes.count(value)) {
            isValid = true;
        }
    }

    if (isValid) {
        syncCache(configPath);
        g_configCache[key] = value;

        if (writeConfig(configPath, g_configCache)) {
			// Apply live changes to global variables
			if (key == "menu_color") {
				menuColor = value;
				color = getMenuColor(); // Refresh ANSI string
				
				// Custom feedback for Menu Color
				std::cout << "\n\033[0;1mMenu color set to: \033[1;92m" 
						  << value << "\033[0;1m.\033[0m\n";
			} 
			else if (key == "ui_theme") {
				globalListTheme = value;
				
				// Keep existing feedback for Theme
				std::cout << "\n\033[0;1mUI theme set to: \033[1;92m" 
						  << value << "\033[0;1m.\033[0m\n";
			}
		} else {
			std::cerr << "\n\033[1;91mError: Unable to access configuration file.\n";
		}
    } else {
        std::cerr << "\n\033[1;31mError: Invalid command or unsupported value.\033[0;1m\n";
    }

    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
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

    std::string command = inputSearch.substr(1, 2); 
    size_t underscorePos = inputSearch.find('_');
    
    if (underscorePos != std::string::npos) {
        std::string settingsStr = inputSearch.substr(underscorePos + 1);
        std::string newValue = (command == "cl") ? "compact" : "full";
        bool isFull = (newValue == "full");
        
        syncCache(configPath);
        std::vector<std::string> updatedLabels;

        for (char c : settingsStr) {
            auto it = settingMap.find(c);
            if (it != settingMap.end()) {
                std::string key = it->second;
                g_configCache[key] = newValue; // Update Cache

                if (key == "mount_list") {
                    displayConfig::toggleFullListMount = isFull;
                    updatedLabels.push_back("\033[1;92mmount");
                } 
                else if (key == "umount_list") {
                    displayConfig::toggleFullListUmount = isFull;
                    updatedLabels.push_back("\033[1;93munmount");
                } 
                else if (key == "cp_mv_rm_list") {
                    displayConfig::toggleFullListCpMvRm = isFull;
                    updatedLabels.push_back("\033[1;92mcp\033[0;1m/\033[1;93mmv\033[0;1m/\033[1;91mrm");
                } 
                else if (key == "conversion_lists") {
                    displayConfig::toggleFullListConversions = isFull;
                    updatedLabels.push_back("\033[1;38;5;208mconversions");
                } 
                else if (key == "write_list") {
                    displayConfig::toggleFullListWrite = isFull;
                    updatedLabels.push_back("\033[1;33mwrite");
                }
            }
        }

        if (writeConfig(configPath, g_configCache)) {
            if (!updatedLabels.empty()) {
                std::cout << "\n\033[0;1mDisplay mode set to \033[1;92m" << newValue << "\033[0;1m for:\033[0m\n";
                for (const auto& label : updatedLabels) {
                    std::cout << "  - " << label << "\033[0m\n";
                }
            }
        } else {
            std::cerr << "\n\033[1;91mError: Unable to access configuration file.\n";
        }
    }

    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Bulk updater for history and thread settings.
 * Updates both memory cache and disk.
 */
void updateConfigSettings(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); disable_ctrl_d();
    static const std::unordered_map<std::string, std::string> keyMap = {
        {"filterhist", "filter_history_lines"}, {"pathhist", "folder_path_history_lines"},
        {"max", "max_thread_cap"}, {"mount", "threads_for_mount"}, {"umount", "threads_for_umount"},
        {"conv", "threads_for_conversions"}, {"cpmv", "threads_for_cp_mv"}, {"rm", "threads_for_rm"},
        {"clean", "threads_for_database_cleanup"}, {"sort", "threads_for_list_sorting"}, {"filter", "threads_for_list_filtering"}
    };

    size_t eqPos = inputSearch.find('=');
    if (eqPos != std::string::npos) {
        std::string shortName = inputSearch.substr(5, eqPos - 5), valueStr = inputSearch.substr(eqPos + 1);
        auto it = keyMap.find(shortName);
        if (it != keyMap.end()) {
            bool valid = false;
            for(const auto& entry : CONFIG_ORDERED_DEFAULTS) {
                if(entry.key == it->second) { valid = entry.validate(valueStr); break; }
            }

            if (valid) {
                syncCache(configPath);
                g_configCache[it->second] = valueStr; // Update Cache
                if (writeConfig(configPath, g_configCache)) {
                    applyThreadCapsAndHistoryLimits(g_configCache); 
                    std::cout << "\n\033[1;37m" << it->second << " updated to: " << valueStr << "\033[0m\n";
                }
            } else {
                std::cout << "\n\033[1;31mError: Value '" << valueStr << "' is out of range or invalid.\033[0m\n";
            }
        }
    }
    std::cout << color << "\n↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief UI Function: Displays the current config file content to the user.
 */
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();

    fs::path p(configPath);
    if (!fs::exists(p.parent_path()) && !p.parent_path().empty()) 
        fs::create_directories(p.parent_path());

    // Ensure cache is synced and file is healthy before displaying
    syncCache(configPath);

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        std::cerr << "\n\033[1;31mError: Could not open configuration for reading.\033[0m\n";
        return;
    }

    std::cout << "\n\033[1;96m==== Current Configuration (Verified Cache) ====\033[0;1m\n\n";
    std::string line; 
    int lineNumber = 1;
    
    while (std::getline(configFile, line)) {
        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m" << trimmed << "\033[0m\n";
        }
    }
    configFile.close();

    std::cout << "\n\033[1;93mPath: \033[1;97m" << configPath 
              << color << "\n\n↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
