// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"
#include "../settings.h"

/**
 * @brief Helper to apply configuration from cache to global variables.
 */
void applyConfigEffects(const std::map<std::string, std::string>& cache) {
    // Thread caps and history
    applyThreadCapsAndHistoryLimits(cache);

    // UI Toggles
    if (cache.count("mount_list"))        displayConfig::toggleFullListMount = (cache.at("mount_list") == "full");
    if (cache.count("umount_list"))       displayConfig::toggleFullListUmount = (cache.at("umount_list") == "full");
    if (cache.count("cp_mv_rm_list"))     displayConfig::toggleFullListCpMvRm = (cache.at("cp_mv_rm_list") == "full");
    if (cache.count("write2usb_list"))    displayConfig::toggleFullListWrite2usb = (cache.at("write2usb_list") == "full");
    if (cache.count("convert2iso_lists")) displayConfig::toggleFullListConvert2iso = (cache.at("convert2iso_lists") == "full");
    if (cache.count("filenames_only"))    displayConfig::toggleNamesOnly = (cache.at("filenames_only") == "on");
    
    // Appearance
    if (cache.count("skin")) { skin = cache.at("skin"); color = getskin(); }
    if (cache.count("theme")) { globalTheme = cache.at("theme"); }
    
    if (cache.count("pagination")) {
        try { ITEMS_PER_PAGE = std::stoul(cache.at("pagination")); } catch (...) {}
    }
}

void interactiveConfigEditor(const std::string& configPath) {
    signal(SIGINT, SIG_IGN);
    syncCache(configPath);

    while (true) {
        clearScrollBuffer();
        auto tc = resolveOptionsTheme();
        std::cout << "\n" << tc.highlight << "=== Settings Editor ===\n\n" << tc.reset;
        std::cout  << tc.warning << "Config File: " << tc.reset << configPath << "\n";

        int index = 1;
		for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
			if (!entry.section.empty()) {
				std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
			}
			std::string val = g_configCache.count(entry.key) ? g_configCache[entry.key] : entry.defaultValue;
			
			std::string_view valColor = (index == 1) ? color : std::string_view(tc.data);
			
			std::cout << tc.warning << std::right << std::setw(2) << index++ << ". " << tc.reset
					  << tc.label << std::left << std::setw(32) << entry.key << tc.reset
					  << "= " << tc.reset
					  << valColor << val << tc.reset << "\n";
		}
        

        std::cout << "\n" << tc.accent << "Actions (↵): " << tc.warning << "1-" << (index-1) 
                  << tc.reset << " Edit | " << tc.warning << "r" << tc.reset << " Reset | " 
                  << tc.warning << "q" << tc.reset << " Save&Return | " << tc.warning << "↵" << tc.reset << " Return\n";

		std::string prompt = std::format(
			"\n\001{}\002Action\001{}\002 ↵ : \001{}\002",
			UI::Palette::Yellow, 
			tc.label, 
			tc.reset
		);
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        if (!rawInput) break;

        std::string input = trim(rawInput.get());
        if (input.empty()) break;

        if (input == "q" || input == "Q") {
			std::string confirmPrompt = std::format(
				"\001{}\002\nSave settings to disk and return? (y/n): \001{}\002",
				tc.highlight, 
				tc.reset
			);
			
			std::unique_ptr<char, decltype(&std::free)> confirmInput(readline(confirmPrompt.c_str()), &std::free);
			
			if (confirmInput) {
				std::string confirm = trim(confirmInput.get());
				if (confirm == "y" || confirm == "Y") {
					if (!flushCache(configPath)) {
						pressEnterToContinue();
						continue;
					}
					break;
				}
			}
			continue;
		}

        if (input == "r" || input == "R") {
            std::string confirmPrompt = std::format(
				"\001{}\002\nReset all settings to defaults? (y/n): \001{}\002",
				tc.warning, 
				tc.reset
			);
            
            std::unique_ptr<char, decltype(&std::free)> confirmInput(readline(confirmPrompt.c_str()), &std::free);
            
            if (confirmInput) {
                std::string confirm = trim(confirmInput.get());
                if (confirm == "y" || confirm == "Y") {
                    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
                        g_configCache[e.key] = e.defaultValue;
                    }
                    
                    // Apply changes immediately to global state/UI
                    applyConfigEffects(g_configCache);
                    
                    std::cout << tc.label << "\nAll settings reset to defaults.\n" << tc.reset;
                    pressEnterToContinue();
                }
            }
            continue;
        }

		{
			// Build a dummy vector sized to CONFIG_ORDERED_DEFAULTS for bounds checking
			std::vector<std::string> slots(CONFIG_ORDERED_DEFAULTS.size());
			std::unordered_set<std::string> errors;
			std::unordered_set<int> choices;

			tokenizeInput(input, slots, errors, choices);
			if (!errors.empty()) std::cout << "\n";
			for (const auto& err : errors)
				std::cout << err << "\n";
			if (!errors.empty())
				pressEnterToContinue();

			// Edit in ascending order for predictability
			std::vector<int> ordered(choices.begin(), choices.end());
			std::sort(ordered.begin(), ordered.end());
			for (int choice : ordered) {
				// If user signals abort (Ctrl+D), we stop the for-loop immediately
				if (!editSetting(configPath, CONFIG_ORDERED_DEFAULTS[choice - 1].key)) {
					break; 
				}
			}
		}
	}
}

/**
 * @brief Persists the current configuration cache to a physical file.
 * 
 * This function reconstructs the configuration file by iterating through the 
 * global @c CONFIG_ORDERED_DEFAULTS. It ensures the file remains organized 
 * with section headers and descriptive comments. Values are sourced from 
 * @c g_configCache, falling back to defined defaults if a key is missing.
 * 
 * @param configPath The filesystem path to the configuration file to be written.
 * @note This operation uses @c std::ios::trunc, overwriting the existing file 
 *       to ensure consistency between the memory cache and disk.
 */
void saveConfigToFile(const std::string& configPath) {
    std::ofstream outFile(configPath, std::ios::trunc);
    if (!outFile.is_open()) return;

    for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
        // Write section header if it exists
        if (!entry.section.empty()) {
            outFile << "\n# === " << entry.section << " ===\n";
        }

        // Write the comment
        outFile << "# " << entry.comment << "\n";

        // Get value from cache, or use default if not present
        std::string value = g_configCache.count(entry.key) ? g_configCache[entry.key] : entry.defaultValue;
        outFile << entry.key << "=" << value << "\n\n";
    }
    outFile.close();
}

/**
 * @brief Provides an interactive CLI interface to modify a specific configuration setting.
 * 
 * Displays the current value, description, and valid input hints for a given key.
 * The function enters a retry loop until the user provides valid input or cancels.
 * 
 * @details The workflow includes:
 * - Finding the setting definition in @c CONFIG_ORDERED_DEFAULTS.
 * - Prompting the user via @c readline with dynamic hints based on the key type.
 * - Validating input using the entry's specific validation logic.
 * - On success: updating @c g_configCache, applying side effects via 
 *   @c applyConfigEffects, and persisting changes to disk via @c saveConfigToFile.
 * 
 * @param configPath Path to the configuration file (used for saving updates).
 * @param key The specific configuration identifier to edit.
 * @return true if the operation was completed or skipped (Empty Enter).
 * @return false if the operation was explicitly cancelled (Ctrl+D / EOF).
 */
bool editSetting(const std::string& configPath, const std::string& key) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) { 
        if (e.key == key) { entry = &e; break; } 
    }
    if (!entry) return true;

    // Move current value outside the loop so we can compare changes
    std::string current = g_configCache.count(key) ? g_configCache[key] : entry->defaultValue;

    while (true) {
        clearScrollBuffer();
        std::cout << "\n" << tc.highlight << "=== Edit Setting ===\n\n" << tc.reset;
        std::cout << tc.label << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
        std::cout << tc.label << "Current: " << tc.reset << tc.highlight << current << tc.reset << "\n";
        std::cout << "\nDescription: " << entry->comment << tc.reset << "\n\n";

        // --- Dynamic Hint Block ---
        std::cout << tc.label << "Valid values: " << tc.reset;
		if (key == "skin") {
			std::cout << "green, cyan, white, purple, amber, rose\n";
		} else if (key == "theme") {
			std::cout << "original, classic, high_contrast, neon, ocean, sunset,\n"
					  << "              forest, midnight, mono, retro, crimson, dracula, tokyo\n";
		} else if (key == "auto_update" || key == "filenames_only") {
			std::cout << "on, off\n";
		} else if (key == "pagination" || key.find("thread_cap") != std::string::npos || key.find("_lines") != std::string::npos) {
			std::cout << "numeric value (integer " << (key == "pagination" ? ">= 0" : "> 0") << ")\n";
		} else if (key.find("_list") != std::string::npos) {
			std::cout << "full, compact\n";
		} else {
			std::cout << "Refer to description above\n";
		}
        std::cout << "\n";

		std::string prompt = std::format(
			"\001{}\002Value\001{}\002 ↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
			UI::Palette::Green, 
			tc.label, 
			UI::Palette::Yellow, 
			tc.reset, 
			tc.reset
		);
        
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        // Handle Cancel (Ctrl+D or Empty Enter)
        if (!rawInput) return false;
        std::string newVal = trim(rawInput.get());
        if (newVal.empty() || newVal == current) return true;

        // Validation Check
        if (entry->validate && !entry->validate(newVal)) {
            std::cout << "\n" << tc.error << "Invalid value: " << tc.warning << "'" << newVal << "'" << tc.reset << "\n";
            pressEnterToTry(); 
            continue; 
        }

        // --- Success Case ---
        g_configCache[key] = newVal;
        applyConfigEffects(g_configCache);
        
        // Save the update to the physical file immediately
        saveConfigToFile(configPath);
        
        std::cout << "\n" << tc.label << "✓ Updated and saved to: " << tc.reset << configPath << "\n";
        
        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }
        
        pressEnterToContinue();
        break; // Exit loop
    }
    return true;
}
