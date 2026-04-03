// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


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
    try { int n = std::stoi(v); return n >= min && n <= max; } catch (...) { return false; }
};

/**
 * @brief Canonical list of all supported configuration settings with validation.
 */
static const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    {"auto_update", "off", "Enable background metadata updates from folder path history (on/off)", "General Settings", isOnOff},
    {"filenames_only", "off", "Display only filenames instead of full paths (on/off)", "", isOnOff},
    {"pagination", "25", "Items per page in list view (0 to disable)", "", [](const std::string& v){ return isNum(v, 0, 1000); }},
    
    {"folder_path_history_lines", "100", "Max unique folder paths to persist in history", "History Settings", [](const std::string& v){ return isNum(v, 1, 5000); }},
    {"filter_history_lines", "50", "Max unique search filters to persist in history", "", [](const std::string& v){ return isNum(v, 1, 1000); }},
    
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
    {"threads_for_list_filtering", "4", "Threads allocated for UI list filtering", "", [](const std::string& v){ return isNum(v, 1, 64); }}
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
 * @brief Writes the current configuration state back to disk.
 * @return True if write succeeded, False if file was inaccessible.
 */
static bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config) {
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
 * @brief Self-Healing logic: Checks for missing keys OR bad values and resets them.
 */
static bool ensureDefaults(std::map<std::string, std::string>& configMap, const std::string& configPath) {
    bool needsUpdate = false;
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        auto it = configMap.find(entry.key);
        // FIX: If key is missing OR fails validation, reset to default
        if (it == configMap.end() || (entry.validate && !entry.validate(it->second))) { 
            configMap[entry.key] = entry.defaultValue; 
            needsUpdate = true; 
        }
    }
    if (needsUpdate) return writeConfig(configPath, configMap);
    return true;
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
    
    MAX_HISTORY_LINES = getVal("folder_path_history_lines", 100);
    MAX_HISTORY_PATTERN_LINES = getVal("filter_history_lines", 50);
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
 * @brief Checks if auto_update is enabled.
 */
bool readUserConfigUpdates(const std::string& filePath) {
    std::map<std::string, std::string> configMap = readConfig(filePath);
    auto it = configMap.find("auto_update");
    if (it == configMap.end()) return false;
    std::string val = it->second;
    return (val == "on");
}

/**
 * @brief Loads pagination setting into the global ITEMS_PER_PAGE.
 */
bool paginationSet(const std::string& filePath) {
    std::map<std::string, std::string> configMap = readConfig(filePath);
    if (configMap.count("pagination")) {
        try { ITEMS_PER_PAGE = std::stoi(configMap["pagination"]); return true; } catch (...) { return false; }
    }
    return false;
}

/**
 * @brief Main entry point for loading all config settings at app startup.
 */
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    fs::path configFilePath(filePath);
    if (!fs::exists(configFilePath.parent_path()) && !configFilePath.parent_path().empty()) 
        fs::create_directories(configFilePath.parent_path());

    std::map<std::string, std::string> configMap = readConfig(filePath);
    ensureDefaults(configMap, filePath);

    displayConfig::toggleFullListMount = (configMap["mount_list"] == "full");
    displayConfig::toggleFullListUmount = (configMap["umount_list"] == "full");
    displayConfig::toggleFullListCpMvRm = (configMap["cp_mv_rm_list"] == "full");
    displayConfig::toggleFullListWrite = (configMap["write_list"] == "full");
    displayConfig::toggleFullListConversions = (configMap["conversion_lists"] == "full");
    displayConfig::toggleNamesOnly = (configMap["filenames_only"] == "on");

    applyThreadCapsAndHistoryLimits(configMap);
    return configMap;
}

/**
 * @brief Command handler for changing pagination.
 */
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    size_t underscorePos = inputSearch.find('_');
    if (underscorePos != std::string::npos) {
        std::string valueStr = inputSearch.substr(underscorePos + 1);
        if (isNum(valueStr, 0, 1000)) { // FIX: Validate before writing
            int val = std::stoi(valueStr);
            std::map<std::string, std::string> config = readConfig(configPath);
            config["pagination"] = valueStr;
            
            if (writeConfig(configPath, config)) {
                ITEMS_PER_PAGE = val;
                if (val > 0) std::cout << "\n\033[0;1mPagination updated to \033[1;93m" << val << "\033[0m" << std::endl;
                else std::cout << "\n\033[0;1mPagination \033[1;91mDisabled\033[0m." << std::endl;
            }
        } else {
            std::cout << "\n\033[1;31mError: Invalid number (0-1000 required)\033[0m\n";
        }
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Command handler for toggling filename display mode.
 */
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    std::map<std::string, std::string> config = readConfig(configPath);
    if (inputSearch == "*flno_on" || inputSearch == "*flno_off") {
        bool isEnabling = (inputSearch == "*flno_on");
        config["filenames_only"] = isEnabling ? "on" : "off";

        if (writeConfig(configPath, config)) {
            displayConfig::toggleNamesOnly = isEnabling;
            std::cout << "\n\033[0;1mFilename-only lists: " << (isEnabling ? "ON" : "OFF") << "\033[0m\n";
        }
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"}, {'u', "umount_list"}, {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"}, {'w', "write_list"}
};

/**
 * @brief Validates if the input string is a properly formatted display mode command.
 * Used by external modules to check syntax before calling setDisplayMode.
 */
bool isValidInput(const std::string& input) {
    // Check for minimum length and prefix (*cl_ or *fl_)
    if (input.size() < 4 || input[0] != '*') return false;
    
    std::string prefix = input.substr(1, 2);
    if (prefix != "cl" && prefix != "fl") return false;

    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) return false;

    // Validate that the characters after the underscore are known keys
    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) {
        if (settingMap.find(c) == settingMap.end()) return false;
    }
    
    return true;
}


/**
 * @brief Command handler for switching list views between Compact and Full.
 */
void setDisplayMode(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); 
    disable_ctrl_d();

    std::string command = inputSearch.substr(1, 2); 
    size_t underscorePos = inputSearch.find('_');
    if (underscorePos != std::string::npos) {
        std::string settingsStr = inputSearch.substr(underscorePos + 1);
        std::string newValue = (command == "cl") ? "compact" : "full";
        std::map<std::string, std::string> config = readConfig(configPath);
        
        for (char c : settingsStr) {
            if (settingMap.count(c)) config[settingMap.at(c)] = newValue;
        }

        if (writeConfig(configPath, config)) {
            std::cout << "\n\033[0;1mDisplay mode set to \033[1;92m" << newValue << "\033[0m\n";
            // Re-sync locals
            displayConfig::toggleFullListMount = (config["mount_list"] == "full");
            displayConfig::toggleFullListUmount = (config["umount_list"] == "full");
            // ... (sync other flags as needed)
        }
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Bulk updater for history and thread settings.
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
            // FIX: Validate the value before updating
            bool valid = false;
            for(const auto& entry : CONFIG_ORDERED_DEFAULTS) {
                if(entry.key == it->second) { valid = entry.validate(valueStr); break; }
            }

            if (valid) {
                std::map<std::string, std::string> config = readConfig(configPath);
                config[it->second] = valueStr; 
                if (writeConfig(configPath, config)) {
                    applyThreadCapsAndHistoryLimits(config); 
                    std::cout << "\n\033[1;37m" << it->second << " updated to: " << valueStr << "\033[0m\n";
                }
            } else {
                std::cout << "\n\033[1;31mError: Value '" << valueStr << "' is out of range or invalid.\033[0m\n";
            }
        }
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief UI Function: Displays the current config file content to the user.
 */
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();
    // (Logic for ensuring directory/file exists remains same...)
    std::ifstream configFile(configPath);
    if (!configFile.is_open()) return;

    std::cout << "\n\033[1;96m==== Configuration Options ====\033[0;1m\n\n";
    std::string line; int lineNumber = 1;
    while (std::getline(configFile, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') 
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m" << line << "\033[0m\n";
    }
    configFile.close();
    std::cout << "\n\033[1;93mConfig: \033[1;97m" << configPath << "\n\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
