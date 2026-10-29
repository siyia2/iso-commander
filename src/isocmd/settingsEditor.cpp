// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../inputHandling.h"
#include "../listResizeWatcher.h"
#include "../pausePrompt.h"
#include "../readline.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"
#include "../verbose.h"
#include "../tokenize.h"

/**
 * DISK I/O & ERROR REPORTING
 */
void printConfigError(const std::string& configPath) {
    auto [label, accent, warning, error, reset, path, highlight, data, str] = resolveOptionsTheme();
    std::cerr << "\n" << error
              << "Error: Unable to access configuration file: "
              << warning << "'" << configPath << "'"
              << error << ".\033[J\n" << reset;
}

/**
 * @brief Commits the in-memory configuration cache to disk.
 *
 * @return true on success; false on write failure (after printing the error).
 */
bool flushCache(const std::string& configPath) {
    if (writeConfig(configPath, ConfigCaches::g_configCache)) return true;
    printConfigError(configPath);
    return false;
}

/**
 * @brief Applies the configuration cache to the global runtime state.
 *
 * Bridge between the raw string @c g_configCache and the functional variables
 * the app actually uses (UI toggles, thread limits, skinning).
 *
 * Updates in order: thread caps and history limits, list-display toggles
 * (full/compact, filenames-only), skin and theme (via @c getskin() to refresh
 * the color palette), the default input color, pagination, and the live-filter
 * limit. Numeric settings are parsed with a @c try and silently kept at their
 * previous value on parse failure.
 *
 * @param cache The key-value pairs to apply.
 */
void applyConfigEffects(const std::map<std::string, std::string>& cache) {
    // Thread caps and history
    applyThreadCapsAndHistoryLimits(cache);

    // UI toggles
    if (cache.count("mount_list"))        displayConfig::toggleFullListMount = (cache.at("mount_list") == "full");
    if (cache.count("umount_list"))       displayConfig::toggleFullListUmount = (cache.at("umount_list") == "full");
    if (cache.count("cp_mv_rm_list"))     displayConfig::toggleFullListCpMvRm = (cache.at("cp_mv_rm_list") == "full");
    if (cache.count("write2usb_list"))    displayConfig::toggleFullListWrite2usb = (cache.at("write2usb_list") == "full");
    if (cache.count("convert2iso_lists")) displayConfig::toggleFullListConvert2iso = (cache.at("convert2iso_lists") == "full");
    if (cache.count("filenames_only"))    displayConfig::toggleNamesOnly = (cache.at("filenames_only") == "on");

    // Appearance
    if (cache.count("skin")) { skin = cache.at("skin"); color = getskin(); }
    if (cache.count("theme")) { globalTheme = cache.at("theme"); }

    // Default input color
    if (cache.count("base")) {
        UI::Palette::defaultColor = cache.at("base");
        UI::Palette::updateDefaultColors();
    }

    if (cache.count("pagination")) {
        try { GlobalState::ITEMS_PER_PAGE = std::stoul(cache.at("pagination")); } catch (...) {}
    }
    if (cache.count("live_filter_limit")) {
        try { GlobalState::LIVE_FILTER_LIMIT = std::stoul(cache.at("live_filter_limit")); } catch (...) {}
    }
}

void helpSettingsEditor();
void syncCache(const std::string& filePath);

/**
 * @brief Launches the interactive TUI for configuration management.
 *
 * Menu-driven environment for viewing current settings, editing them by index,
 * or resetting the whole configuration to defaults.
 *
 * - **Sectioning:** Groups settings by the sections in
 *   @c CONFIG_ORDERED_DEFAULTS.
 * - **Input handling:** GNU Readline; supports single indices and tokenized
 *   multi-edits.
 * - **Validation:** delegated to @c editSetting per key.
 * - **Persistence:** synchronized with @c g_configCache; a full reset triggers
 *   an immediate @c flushCache on confirmation.
 * - **Cleanup (RAII):** a local @c SettingsGuard restores terminal keybindings
 *   on every exit path.
 * - **Signals:** @c SIGINT is ignored during the session.
 * - **Resize repaint:** the screen is drawn by a lambda (@c drawSettings) that
 *   runs once per iteration and is also passed to @c readlineWithResize() for
 *   both the main prompt and the reset confirmation prompt, so the screen
 *   repaints on terminal-width changes while a prompt waits. The lambda ends
 *   on the line after "Actions:", where each prompt's leading newline expects
 *   the cursor.
 *
 * @param configPath Path to the @c .conf file used for synchronization.
 */
void interactiveConfigEditor(const std::string& configPath) {
    PendingReadlineTextClearGuard pendingTextGuard;

    struct SettingsGuard {
        ~SettingsGuard() { reset_custom_keybindingsForSettingsEditor(); }
    } guard;

    signal(SIGINT, SIG_IGN);
    syncCache(configPath);

    while (true) {
        verboseSets.uniqueErrorTokenMessages.clear();
        auto tc = resolveOptionsTheme();

        // Draws the whole settings screen. Used for the normal draw and as the
        // resize repaint (runs on the Readline thread; must not call readline()).
        // `tc` is captured by reference, so a theme refresh in the reset branch
        // is picked up by later repaints.
        auto drawSettings = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << "\n" << tc.highlight << "=== Settings Editor ===\n\n" << tc.reset;
            std::cout << tc.warning << "Config File: " << tc.reset << configPath << "\n";

            int index = 1;
            for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
                if (!entry.section.empty()) {
                    std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
                }
                std::string val = ConfigCaches::g_configCache.count(entry.key) ? ConfigCaches::g_configCache[entry.key] : entry.defaultValue;

                std::string_view rowColor = (index == 1) ? UI::Palette::BoldReset :
                                            ((index == 2) ? color : tc.data);

                std::cout << tc.warning << std::right << std::setw(2) << index++ << ". " << tc.reset
                          << tc.label << std::left << std::setw(32) << entry.key << tc.reset
                          << "= " << tc.reset
                          << rowColor << val << tc.reset << "\n";
            }
            std::cout << "\n" << tc.accent << "Actions: " << tc.warning << "1-" << (index-1)
                      << tc.reset << " ↵ Edit | " << tc.warning << "r" << tc.reset << " Reset\n";
        };

        drawSettings();

        setup_custom_keybindingsForSettingsEditor();

        std::string prompt = std::format(
            "\n{}IDX\001{}\002 ↵ for {}editor\001{}\002, ? for help: \001{}\002",
            UI::Palette::RL_idxGreen,
            tc.label,
            UI::Palette::RL_Yellow,
            tc.label,
            tc.reset
        );
        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, drawSettings);
        rl_startup_hook = nullptr;

        if (!rawInput) break;

        std::string input = trim(rawInput.get());
        if (input == "\x1b") break;

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

            // The settings list is still on screen and the prompt starts with a
            // newline, so the same repaint applies here.
            RlLine confirmInput = readlineWithResize(confirmPrompt, drawSettings);

            if (confirmInput) {
                std::string confirm = trim(confirmInput.get());
                if (confirm == "y" || confirm == "Y") {
                    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
                        ConfigCaches::g_configCache[e.key] = e.defaultValue;
                    }
                    applyConfigEffects(ConfigCaches::g_configCache);
                    tc = resolveOptionsTheme();
                    {
                        SyncOutput sync;
                        std::cout << tc.label << "[+] Defaults applied.\n" << tc.reset;
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
 * @brief Interactive CLI for modifying a single configuration setting.
 *
 * Shows the current value, the description, and a value hint. Loops until
 * valid input is received, the change is skipped, or the session is aborted.
 *
 * - **Key discovery:** looks up the @c ConfigEntry for metadata and defaults.
 * - **Dynamic UX:** context-aware hints for themes, skins, and numeric ranges.
 * - **Validation:** via the entry's @c validate lambda before acceptance.
 * - **Hot-reload:** updates @c g_configCache and immediately calls
 *   @c applyConfigEffects so changes take effect in the running session (e.g.
 *   instant theme switching).
 * - **Persistence:** via @c flushCache; on disk-write failure the user is told
 *   the change is "memory-only".
 * - **State refresh:** @c sortAfterFilenamesOnlyFlag is called when the
 *   filenames-only toggle changes.
 * - **Resize repaint:** the screen is drawn by a lambda (@c drawEdit) that is
 *   also passed to @c readlineWithResize(), so it repaints on terminal-width
 *   changes while the prompt waits. The lambda stops at the last hint line,
 *   one line short of the blank line before the prompt (this prompt has no
 *   leading newline): the normal path prints that blank line itself, and the
 *   resize watcher prints it after a repaint.
 *
 * @param configPath Path to the @c .conf file for persistence.
 * @param key        Configuration key to modify.
 * @return true if the setting was updated, matched the current value, or was
 *         skipped; false if the user aborted (Ctrl+D or Esc).
 */
bool editSetting(const std::string& configPath, const std::string& key) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
        if (e.key == key) { entry = &e; break; }
    }
    if (!entry) return true;
    reset_custom_keybindingsForSettingsEditor();

    // Keep the current value outside the loop so we can compare changes.
    std::string current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : entry->defaultValue;

    // Draws the edit screen up to the last hint line (NOT the blank line
    // before the prompt). Used for the first draw and as the resize repaint;
    // the blank line comes from the caller / the watcher.
    auto drawEdit = [&] {
        clearScrollBuffer();
        std::cout << "\n" << tc.highlight << "=== Edit Setting ===\n\n" << tc.reset;
        std::cout << tc.label << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
        std::cout << tc.label << "Current: " << tc.reset << tc.highlight << current << tc.reset << "\n";
        std::string displayComment = entry->comment;
        size_t pos = displayComment.find("\n# ");
        if (pos != std::string::npos) displayComment.replace(pos, 3, "\n             ");
        std::cout << "\nDescription: " << displayComment << tc.reset << "\n\n";

        // --- Dynamic hint block ---
        std::cout << tc.label << "Valid values: " << tc.reset;
        if (key == "base") {
            std::cout << "charcoal, black, gray, silver, ash, white, steel\n";
        } else if (key == "skin") {
            std::cout << "green, cyan, white, purple, amber, rose, gray, black\n";
        } else if (key == "theme") {
            std::cout << "original, classic, high_contrast, neon, ocean, sunset, forest,\n"
                      << "               midnight, mono, retro, crimson, dracula, tokyo, paper, sakura\n";
        } else if (key == "auto_update" || key == "filenames_only") {
            std::cout << "on, off\n";
        } else if (key == "pagination" || key == "live_filter_limit" || key.find("thread_cap") != std::string::npos || key.find("_lines") != std::string::npos) {
            int min = 1, max = 256;
            if (key == "live_filter_limit")                   { min = 0;  max = 100000; }
            if (key == "pagination")                          { min = 0;  max = 1000;  }
            else if (key == "folder_path_history_lines")      { min = 0;  max = 5000;  }
            else if (key == "filter_history_lines")           { min = 0;  max = 1000;  }
            else if (key == "combined_thread_cap")            { min = 1;  max = 256;   }
            else if (key == "thread_cap_for_mount")           { min = 1;  max = 128;   }
            else if (key == "thread_cap_for_umount")          { min = 1;  max = 128;   }
            else if (key == "thread_cap_for_cp_mv")           { min = 1;  max = 128;   }
            else if (key == "thread_cap_for_rm")              { min = 1;  max = 128;   }
            else if (key == "thread_cap_for_convert2iso")     { min = 1;  max = 128;   }
            else if (key == "thread_cap_for_database_cleanup"){ min = 1;  max = 128;   }
            else if (key == "thread_cap_for_list_sorting")    { min = 1;  max = 64;    }
            else if (key == "thread_cap_for_list_filtering")  { min = 1;  max = 64;    }

            std::cout << "numeric value (min - max: " << min << " - " << max << ")\n";
        } else if (key.find("_list") != std::string::npos) {
            std::cout << "full, compact\n";
        } else {
            std::cout << "Refer to description above\n";
        }
    };

    while (true) {
        rl_bind_keyseq("\\e", exit_handler);
        drawEdit();
        std::cout << "\n";   // blank line before the prompt

        std::string prompt = std::format(
            "\001{}\002Value\001{}\002 ↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
            UI::Palette::Green,
            tc.label,
            UI::Palette::Yellow,
            tc.reset,
            tc.reset
        );

        RlLine rawInput = readlineWithResize(prompt, drawEdit);

        // Cancel (Ctrl+D or Esc).
        if (!rawInput || rawInput.get()[0] == 27) return false;
        std::string newVal = trim(rawInput.get());
        if (newVal.empty() || newVal == current) return true;

        // Validation.
        if (entry->validate && !entry->validate(newVal)) {
            std::cout << "\n" << tc.error << "Invalid value: " << tc.warning << "'" << newVal << "'" << tc.reset << "\n";
            pressEnterToTry();
            continue;
        }

        // --- Success case ---
        ConfigCaches::g_configCache[key] = newVal;
        applyConfigEffects(ConfigCaches::g_configCache);

        bool saved = flushCache(configPath);

        // Refresh the local theme context in case 'skin' or 'theme' changed.
        tc = resolveOptionsTheme();

        if (saved) {
            std::cout << std::format("\n{}✓ Updated and saved to: {}{}\n",
                                     tc.label, tc.reset, configPath);
        } else {
            std::cout << tc.warning << "Notice: " << tc.error
                      << "Setting update is memory-only (Disk write failed)."
                      << tc.reset << "\n";
        }

        // Specific side-effect for the filenames display toggle.
        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }

        pressEnterToContinue();
        break;
    }
    return true;
}
