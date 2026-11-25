// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>

import std;
import isocmd.settings.settings;
import isocmd.core.paths;
import isocmd.core.threadCaps;
import isocmd.core.uiState;
import isocmd.ui.display.display;
import isocmd.ui.display.themes;

namespace fs = std::filesystem;

namespace ConfigValues {
	extern const std::vector<std::string> base    = {"ash", "black", "charcoal", "gray", "silver", "steel", "white"};
	extern const std::vector<std::string> skin    = {"amber", "black", "cyan", "gray", "green", "purple", "rose", "white"};
	extern const std::vector<std::string> theme   = {"classic", "crimson", "dracula", "forest", "high_contrast",
		                                             "midnight", "mono", "neon", "ocean", "original", "paper",
		                                             "retro", "sakura", "sunset", "tokyo"};
	extern const std::vector<std::string> onOff   = {"off", "on"};
	extern const std::vector<std::string> display = {"compact", "full"};
}

// True if @p v is one of @p values.
bool isOneOf(const std::vector<std::string>& values, const std::string& v) {

	return std::find(values.begin(), values.end(), v) != values.end();

}

/**
 * @brief Validates that a string represents a plain non-negative integer within [min, max].
 * Rejects leading zeros, signs, decimals, or any non-digit characters.
 */
bool isNum(const std::string& v, int min, int max) {

	if (v.empty() ||
	    !std::all_of(v.begin(), v.end(),
	                 [](unsigned char c) { return std::isdigit(c); }))
	    return false;
	if (v.size() > 1 && v[0] == '0') return false;
	try {
		int n = std::stoi(v);
		return n >= min && n <= max;
	} catch (...) {
		return false;
	}

}

/**
 * @brief Removes leading and trailing whitespace from a string.
 */
std::string trim(std::string str) {

	if (str.empty()) return str;
	str.erase(0, str.find_first_not_of(" \t"));
	std::size_t last = str.find_last_not_of(" \t");
	if (last != std::string::npos) str.erase(last + 1);
	return str;

}

/**
 * @brief Builds an enumerated setting whose validator and editor metadata share @p values.
 *
 * @p values must have static storage duration (use the @c ConfigValues lists).
 */
ConfigEntry enumEntry(std::string key, std::string def, std::string comment,
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
ConfigEntry numericEntry(std::string key, std::string def, std::string comment,
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
extern const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS = {
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
	numericEntry("pagination", "25",
	             "Items per page in list view (0 to disable)",
	             "", 0, 1000),
	enumEntry("write2usb_checksum", "off",
	          "Verify data after writing to USB",
	          "",
	          ConfigValues::onOff),

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

	if (ConfigCaches::g_configCache.empty() || ConfigCaches::g_cachedPath != filePath) {
		ConfigCaches::g_configCache = readConfig(filePath);
		ensureDefaults(ConfigCaches::g_configCache, filePath);
		ConfigCaches::g_cachedPath = filePath;
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
		if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) {return std::isdigit(c);}))
		return defaultVal;
		if (s.size() > 1 && s[0] == '0') return defaultVal;
		try {
			return static_cast<size_t>(std::stoull(s));
		} catch (...) { return defaultVal; }

	};

	GlobalState::MAX_HISTORY_LINES         		 = getVal("folder_path_history_lines",    30);
	GlobalState::MAX_HISTORY_PATTERN_LINES 		 = getVal("filter_history_lines",         15);

	GlobalThreadCaps::MAX_USEFUL_THREADS        = getVal("combined_thread_cap",               16);
	GlobalThreadCaps::MOUNT_THREAD_CAP          = getVal("thread_cap_for_mount",             8);
	GlobalThreadCaps::UMOUNT_THREAD_CAP         = getVal("thread_cap_for_umount",            8);
	GlobalThreadCaps::CPMV_THREAD_CAP           = getVal("thread_cap_for_cp_mv",             4);
	GlobalThreadCaps::RM_THREAD_CAP             = getVal("thread_cap_for_rm",                8);
	GlobalThreadCaps::CONV_THREAD_CAP           = getVal("thread_cap_for_convert2iso",       4);
	GlobalThreadCaps::CLEAN_THREAD_CAP          = getVal("thread_cap_for_database_cleanup",  4);
	GlobalThreadCaps::SORT_THREAD_CAP           = getVal("thread_cap_for_list_sorting",      2);
	GlobalThreadCaps::FILTER_THREAD_CAP         = getVal("thread_cap_for_list_filtering",    2);

}

// ---------------------------------------------------------------------------
// Read-only accessors
// ---------------------------------------------------------------------------

/**
 * @brief Optimized: Checks if auto_update is enabled using cache.
 */
bool readUserConfigUpdates(const std::string& filePath) {

	syncCache(filePath);
	auto it = ConfigCaches::g_configCache.find("auto_update");
	return (it != ConfigCaches::g_configCache.end() && it->second == "on");

}

/**
 * @brief Returns the current interactive USB checksum verification setting.
 */
bool write2usbChecksumEnabled() {

	const auto it = ConfigCaches::g_configCache.find("write2usb_checksum");
	return it != ConfigCaches::g_configCache.end() && it->second == "on";

}

/**
 * @brief Optimized: Loads pagination setting into the global ITEMS_PER_PAGE via cache.
 */
bool paginationSet(const std::string& filePath) {

	syncCache(filePath);
	auto it = ConfigCaches::g_configCache.find("pagination");

	if (it != ConfigCaches::g_configCache.end()) {
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

	// Existing configuration mappings
	displayConfig::toggleFullListMount         = (ConfigCaches::g_configCache["mount_list"]         == "full");
	displayConfig::toggleFullListUmount        = (ConfigCaches::g_configCache["umount_list"]        == "full");
	displayConfig::toggleFullListCpMvRm        = (ConfigCaches::g_configCache["cp_mv_rm_list"]      == "full");
	displayConfig::toggleFullListWrite2usb     = (ConfigCaches::g_configCache["write2usb_list"]     == "full");
	displayConfig::toggleFullListConvert2iso   = (ConfigCaches::g_configCache["convert2iso_lists"] == "full");
	displayConfig::toggleNamesOnly             = (ConfigCaches::g_configCache["filenames_only"]      == "on");

	skin          = ConfigCaches::g_configCache["skin"];
	color         = getskin();
	globalTheme   = ConfigCaches::g_configCache["theme"];

	// --- New: Update Default Text Color ---
	UI::Palette::defaultColor = ConfigCaches::g_configCache["base"];
	UI::Palette::updateDefaultColors();
	// --------------------------------------

	applyThreadCapsAndHistoryLimits(ConfigCaches::g_configCache);

	return ConfigCaches::g_configCache;

}
