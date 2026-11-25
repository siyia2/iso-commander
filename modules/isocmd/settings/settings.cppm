// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.settings.settings;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- config/settings ----

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
	extern const std::vector<std::string> base;
	extern const std::vector<std::string> skin;
	extern const std::vector<std::string> theme;
	extern const std::vector<std::string> onOff;
	extern const std::vector<std::string> display;
}

/**
 * VALIDATION HELPERS & UTILITIES
 */

// True if @p v is one of @p values.
bool isOneOf(const std::vector<std::string>& values, const std::string& v);

/**
 * @brief Validates that a string represents a plain non-negative integer within [min, max].
 * Rejects leading zeros, signs, decimals, or any non-digit characters.
 */
bool isNum(const std::string& v, int min, int max);

/**
 * @brief Removes leading and trailing whitespace from a string.
 */
std::string trim(std::string str);

/**
 * @brief Builds an enumerated setting whose validator and editor metadata share @p values.
 *
 * @p values must have static storage duration (use the @c ConfigValues lists).
 */
ConfigEntry enumEntry(std::string key, std::string def, std::string comment,
                      std::string section, const std::vector<std::string>& values);

/**
 * @brief Builds a numeric setting whose validator and editor range hint share [min, max].
 */
ConfigEntry numericEntry(std::string key, std::string def, std::string comment,
                         std::string section, int min, int max);

/**
 * CONFIGURATION DEFINITIONS
 * Static list of all supported settings, defaults, and sections.
 */
extern const std::vector<ConfigEntry> CONFIG_ORDERED_DEFAULTS;

/**
 * CONFIGURATION EFFECTS & PERSISTENCE (settingsEffect.cpp)
 */
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config);
bool flushCache(const std::string& configPath);
void applyConfigEffects(const std::map<std::string, std::string>& cache,
                        const std::string& onlyKey = {});
void applyThreadCapsAndHistoryLimits(const std::map<std::string, std::string>& configMap);
const ConfigEntry* findConfigEntry(const std::string& key);

// Reads the live settings cache; defaults to disabled when absent.
bool write2usbChecksumEnabled();

/**
 * INTERACTIVE EDITOR DECLARATIONS (settingsEditor.cpp & related UI/input helpers)
 */
void helpSettingsEditor();
void syncCache(const std::string& filePath);
int pgup_handler(int, int);
int pgdn_handler(int, int);

void interactiveConfigEditor(const std::string& configPath);

} // extern "C++"
