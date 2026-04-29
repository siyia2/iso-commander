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
    disable_ctrl_d();
    syncCache(configPath);

    while (true) {
        clearScrollBuffer();
        auto tc = resolveOptionsTheme();
        std::cout << "\n" << tc.highlight << "=== Settings Editor ===\n\n" << tc.reset;
        std::cout  << tc.warning << "Config File: " << tc.reset << configPath << "\n";

        int index = 1;
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            // If there's a section header, print it without affecting the index padding
            if (!entry.section.empty()) {
                std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
            }

            std::string val = g_configCache.count(entry.key) ? g_configCache[entry.key] : entry.defaultValue;
            
            std::cout << tc.warning << std::right << std::setw(2) << index++ << ". " << tc.reset
                      << tc.label << std::left << std::setw(32) << entry.key << tc.reset
                      << tc.data << "= " << tc.reset
                      << tc.highlight << val << tc.reset << "\n";
        }
        

        std::cout << "\n" << tc.accent << "Actions (↵): " << tc.warning << "1-" << (index-1) 
                  << tc.reset << " Edit | " << tc.warning << "r" << tc.reset << " Reset | " 
                  << tc.warning << "q" << tc.reset << " Save&Exit | " << tc.warning << "↵" << tc.reset << " Exit\n";

		std::string prompt = 
			std::string("\n\001") + std::string(UI::Palette::Yellow) + "\002Action" + 
			"\001" + std::string(tc.label) + "\002 ↵ : " + 
			"\001" + std::string(tc.reset) + "\002";
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        if (!rawInput) break;

        std::string input = trim(rawInput.get());
        if (input.empty()) break;

        if (input == "q" || input == "Q") {
			std::string confirmPrompt = std::string("\001") + std::string(tc.warning) + "\002" + 
									   "\nSave any changes and exit? (y/n): \001" + 
									   std::string(tc.reset) + "\002";
			
			std::unique_ptr<char, decltype(&std::free)> confirmInput(readline(confirmPrompt.c_str()), &std::free);
			
			if (confirmInput) {
				std::string confirm = trim(confirmInput.get());
				if (confirm == "y" || confirm == "Y") {
					flushCache(configPath);
					break;
				}
			}
			continue;
		}

        if (input == "r" || input == "R") {
            std::string confirmPrompt = std::string("\001") + std::string(tc.warning) + "\002" + 
                                       "\nReset all settings to defaults? (y/n): \001" + 
                                       std::string(tc.reset) + "\002";
            
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

        // Safe numeric check
        if (std::all_of(input.begin(), input.end(), ::isdigit)) {
            int choice = std::stoi(input);
            if (choice >= 1 && choice <= (int)CONFIG_ORDERED_DEFAULTS.size()) {
                editSetting(CONFIG_ORDERED_DEFAULTS[choice - 1].key);
            }
        }
    }
}

void editSetting(const std::string& key) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) { 
        if (e.key == key) { entry = &e; break; } 
    }
    if (!entry) return;

    // Move current value outside the loop so we can compare changes
    std::string current = g_configCache.count(key) ? g_configCache[key] : entry->defaultValue;

    while (true) {
        clearScrollBuffer();
        std::cout << "\n" << tc.highlight << "=== Edit Setting ===\n\n" << tc.reset;
        std::cout << tc.label << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
        std::cout << tc.label << "Current: " << tc.reset << tc.highlight << current << tc.reset << "\n";
        std::cout << tc.data << "\nDescription: " << entry->comment << tc.reset << "\n\n";

        // --- Dynamic Hint Block ---
        std::cout << tc.label << "Valid values: " << tc.reset;
        if (key == "skin") {
            std::cout << "green, cyan, white, purple, amber, rose\n";
        } else if (key == "theme") {
            std::cout << "original, classic, high_contrast, neon, ocean, sunset,\n"
                      << "              forest, midnight, mono, retro, crimson, dracula, tokyo\n";
        } else if (key.find("_list") != std::string::npos) {
            std::cout << "full, compact\n";
        } else if (key == "auto_update" || key == "filenames_only") {
            std::cout << "on, off\n";
        } else if (key == "pagination" || key.find("thread_cap") != std::string::npos || key.find("_lines") != std::string::npos) {
            std::cout << "numeric value (integer " << (key == "pagination" ? ">= 0" : "> 0") << ")\n";
        } else {
            std::cout << "Refer to description above\n";
        }
        std::cout << "\n";

        std::string prompt = std::string("\001") + std::string(UI::Palette::Green) + "\002Value\001" + std::string(tc.label) + "\002 ↵" + 
        " | ↵ return: \001" + std::string(tc.reset) + "\002";
        
        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        
        // Handle Cancel (Ctrl+D or Empty Enter)
        if (!rawInput) return;
        std::string newVal = trim(rawInput.get());
        if (newVal.empty() || newVal == current) return;

        // Validation Check
        if (entry->validate && !entry->validate(newVal)) {
            std::cout << "\n" << tc.error << "Invalid value: " << tc.warning << "'" << newVal << "'" << tc.reset << "\n";
            pressEnterToTry(); 
            continue; 
        }

        // --- Success Case ---
        g_configCache[key] = newVal;
        applyConfigEffects(g_configCache); 
        
        std::cout << "\n" << tc.label << "✓ Updated successfully." << tc.reset << "\n";
        pressEnterToContinue();
        break; // Exit the loop and return to the main menu
    }
}
