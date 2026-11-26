// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.settings.settings;
import isocmd.core.paths;
import isocmd.core.uiState;
import isocmd.ui.display.display;
import isocmd.ui.display.themes;

// Declarations used only by this file (moved out of the module interface).
void printConfigError(const std::string& configPath);

// ============================================================================
// Internal Effect Helpers
// ============================================================================

namespace {

	/**
	 * @brief Maps a boolean setting key and enabled value to a target flag reference.
	 */
	struct BoolToggleRule {
		const char* key;          ///< Configuration key name.
		bool& target;             ///< Destination boolean state updated from the cache.
		const char* enabledValue; ///< Cache string value that sets @p target to `true`.
	};

	/**
	 * @brief Attempts to parse an unsigned long value without throwing on invalid input.
	 * @param rawValue String representation to parse.
	 * @param target Destination unsigned long updated only if parsing succeeds.
	 */
	void tryParseUlong(const std::string& rawValue, unsigned long& target) {

		try {
			target = std::stoul(rawValue);
		} catch (...) {
		}

	}

	/**
	 * @brief Updates boolean UI/display flags from matching keys in @p cache.
	 * @param cache Configuration key-value map.
	 * @param onlyKey Optional single key filter; empty applies to all matching rules.
	 * @param rules Array of toggle rule descriptors.
	 * @param count Number of elements in @p rules.
	 */
	void applyBoolToggles(const std::map<std::string, std::string>& cache,
	                      const std::string& onlyKey,
	                      const BoolToggleRule* rules,
	                      std::size_t count) {

		for (std::size_t i = 0; i < count; ++i) {
			const auto& rule = rules[i];
			if (!onlyKey.empty() && onlyKey != rule.key) {
				continue;
			}
			const auto it = cache.find(rule.key);
			if (it != cache.end()) {
				rule.target = (it->second == rule.enabledValue);
			}
		}

	}

	/**
	 * @brief Applies skin, theme, base palette, and pagination settings.
	 * @param cache Configuration key-value map.
	 * @param onlyKey Optional single key filter; empty applies to all matching keys.
	 */
	void applyAppearanceAndLimits(const std::map<std::string, std::string>& cache,
	                              const std::string& onlyKey) {

		auto matches = [&](const char* key) -> const std::string* {

			if (!onlyKey.empty() && onlyKey != key) {
				return nullptr;
			}
			const auto it = cache.find(key);
			return (it != cache.end()) ? &it->second : nullptr;

		};

		if (const std::string* val = matches("skin")) {
			skin = *val;
			color = getskin();
		}
		if (const std::string* val = matches("theme")) {
			globalTheme = *val;
		}
		if (const std::string* val = matches("base")) {
			UI::Palette::defaultColor = *val;
			UI::Palette::updateDefaultColors();
		}
		if (const std::string* val = matches("pagination")) {
			tryParseUlong(*val, GlobalState::ITEMS_PER_PAGE);
		}

	}

} // namespace

// ============================================================================
// Public Configuration Functions
// ============================================================================

/**
 * @brief Prints a theme-styled error message when the configuration file cannot be accessed.
 * @param configPath Path to the configuration file that failed to open or write.
 */
void printConfigError(const std::string& configPath) {

	auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
	std::cerr << "\n" << error
	          << "Error: Unable to access configuration file: "
	          << warning << "'" << configPath << "'"
	          << error << ".\033[J\n" << reset;

}

/**
 * @brief Writes the global configuration cache to disk and reports errors on failure.
 * @param configPath Destination configuration file path.
 * @return `true` if the configuration was written; `false` otherwise.
 */
bool flushCache(const std::string& configPath) {

	if (writeConfig(configPath, ConfigCaches::g_configCache)) {
		return true;
	}
	printConfigError(configPath);
	return false;

}

/**
 * @brief Propagates cached configuration values to live runtime state and UI settings.
 * @param cache Configuration key-value map to read values from.
 * @param onlyKey Optional setting key to apply selectively; empty applies all settings.
 */
void applyConfigEffects(const std::map<std::string, std::string>& cache,
                        const std::string& onlyKey) {

	static const std::unordered_set<std::string> nonThreadKeys = {
		"mount_list", "umount_list", "cp_mv_rm_list", "write2usb_list",
		"convert2iso_lists", "filenames_only", "skin", "theme", "base",
		"pagination", "verify_checksum", "buffered_writes"
	};
	if (onlyKey.empty() || !nonThreadKeys.count(onlyKey)) {
		applyThreadCapsAndHistoryLimits(cache);
	}

	const BoolToggleRule toggleRules[] = {
		{ "mount_list",        displayConfig::toggleFullListMount,       "full" },
		{ "umount_list",       displayConfig::toggleFullListUmount,      "full" },
		{ "cp_mv_rm_list",     displayConfig::toggleFullListCpMvRm,      "full" },
		{ "write2usb_list",    displayConfig::toggleFullListWrite2usb,   "full" },
		{ "convert2iso_lists", displayConfig::toggleFullListConvert2iso, "full" },
		{ "filenames_only",    displayConfig::toggleNamesOnly,           "on"   },
	};
	applyBoolToggles(cache, onlyKey, toggleRules, std::size(toggleRules));
	applyAppearanceAndLimits(cache, onlyKey);

}

/**
 * @brief Finds the default metadata entry matching a configuration key.
 * @param key Configuration key name to look up.
 * @return Pointer to the matching `ConfigEntry` in `CONFIG_ORDERED_DEFAULTS`, or `nullptr` if not found.
 */
const ConfigEntry* findConfigEntry(const std::string& key) {

	for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
		if (e.key == key) {
			return &e;
		}
	}
	return nullptr;

}
