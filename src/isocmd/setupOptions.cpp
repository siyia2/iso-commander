// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


/**
 * @struct ConfigEntry
 * @brief Metadata for a single configuration setting.
 */
struct ConfigEntry {
    std::string key;
    std::string defaultValue;
    std::string comment;
    std::string section; 
};


/**
 * @brief Canonical list of all supported configuration settings.
 * The "Source of Truth" for file generation and validation.
 */
static const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
    // --- General Settings ---
    {"auto_update", "off", "Enable background metadata updates from folder path history (on/off)", "General Settings"},
    {"filenames_only", "off", "Display only filenames instead of full paths (on/off)", ""},
    {"pagination", "25", "Items per page in list view (0 to disable)", ""},

    // --- History Settings ---
    {"folder_path_history_lines", "100", "Max unique folder paths to persist in history ~/.local/share/isocmd/database/iso_commander_database.txt", "History Settings"},
    {"filter_history_lines", "50", "Max unique search filters to persist in history ~/.local/share/isocmd/database/iso_commander_filter_database.txt", ""},

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
 * @brief Utility to trim whitespace from both ends of a string.
 */
static std::string trim(std::string str) {
    if(str.empty()) return str;
    str.erase(0, str.find_first_not_of(" \t"));
    size_t last = str.find_last_not_of(" \t");
    if (last != std::string::npos) str.erase(last + 1);
    return str;
}


/**
 * @brief Core Parser: Reads the config file and returns a key-value map.
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
                std::string key = trim(line.substr(0, equalPos));
                std::string value = trim(line.substr(equalPos + 1));
                config[key] = value;
            }
        }
        inFile.close();
    }
    return config;
}


/**
 * @brief Core Writer: Persists the map to disk with comments and sections.
 */
static void writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config) {
    std::ofstream outFile(configPath);
    if (!outFile) return;

    outFile << "############################################################\n";
    outFile << "# ISO COMMANDER'S CONFIGURATION FILE                       #\n";
    outFile << "############################################################\n\n";

    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        if (!entry.section.empty()) {
            outFile << "\n# --- " << entry.section << " ---\n";
        }

        auto it = config.find(entry.key);
        std::string value = (it != config.end() ? it->second : entry.defaultValue);

        outFile << "# " << entry.comment << "\n";
        outFile << entry.key << " = " << value << "\n";
    }
}


/**
 * @brief Self-Healing: Ensures missing keys are added to the file.
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
 * @brief Global Sync: Maps config values to internal application variables.
 */
static void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap) {
    auto getVal = [&](const std::string& key, size_t defaultVal) -> size_t {
        auto it = configMap.find(key);
        if (it == configMap.end()) return defaultVal;
        try {
            int v = std::stoi(it->second);
            return (v >= 0) ? static_cast<size_t>(v) : defaultVal;
        } catch (...) {
            return defaultVal;
        }
    };
    
    MAX_HISTORY_LINES          = getVal("folder_path_history_lines", 100);
    MAX_HISTORY_PATTERN_LINES  = getVal("filter_history_lines", 50);

    MAX_USEFUL_THREADS = getVal("max_thread_cap", 32);
    CPMV_THREAD_CAP    = getVal("threads_for_cp_mv", 8);
    CONV_THREAD_CAP    = getVal("threads_for_conversions", 8);
    MOUNT_THREAD_CAP   = getVal("threads_for_mount", 16);
    CLEAN_THREAD_CAP   = getVal("threads_for_database_cleanup", 16);
    UMOUNT_THREAD_CAP  = getVal("threads_for_umount", 32); 
    RM_THREAD_CAP      = getVal("threads_for_rm", 32);
    SORT_THREAD_CAP    = getVal("threads_for_list_sorting", 4);
    FILTER_THREAD_CAP  = getVal("threads_for_list_filtering", 4);
}


/**
 * @brief Checks if auto-update is enabled.
 */
bool readUserConfigUpdates(const std::string& filePath) {
    std::map<std::string, std::string> configMap = readConfig(filePath);
    ensureDefaults(configMap, filePath);
    
    std::string val = configMap["auto_update"];
    // Case-insensitive check
    return (val == "on" || val == "ON" || val == "On");
}


/**
 * @brief Loads pagination setting into the global variable.
 */
bool paginationSet(const std::string& filePath) {
    std::map<std::string, std::string> configMap = readConfig(filePath);
    if (configMap.count("pagination")) {
        try {
            ITEMS_PER_PAGE = std::stoi(configMap["pagination"]);
            return true;
        } catch (...) { return false; }
    }
    return false;
}


/**
 * @brief Initializes UI display toggles from config.
 */
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    fs::path configFilePath(filePath);
    if (!fs::exists(configFilePath.parent_path()) && !configFilePath.parent_path().empty()) {
        fs::create_directories(configFilePath.parent_path());
    }

    std::map<std::string, std::string> configMap = readConfig(filePath);
    ensureDefaults(configMap, filePath);

    displayConfig::toggleFullListMount        = (configMap["mount_list"]       == "full");
    displayConfig::toggleFullListUmount       = (configMap["umount_list"]      == "full");
    displayConfig::toggleFullListCpMvRm       = (configMap["cp_mv_rm_list"]    == "full");
    displayConfig::toggleFullListWrite        = (configMap["write_list"]       == "full");
    displayConfig::toggleFullListConversions  = (configMap["conversion_lists"] == "full");
    displayConfig::toggleNamesOnly            = (configMap["filenames_only"]   == "on");

    applyThreadCapsAndHistoryLimits(configMap);
    return configMap;
}


/**
 * @brief Updates pagination via runtime command.
 */
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    size_t eqPos = inputSearch.find('=');
    if (eqPos == std::string::npos) return;

    std::string valueStr = inputSearch.substr(eqPos + 1);
    try {
        int paginationValue = std::stoi(valueStr);
        std::map<std::string, std::string> config = readConfig(configPath);
        config["pagination"] = std::to_string(paginationValue);
        writeConfig(configPath, config);
        ITEMS_PER_PAGE = paginationValue;
        std::cout << "\n\033[1;32mPagination updated to " << paginationValue << ".\033[0m";
    } catch (...) {
        std::cout << "\n\033[1;31mInvalid pagination value.\033[0m";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief Toggles filename display mode via runtime command.
 */
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    std::map<std::string, std::string> config = readConfig(configPath);

    if (inputSearch == "*flno_on") {
        config["filenames_only"] = "on";
        displayConfig::toggleNamesOnly = true;
    } else if (inputSearch == "*flno_off") {
        config["filenames_only"] = "off";
        displayConfig::toggleNamesOnly = false;
    }

    writeConfig(configPath, config);
    std::cout << "\n\033[1;32mFilename display updated. ↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"}, {'u', "umount_list"}, {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"}, {'w', "write_list"}
};


bool isValidInput(const std::string& input) {
    if (input.size() < 4 || input[0] != '*' || (input.substr(1, 2) != "cl" && input.substr(1, 2) != "fl")) return false;
    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) return false;
    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) if (settingMap.find(c) == settingMap.end()) return false;
    return true;
}


/**
 * @brief Batch updates list display modes (compact vs full).
 */
void setDisplayMode(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

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
            if      (it->second == "mount_list")       displayConfig::toggleFullListMount       = isFull;
            else if (it->second == "umount_list")      displayConfig::toggleFullListUmount      = isFull;
            else if (it->second == "cp_mv_rm_list")    displayConfig::toggleFullListCpMvRm      = isFull;
            else if (it->second == "conversion_lists") displayConfig::toggleFullListConversions = isFull;
            else if (it->second == "write_list")       displayConfig::toggleFullListWrite       = isFull;
        }
    }
    writeConfig(configPath, config);
    std::cout << "\n\033[1;32mDisplay modes updated. ↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief Updates numeric settings (threads/history) via runtime command.
 */
void updateConfigSettings(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    static const std::unordered_map<std::string, std::string> keyMap = {
        {"filterhist", "filter_history_lines"}, {"pathhist", "folder_path_history_lines"},
        {"max", "max_thread_cap"}, {"mount", "threads_for_mount"}, {"umount", "threads_for_umount"},
        {"conv", "threads_for_conversions"}, {"cpmv", "threads_for_cp_mv"}, {"rm", "threads_for_rm"},
        {"clean", "threads_for_database_cleanup"}, {"sort", "threads_for_list_sorting"}, {"filter", "threads_for_list_filtering"}
    };

    size_t eqPos = inputSearch.find('=');
    if (eqPos == std::string::npos) return;

    std::string shortName = inputSearch.substr(5, eqPos - 5);
    std::string valueStr  = inputSearch.substr(eqPos + 1);

    auto it = keyMap.find(shortName);
    if (it != keyMap.end()) {
        std::map<std::string, std::string> config = readConfig(configPath);
        config[it->second] = valueStr;
        writeConfig(configPath, config);
        applyThreadCapsAndHistoryLimits(config);
        std::cout << "\n\033[1;32mSetting '" << it->second << "' updated to " << valueStr << ".\033[0m";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


/**
 * @brief UI screen to view current configuration.
 */
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();

    auto reportError = [&](const std::string& msg) {
        std::cerr << "\n\033[1;91m" << msg << "\033[1;91m.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    // Attempt to create directory if missing
    std::filesystem::path configDir = std::filesystem::path(configPath).parent_path();
    if (!configDir.empty() && !std::filesystem::exists(configDir)) {
        try { std::filesystem::create_directories(configDir); } 
        catch (...) { reportError("Permission denied creating config directory"); return; }
    }

    // Initialize with defaults if file doesn't exist
    if (!std::filesystem::exists(configPath)) {
        std::map<std::string, std::string> emptyMap;
        writeConfig(configPath, emptyMap); 
    }

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        reportError("Unable to open configuration file: " + configPath);
        return;
    }

    std::cout << "\n\033[1;96m==== Configuration Options ====\033[0;1m\n\n";
    std::string line;
    int lineNumber = 1;
    while (std::getline(configFile, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') {
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m" << line << "\033[0m\n";
        }
    }
    configFile.close();

    std::cout << "\n\033[1;93mConfig: \033[1;97m" << configPath << "\033[0;1m\n";
    std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
