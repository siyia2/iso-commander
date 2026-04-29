// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"
#include "../settings.h"

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
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap) {
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
    CONV_THREAD_CAP           = getVal("thread_cap_for_convert2iso",       8);
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
    displayConfig::toggleFullListWrite2usb       = (g_configCache["write2usb_list"]       == "full");
    displayConfig::toggleFullListConvert2iso = (g_configCache["convert2iso_lists"] == "full");
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
        auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
        std::cout << "\n" << error << "Error: Invalid number (0-1000 required)\033[J\n" << reset;
        pressEnterToTry();
        return;
    }

    std::string valueStr = inputSearch.substr(colonPos + 1);
    if (!isNum(valueStr, 0, 1000)) {
        auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
        std::cout << "\n" << error << "Error: Invalid number (0-1000 required)\033[J\n" << reset;
        pressEnterToTry();
        return;
    }

    syncCache(configPath);
    int val = std::stoi(valueStr);
    g_configCache["pagination"] = valueStr;

    if (flushCache(configPath)) {
		ITEMS_PER_PAGE = static_cast<size_t>(val);
        auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
        if (val > 0) {
            std::cout << "\n" << label << "Pagination status updated: Max entries per page set to "
                      << warning << val << label << ".\033[J\n" << reset;
        } else {
            std::cout << "\n" << label << "Pagination status updated: "
                      << error << "Disabled" << label << ".\033[J\n" << reset;
        }
    }

    pressEnterToContinue();
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
        {"conv", "thread_cap_for_convert2iso"}, {"cpmv", "thread_cap_for_cp_mv"}, {"rm", "thread_cap_for_rm"},
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

            auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();

            if (valid) {
                syncCache(configPath);
                g_configCache[it->second] = valueStr;

                if (flushCache(configPath)) {
                    applyThreadCapsAndHistoryLimits(g_configCache);
                    std::cout << "\n" << accent << it->second
                              << label  << " updated to: "
                              << warning << valueStr << "\n" << reset;
                }
            } else {
                std::cout << "\n" << error << "Error: Value "
                          << warning << "'" << valueStr << "'"
                          << error  << " is out of range or invalid.\n" << reset;
            }
        }
    }

    pressEnterToContinue();
}

/**
 * @brief UI Function: Displays the current config file content to the user.
 */
void displayConfigurationOptions(const std::string& configPath) {
    syncCache(configPath);

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) { printConfigError(configPath); pressEnterToContinue(); return; }
    
    clearScrollBuffer();

    auto tc = resolveOptionsTheme();
    
    std::cout << "\n" << tc.highlight << "==== Current Configuration ====\n\n" << tc.reset;

    std::string line; 
    int lineNum = 1;
    while (std::getline(configFile, line)) {
        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            std::cout << tc.warning << lineNum++ << ". " 
                      << tc.label << trimmed << tc.reset << "\n";
        }
    }
    
    std::cout << "\n" << tc.warning << "Path: " << tc.reset << configPath << "\n";
    pressEnterToReturn();
}
