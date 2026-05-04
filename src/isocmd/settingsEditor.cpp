// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <csignal>
#include <filesystem>
#include <format>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../caches.h"
#include "../display.h"
#include "../inputHandling.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"
#include "../verbose.h"
#include "../tokenize.h"

namespace fs = std::filesystem;

/**
 * @brief Synchronizes global runtime variables with values from the configuration cache.
 * 
 * This function acts as the bridge between the raw string-based @c g_configCache 
 * and the actual functional variables used by the application (UI toggles, 
 * thread limits, and skinning).
 * 
 * @details The function performs the following updates:
 * - **Threading & History:** Forwards the cache to @c applyThreadCapsAndHistoryLimits.
 * - **UI State:** Sets @c displayConfig flags for list modes (full/compact) and filename visibility.
 * - **Visuals:** Updates the global @c skin and @c globalTheme; triggers @c getskin() to refresh color palettes.
 * - **Pagination:** Safely parses and updates @c ITEMS_PER_PAGE.
 * 
 * @param cache A map containing the key-value pairs to be applied to the system state.
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
        try { GlobalState::ITEMS_PER_PAGE = std::stoul(cache.at("pagination")); } catch (...) {}
    }
}

void helpSettingsEditor();
void syncCache(const std::string& filePath);

/**
 * @brief Launches the interactive Terminal User Interface (TUI) for configuration management.
 * 
 * Provides a structured, menu-driven environment where users can view current settings,
 * modify them by index, or reset the entire configuration to defaults.
 * 
 * @details **Workflow features:**
 * - **Sectioning:** Groups settings visually based on the sections defined in @c CONFIG_ORDERED_DEFAULTS.
 * - **Input Handling:** Utilizes GNU Readline for robust command input, supporting both single indices and tokenized multi-edits.
 * - **Validation:** Delegates to @c editSetting for per-key value validation and atomic updates.
 * - **Persistence:** Synchronizes with @c g_configCache. Note that a full reset triggers an immediate @c flushCache to disk upon confirmation.
 * - **Cleanup (RAII):** Employs a local @c SettingsGuard to ensure terminal keybindings are restored even if the loop breaks unexpectedly.
 * - **Signal Handling:** Temporarily ignores @c SIGINT to prevent accidental session termination during configuration.
 * 
 * @param configPath The filesystem path to the @c .conf file used for synchronization.
 */
void interactiveConfigEditor(const std::string& configPath) {
    struct SettingsGuard {
        ~SettingsGuard() { reset_custom_keybindingsForSettingsEditor(); }
    } guard;

    signal(SIGINT, SIG_IGN);
    syncCache(configPath);

    while (true) {
        clearScrollBuffer();
        verboseSets.uniqueErrorTokenMessages.clear();
        auto tc = resolveOptionsTheme();
        std::cout << "\n" << tc.highlight << "=== Settings Editor ===\n\n" << tc.reset;
        std::cout << tc.warning << "Config File: " << tc.reset << configPath << "\n";

        int index = 1;
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            if (!entry.section.empty()) {
                std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
            }
            std::string val = GlobalCaches::g_configCache.count(entry.key) ? GlobalCaches::g_configCache[entry.key] : entry.defaultValue;
            
            std::string_view valColor = (index == 1) ? color : std::string_view(tc.data);
            
            std::cout << tc.warning << std::right << std::setw(2) << index++ << ". " << tc.reset
                      << tc.label << std::left << std::setw(32) << entry.key << tc.reset
                      << "= " << tc.reset
                      << valColor << val << tc.reset << "\n";
        }
		setup_custom_keybindingsForSettingsEditor();
        std::cout << "\n" << tc.accent << "Actions: " << tc.warning << "1-" << (index-1) 
                  << tc.reset << " ↵ Edit | " << tc.warning << "r" << tc.reset << " Reset |" << tc.warning << " ?" << tc.reset << " help" << tc.reset << "\n";

        std::string prompt = std::format(
            "\n\001{}\002Action\001{}\002 ↵ | < \001{}\002Exit\001{}\002: \001{}\002",
            UI::Palette::Yellow,
            tc.label,
            UI::Palette::Red, 
            tc.label,
            tc.reset
        );

        std::unique_ptr<char, decltype(&std::free)> rawInput(readline(prompt.c_str()), &std::free);
        if (!rawInput) break;

        std::string input = trim(rawInput.get());
        if (input == "<") break;
        
        if (input == "?") {
            helpSettingsEditor();
            continue;
        }

        if (input == "r" || input == "R") {
			reset_custom_keybindingsForSettingsEditor();
            std::string confirmPrompt = std::format(
                "\001{}\002\nReset all settings to defaults? (y/n): \001{}\002",
                color, 
                UI::Palette::BoldReset
            );
            
            std::unique_ptr<char, decltype(&std::free)> confirmInput(readline(confirmPrompt.c_str()), &std::free);
            
            if (confirmInput) {
                std::string confirm = trim(confirmInput.get());
                if (confirm == "y" || confirm == "Y") {
                    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
                        GlobalCaches::g_configCache[e.key] = e.defaultValue;
                    }
                    applyConfigEffects(GlobalCaches::g_configCache);
                    tc = resolveOptionsTheme();
                    std::cout << tc.label << "[+] Defaults applied.\n" << tc.reset;
                    if (!confirm.empty() && std::tolower(confirm[0]) == 'y') {
						if (flushCache(configPath)) {
							tc = resolveOptionsTheme();
							std::cout << tc.highlight << "\n[✔] Settings saved to disk.\n" << tc.reset;
						}
						pressEnterToContinue();
					}
                }
            }
            continue;
        }

        {
            std::vector<std::string> slots(CONFIG_ORDERED_DEFAULTS.size());
            std::unordered_set<std::string> errors;
            std::unordered_set<int> choices;

            tokenizeInput(input, slots, choices);
            if (!verboseSets.uniqueErrorTokenMessages.empty()) std::cout << "\n";
            for (const auto& err : verboseSets.uniqueErrorTokenMessages)
                std::cout << err << "\n";
            if (!verboseSets.uniqueErrorTokenMessages.empty())
                pressEnterToContinue();

            std::vector<int> ordered(choices.begin(), choices.end());
            std::sort(ordered.begin(), ordered.end());
            for (int choice : ordered) {
                if (!editSetting(configPath, CONFIG_ORDERED_DEFAULTS[choice - 1].key)) {
                    break; 
                }
            }
        }
    }
}

/**
 * @brief Provides an interactive CLI interface to modify a specific configuration setting.
 * 
 * Displays current values, descriptions, and dynamic input hints. The function 
 * loops until valid input is received, the change is skipped, or the session is aborted.
 * 
 * @details **Workflow features:**
 * - **Key Discovery:** Locates the @c ConfigEntry definition for metadata and default values.
 * - **Dynamic UX:** Displays context-aware hints for themes, skins, and numeric ranges.
 * - **Validation:** Enforces integrity via the entry's @c validate lambda before acceptance.
 * - **Hot-Reloading:** Updates @c g_configCache and immediately triggers @c applyConfigEffects 
 *   to reflect changes in the running session (e.g., instant theme switching).
 * - **Persistence:** Attempts to persist changes via @c flushCache. If disk I/O fails, 
 *   the user is notified that the change is "memory-only."
 * - **State Refresh:** Triggers @c sortAfterFilenamesOnlyFlag if the filenames display 
 *   toggle is modified to ensure UI consistency.
 * 
 * @param configPath Path to the @c .conf file for persistence.
 * @param key The unique configuration key to be modified.
 * @return true if the setting was updated, matched the current value, or was skipped.
 * @return false if the user aborted the prompt (e.g., via Ctrl+D).
 */
bool editSetting(const std::string& configPath, const std::string& key) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) { 
        if (e.key == key) { entry = &e; break; } 
    }
    if (!entry) return true;
    reset_custom_keybindingsForSettingsEditor();

    // Move current value outside the loop so we can compare changes
    std::string current = GlobalCaches::g_configCache.count(key) ? GlobalCaches::g_configCache[key] : entry->defaultValue;

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
			int min = 1, max = 256;
			if (key == "pagination")                          { min = 0;  max = 1000; }
			else if (key == "folder_path_history_lines")      { min = 0;  max = 5000; }
			else if (key == "filter_history_lines")           { min = 0;  max = 1000; }
			else if (key == "combined_thread_cap")            { min = 1;  max = 256;  }
			else if (key == "thread_cap_for_mount")           { min = 1;  max = 128;  }
			else if (key == "thread_cap_for_umount")          { min = 1;  max = 128;  }
			else if (key == "thread_cap_for_cp_mv")           { min = 1;  max = 128;  }
			else if (key == "thread_cap_for_rm")              { min = 1;  max = 128;  }
			else if (key == "thread_cap_for_convert2iso")     { min = 1;  max = 128;  }
			else if (key == "thread_cap_for_database_cleanup"){ min = 1;  max = 128;  }
			else if (key == "thread_cap_for_list_sorting")    { min = 1;  max = 64;   }
			else if (key == "thread_cap_for_list_filtering")  { min = 1;  max = 64;   }

			std::cout << "numeric value (min - max: " << min << " - " << max << ")\n";
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
        GlobalCaches::g_configCache[key] = newVal;
        applyConfigEffects(GlobalCaches::g_configCache);
        
        // Use the standardized helper to write to disk and handle errors
        bool saved = flushCache(configPath);
        
        // Refresh the local theme context in case 'skin' or 'theme' was changed
        tc = resolveOptionsTheme();

        if (saved) {
            std::cout << std::format("\n{}✓ Updated and saved to: {}{}\n", 
                                     tc.label, tc.reset, configPath);
        } else {
            std::cout << tc.warning << "Notice: " << tc.error 
                      << "Setting update is memory-only (Disk write failed)." 
                      << tc.reset << "\n";
        }
        
        // Specific side-effect for filename display toggle
        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }

        pressEnterToContinue();
        break;
	}
	return true;
}
