// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


/**
 * @struct ConfigEntry
 * @brief Metadata for a single configuration setting.
 * * Used to define the structure of the configuration file, including
 * defaults, helpful comments for the user, and section headers.
 */
struct ConfigEntry {
    std::string key;          ///< The key string in the config file (e.g., "pagination")
    std::string defaultValue; ///< Fallback value if the key is missing
    std::string comment;      ///< Description written above the key in the file
    std::string section;      ///< If not empty, starts a new section header in the file
};


/**
 * @brief Canonical list of all supported configuration settings.
 * * This is the "Source of Truth." To add a new setting to the app, 
 * simply add an entry here. The system will handle the rest.
 */
static const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    // --- General Settings ---
    {"auto_update", "off", "Enable background metadata updates from folder path history (on/off)", "General Settings"},
    {"filenames_only", "off", "Display only filenames instead of full paths (on/off)", ""},
    {"pagination", "25", "Items per page in list view (0 to disable)", ""},

    // --- History Settings ---
    {"folder_path_history_lines", "100", "Max unique folder paths to persist in history", "History Settings"},
    {"filter_history_lines", "50", "Max unique search filters to persist in history", ""},

    // --- List Display Modes ---
    {"mount_list", "compact", "Display mode for mount operations (full/compact)", "Display Modes"},
    {"umount_list", "full", "Display mode for unmount operations (full/compact)", ""},
    {"cp_mv_rm_list", "compact", "Display mode for file operations (full/compact)", ""},
    {"write_list", "compact", "Display mode for write operations (full/compact)", ""},
    {"conversion_lists", "compact", "Display mode for conversion operations (full/compact)", ""},

    // --- Thread Configuration ---
    {"max_thread_cap", "32", "Global maximum concurrent threads allowed", "Thread Configuration"},
    {"threads_for_cp_mv", "8", "Threads allocated for copy/move tasks", ""},
    {"threads_for_conversions", "8", "Threads allocated for ISO file conversions", ""},
    {"threads_for_mount", "16", "Threads allocated for mounting tasks", ""},
    {"threads_for_umount", "32", "Threads allocated for unmounting tasks", ""},
    {"threads_for_database_cleanup", "16", "Threads allocated for DB maintenance", ""},
    {"threads_for_rm", "32", "Threads allocated for removal tasks", ""},
    {"threads_for_list_sorting", "4", "Threads allocated for UI list sorting", ""},
    {"threads_for_list_filtering", "4", "Threads allocated for UI list filtering", ""}
};


/**
 * @brief Utility to remove leading/trailing whitespace from strings.
 * Used to ensure keys and values are clean regardless of user formatting.
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
 * Skips comments (#) and empty lines. Trims whitespace automatically.
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
 * Rewrites the entire file using CONFIG_ORDERED_DEFAULTS to ensure the
 * file remains organized, commented, and contains all necessary keys.
 */
static void writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config) {
    std::ofstream outFile(configPath);
    if (!outFile) return;
    outFile << "############################################################\n# ISO COMMANDER'S CONFIGURATION FILE                       #\n############################################################\n\n";
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        if (!entry.section.empty()) outFile << "\n# --- " << entry.section << " ---\n";
        auto it = config.find(entry.key);
        // Use value from map if exists, otherwise fallback to hardcoded default
        outFile << "# " << entry.comment << "\n" << entry.key << " = " << (it != config.end() ? it->second : entry.defaultValue) << "\n";
    }
}


/**
 * @brief Self-Healing logic: Checks for missing keys and adds them.
 * @return True if file was already perfect, False if it had to be updated.
 */
static bool ensureDefaults(std::map<std::string, std::string>& configMap, const std::string& configPath) {
    bool needsUpdate = false;
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        if (configMap.find(entry.key) == configMap.end()) { 
            configMap[entry.key] = entry.defaultValue; 
            needsUpdate = true; 
        }
    }
    if (needsUpdate) writeConfig(configPath, configMap);
    return !needsUpdate;
}


/**
 * @brief Synchronizes configuration map values with internal global variables.
 * Handles string-to-integer conversion and bounds checking for threads.
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
    
    // History Sync
    MAX_HISTORY_LINES = getVal("folder_path_history_lines", 100);
    MAX_HISTORY_PATTERN_LINES = getVal("filter_history_lines", 50);
    
    // Thread Cap Sync
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
    ensureDefaults(configMap, filePath);
    std::string val = configMap["auto_update"];
    return (val == "on" || val == "ON" || val == "On");
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
 * Ensures directories exist and variables are synced.
 */
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    fs::path configFilePath(filePath);
    if (!fs::exists(configFilePath.parent_path()) && !configFilePath.parent_path().empty()) 
        fs::create_directories(configFilePath.parent_path());

    std::map<std::string, std::string> configMap = readConfig(filePath);
    ensureDefaults(configMap, filePath); // Fix missing keys automatically

    // Map file strings to UI toggle booleans
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
 * @brief Command handler for changing pagination (e.g., *pagination=50).
 */
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN); disable_ctrl_d();
    size_t eqPos = inputSearch.find('=');
    if (eqPos != std::string::npos) {
        std::string valueStr = inputSearch.substr(eqPos + 1);
        try {
            int val = std::stoi(valueStr);
            std::map<std::string, std::string> config = readConfig(configPath);
            config["pagination"] = std::to_string(val);
            writeConfig(configPath, config);
            ITEMS_PER_PAGE = val;
            std::cout << "\n\033[1;37mPagination set to: \033[1;37m" << val << "\033[0m\n";
        } catch (...) {}
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief Command handler for toggling filename display mode.
 */
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); disable_ctrl_d();
    std::map<std::string, std::string> config = readConfig(configPath);
    if (inputSearch == "*flno_on") {
        config["filenames_only"] = "on"; displayConfig::toggleNamesOnly = true;
        std::cout << "\n\033[1;37mFilenames only mode: \033[1;37mON\033[0m\n";
    } else if (inputSearch == "*flno_off") {
        config["filenames_only"] = "off"; displayConfig::toggleNamesOnly = false;
        std::cout << "\n\033[1;37mFilenames only mode: \033[1;37mOFF\033[0m\n";
    }
    writeConfig(configPath, config);
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief Mapping of short-codes to actual config keys for the setDisplayMode function.
 */
const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"}, {'u', "umount_list"}, {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"}, {'w', "write_list"}
};


/**
 * @brief Logic gate to check if an input string is a valid display-mode command.
 * Valid formats: *cl_m, *fl_m, *cl_muowc, etc.
 */
bool isValidInput(const std::string& input) {
    if (input.size() < 4 || input[0] != '*' || (input.substr(1, 2) != "cl" && input.substr(1, 2) != "fl")) return false;
    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) return false;
    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) if (settingMap.find(c) == settingMap.end()) return false;
    return true;
}


/**
 * @brief Command handler for switching list views between Compact and Full.
 * Supports bulk updates (e.g., *cl_mu for compact mount AND unmount).
 */
void setDisplayMode(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN); disable_ctrl_d();
    std::string command = inputSearch.substr(1, 2);
    size_t underscorePos = inputSearch.find('_', 3);
    std::string settingsStr = inputSearch.substr(underscorePos + 1);
    std::string newValue = (command == "cl") ? "compact" : "full";

    std::map<std::string, std::string> config = readConfig(configPath);
    for (char c : settingsStr) {
        auto it = settingMap.find(c);
        if (it != settingMap.end()) {
            config[it->second] = newValue;
            bool isFull = (newValue == "full");
            
            std::cout << "\n\033[0;1m" << inputSearch << " → " << newValue << " list is set for ";

            if (it->second == "mount_list") {
                displayConfig::toggleFullListMount = isFull;
                std::cout << "\033[1;92mmount";
            }
            else if (it->second == "umount_list") {
                displayConfig::toggleFullListUmount = isFull;
                std::cout << "\033[1;93munmount";
            }
            else if (it->second == "cp_mv_rm_list") {
                displayConfig::toggleFullListCpMvRm = isFull;
                std::cout << "\033[92mcp\033[0;1m/\033[93mmv\033[0;1m/\033[91mrm";
            }
            else if (it->second == "conversion_lists") {
                displayConfig::toggleFullListConversions = isFull;
                std::cout << "\033[1;38;5;208mconversions"; 
            }
            else if (it->second == "write_list") {
                displayConfig::toggleFullListWrite = isFull;
                std::cout << "\033[1;33mwrite";
            }
            std::cout << "\033[0;1m"; 
        }
    }
    std::cout << std::endl;
    writeConfig(configPath, config);
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief Bulk updater for history and thread settings.
 * Maps short-codes (like 'pathhist') to internal configuration keys.
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
            std::map<std::string, std::string> config = readConfig(configPath);
            config[it->second] = valueStr; 
            writeConfig(configPath, config);
            applyThreadCapsAndHistoryLimits(config); // Sync variables immediately
            std::cout << "\n\033[1;37mSetting \033[1;37m" << it->second << "\033[1;37m updated to: \033[1;37m" << valueStr << "\033[0m\n";
        }
    }
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief UI Function: Displays the current config file content to the user.
 * Parses the file on the fly to show only active settings (skipping comments).
 */
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();
    auto reportError = [&](const std::string& msg) {
        std::cerr << "\n\033[1;91m" << msg << ".\033[0;1m\n\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    // Ensure directory and file exist before trying to read
    std::filesystem::path configDir = std::filesystem::path(configPath).parent_path();
    if (!configDir.empty() && !std::filesystem::exists(configDir)) {
        try { std::filesystem::create_directories(configDir); } catch (...) { reportError("Permission denied"); return; }
    }
    if (!std::filesystem::exists(configPath)) { std::map<std::string, std::string> empty; writeConfig(configPath, empty); }

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) { reportError("Unable to open file"); return; }

    std::cout << "\n\033[1;96m==== Configuration Options ====\033[0;1m\n\n";
    std::string line; int lineNumber = 1;
    while (std::getline(configFile, line)) {
        line = trim(line);
        // Only display lines that contain actual settings
        if (!line.empty() && line[0] != '#') 
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m" << line << "\033[0m\n";
    }
    configFile.close();

    std::cout << "\n\033[1;93mConfig: \033[1;97m" << configPath << "\033[0;1m\n\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
