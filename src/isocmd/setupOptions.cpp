// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <filesystem>
#include <fstream>

// Project Headers
#include "../caches.h"
#include "../concurrency.h"
#include "../display.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"

namespace fs = std::filesystem;

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
 * @brief Self-Healing logic: Checks for missing keys, bad values, OR wrong ordering.
 */
static bool ensureDefaults(std::map<std::string, std::string>& configMap, const std::string& configPath) {
    bool needsUpdate = false;

    // Check for missing keys or invalid values
    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        auto it = configMap.find(entry.key);
        if (it == configMap.end() || (entry.validate && !entry.validate(it->second))) {
            configMap[entry.key] = entry.defaultValue;
            needsUpdate = true;
        }
    }

    // Check if on-disk ordering matches CONFIG_ORDERED_DEFAULTS
    if (!needsUpdate) {
        std::ifstream inFile(configPath);
        if (inFile.is_open()) {
            std::vector<std::string> diskKeys;
            std::string line;
            while (std::getline(inFile, line)) {
                line = trim(line);
                if (line.empty() || line[0] == '#') continue;
                size_t equalPos = line.find('=');
                if (equalPos != std::string::npos)
                    diskKeys.push_back(trim(line.substr(0, equalPos)));
            }
            inFile.close();

            // Build expected key order from CONFIG_ORDERED_DEFAULTS
            std::vector<std::string> expectedKeys;
            for (const auto& entry : CONFIG_ORDERED_DEFAULTS)
                expectedKeys.push_back(entry.key);

            if (diskKeys != expectedKeys)
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
    if (GlobalCaches::g_configCache.empty() || GlobalCaches::g_cachedPath != filePath) {
        GlobalCaches::g_configCache = readConfig(filePath);
        ensureDefaults(GlobalCaches::g_configCache, filePath);
        GlobalCaches::g_cachedPath = filePath;
    }
}

/**
 * @brief Synchronizes configuration map values with internal global variables.
 */
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap) {
    auto getVal = [&](const std::string& key, size_t defaultVal) -> size_t {
		auto it = configMap.find(key);
		if (it == configMap.end()) return defaultVal;
		const std::string& s = it->second;
		if (s.empty() || !std::all_of(s.begin(), s.end(), ::isdigit)) return defaultVal;
		if (s.size() > 1 && s[0] == '0') return defaultVal;
		try {
			return static_cast<size_t>(std::stoull(s));
		} catch (...) { return defaultVal; }
	};
    
    GlobalState::MAX_HISTORY_LINES         		 = getVal("folder_path_history_lines",    30);
    GlobalState::MAX_HISTORY_PATTERN_LINES 		 = getVal("filter_history_lines",         15);
    
    GlobalConcurrency::MAX_USEFUL_THREADS        = getVal("combined_thread_cap",               32);
    GlobalConcurrency::MOUNT_THREAD_CAP          = getVal("thread_cap_for_mount",            16);
    GlobalConcurrency::UMOUNT_THREAD_CAP         = getVal("thread_cap_for_umount",           32); 
    GlobalConcurrency::CPMV_THREAD_CAP           = getVal("thread_cap_for_cp_mv",             8);
    GlobalConcurrency::RM_THREAD_CAP             = getVal("thread_cap_for_rm",               32);
    GlobalConcurrency::CONV_THREAD_CAP           = getVal("thread_cap_for_convert2iso",       8);
    GlobalConcurrency::CLEAN_THREAD_CAP          = getVal("thread_cap_for_database_cleanup", 16);
    GlobalConcurrency::SORT_THREAD_CAP           = getVal("thread_cap_for_list_sorting",      4);
    GlobalConcurrency::FILTER_THREAD_CAP         = getVal("thread_cap_for_list_filtering",    4);
}

// ---------------------------------------------------------------------------
// Read-only accessors
// ---------------------------------------------------------------------------

/**
 * @brief Optimized: Checks if auto_update is enabled using cache.
 */
bool readUserConfigUpdates(const std::string& filePath) {
    syncCache(filePath);
    auto it = GlobalCaches::g_configCache.find("auto_update");
    return (it != GlobalCaches::g_configCache.end() && it->second == "on");
}

/**
 * @brief Optimized: Loads pagination setting into the global ITEMS_PER_PAGE via cache.
 */
bool paginationSet(const std::string& filePath) {
    syncCache(filePath);
    auto it = GlobalCaches::g_configCache.find("pagination");
    
    if (it != GlobalCaches::g_configCache.end()) {
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            if (entry.key == "pagination") {
                if (entry.validate && entry.validate(it->second)) {
                    try { 
                        GlobalState::ITEMS_PER_PAGE = std::stoi(it->second); 
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
 
    displayConfig::toggleFullListMount       = (GlobalCaches::g_configCache["mount_list"]       == "full");
    displayConfig::toggleFullListUmount      = (GlobalCaches::g_configCache["umount_list"]      == "full");
    displayConfig::toggleFullListCpMvRm      = (GlobalCaches::g_configCache["cp_mv_rm_list"]    == "full");
    displayConfig::toggleFullListWrite2usb   = (GlobalCaches::g_configCache["write2usb_list"]       == "full");
    displayConfig::toggleFullListConvert2iso = (GlobalCaches::g_configCache["convert2iso_lists"] == "full");
    displayConfig::toggleNamesOnly           = (GlobalCaches::g_configCache["filenames_only"]   == "on");
 
    skin        = GlobalCaches::g_configCache["skin"];
    color       = getskin();
    globalTheme = GlobalCaches::g_configCache["theme"];
 
    applyThreadCapsAndHistoryLimits(GlobalCaches::g_configCache);
    
    return GlobalCaches::g_configCache;
}
