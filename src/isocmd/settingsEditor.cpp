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
#include "../rendering.h"
#include "../pausePrompts.h"
#include "../readline.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"
#include "../verbose.h"
#include "../tokenize.h"

/**
 * @brief Prints the standard "configuration file inaccessible" error.
 *
 * @param configPath Path of the configuration file to name in the message.
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
 * @param configPath Path of the configuration file to write.
 * @return true on success; false on write failure (after printing the error).
 */
bool flushCache(const std::string& configPath) {
    if (writeConfig(configPath, ConfigCaches::g_configCache)) return true;
    printConfigError(configPath);
    return false;
}

/**
 * @brief Applies cached configuration values to the runtime state.
 *
 * Updates thread and history limits, list-display options, the selected skin
 * and theme, the default input color, pagination, and the live-filter limit.
 * Changing the skin also refreshes the derived color palette. Numeric values
 * that cannot be parsed leave the corresponding runtime setting unchanged.
 *
 * @param cache Configuration key-value pairs to apply.
 */
void applyConfigEffects(const std::map<std::string, std::string>& cache) {
    applyThreadCapsAndHistoryLimits(cache);

    if (cache.count("mount_list"))        displayConfig::toggleFullListMount = (cache.at("mount_list") == "full");
    if (cache.count("umount_list"))       displayConfig::toggleFullListUmount = (cache.at("umount_list") == "full");
    if (cache.count("cp_mv_rm_list"))     displayConfig::toggleFullListCpMvRm = (cache.at("cp_mv_rm_list") == "full");
    if (cache.count("write2usb_list"))    displayConfig::toggleFullListWrite2usb = (cache.at("write2usb_list") == "full");
    if (cache.count("convert2iso_lists")) displayConfig::toggleFullListConvert2iso = (cache.at("convert2iso_lists") == "full");
    if (cache.count("filenames_only"))    displayConfig::toggleNamesOnly = (cache.at("filenames_only") == "on");

    if (cache.count("skin")) { skin = cache.at("skin"); color = getskin(); }
    if (cache.count("theme")) { globalTheme = cache.at("theme"); }

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
 * @brief Runs the interactive configuration editor.
 *
 * Displays settings grouped by section and lets the user edit entries by
 * index or restore all defaults.
 *
 * - **Input:** GNU Readline accepts individual selections and tokenized
 *   multi-selections; per-setting validation is handled by @c editSetting.
 * - **Persistence:** edits update the in-memory cache and runtime state, then
 *   attempt to write the configuration file. Resetting all settings follows
 *   the same sequence after confirmation.
 * - **Terminal cleanup:** a local RAII guard restores the settings-editor
 *   keybindings on every exit path.
 * - **Signals:** SIGINT is ignored while the editor is active.
 * - **Resize handling:** every screen shares @c paintSettingsBody, so a
 *   resize repaints the full settings view rather than just the last
 *   message. @c drawSettings is the repaint callback for both the main
 *   prompt and the reset confirmation, and @c render serves the post-reset
 *   status screen. Once a reset is requested, the screen is repainted
 *   without the main prompt line, and neither that line nor the
 *   confirmation line is re-echoed afterwards.
 *
 * @param configPath Configuration file used to load and save settings.
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

        // Single definition of the settings screen body (header + table +
        // actions line). Used by both the normal draw and the post-reset
        // repaint, so a resize reconstructs the whole screen rather than
        // just the status footer.
        auto paintSettingsBody = [&] {
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

        auto drawSettings = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            paintSettingsBody();
        };

        drawSettings();

        setup_custom_keybindingsForSettingsEditor();

        std::string prompt = std::format(
            "\n{}IDX\001{}\002 ↵ for {}editor\001{}\002, ? for help: \001{}\002",
            UI::Palette::RL_idxGreen,
            tc.highlight,
            UI::Palette::RL_Yellow,
            tc.highlight,
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
                "\n\001{}\002Reset all settings to defaults? (y/n): \001{}\002",
                color,
                UI::Palette::BoldReset
            );

            // Readline's \001/\002 non-printing markers stripped, since we print
            // the confirm line ourselves in the post-reset repaint.
            auto stripRlMarkers = [](std::string s) {
                s.erase(std::remove_if(s.begin(), s.end(),
                                       [](char c) { return c == '\001' || c == '\002'; }),
                        s.end());
                return s;
            };
            std::string confirmPromptEcho = stripRlMarkers(confirmPrompt);

            // Wipe the IDX prompt line now so the screen looks the same before and
            // after a resize: settings body, then the y/n prompt directly beneath.
            drawSettings();

            armPendingInputRestore();
            RlLine confirmInput = readlineWithResize(confirmPrompt, drawSettings);
            rl_startup_hook = nullptr;

            if (confirmInput) {
                std::string confirm = trim(confirmInput.get());
                if (confirm == "y" || confirm == "Y") {
                    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
                        ConfigCaches::g_configCache[e.key] = e.defaultValue;
                    }
                    applyConfigEffects(ConfigCaches::g_configCache);
                    tc = resolveOptionsTheme();

                    bool saved = false;
                    {
                        SyncOutput sync;
                        saved = flushCache(configPath);
                    }

                    auto render = [&] {
                        SyncOutput sync;
                        clearScrollBuffer();
                        paintSettingsBody();
                        std::cout << "\n" << tc.label << "[+] Defaults applied\n" << tc.reset;
                        if (saved) {
                            std::cout << tc.highlight << "[✔] Saved to disk\n" << tc.reset;
                        }
                    };

                    pressEnterToContinueWithRepaint(render);
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
            if (!verboseSets.uniqueErrorTokenMessages.empty()) {
                auto render = [&] {
                    SyncOutput sync;
                    clearScrollBuffer();
                    std::cout << "\n";
                    for (const auto& err : verboseSets.uniqueErrorTokenMessages)
                        std::cout << err << "\n";
                };
                pressEnterToContinueWithRepaint(render);
            }

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
 * @brief Interactively edits one configuration setting.
 *
 * Displays the current value, its description, and valid-value guidance.
 * Invalid input returns to the prompt; an empty input or unchanged value
 * skips the edit.
 *
 * - **Metadata:** finds the matching @c ConfigEntry for its description,
 *   default, and validation callback.
 * - **Validation:** accepts a new value only when the entry's validator
 *   approves it.
 * - **Runtime update:** stores the accepted value in @c g_configCache and
 *   calls @c applyConfigEffects immediately, so the change takes effect
 *   without restarting the application.
 * - **Persistence:** attempts to save the updated cache with @c flushCache.
 *   If writing fails, the runtime change remains active but is memory-only.
 * - **Display refresh:** changing @c filenames_only also calls
 *   @c sortAfterFilenamesOnlyFlag.
 * - **Resize handling:** @c drawEdit is reused for the value prompt and for
 *   the invalid-value and success messages, keeping the edit screen visible
 *   when the terminal is resized during either input or confirmation.
 *
 * @param configPath Configuration file used to persist the change.
 * @param key        Configuration key to edit.
 * @return true if the value was updated or the edit was skipped; false if the
 *         user aborted with Ctrl+D or Esc.
 */
bool editSetting(const std::string& configPath, const std::string& key) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
        if (e.key == key) { entry = &e; break; }
    }
    if (!entry) return true;
    reset_custom_keybindingsForSettingsEditor();

    std::string current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : entry->defaultValue;

    auto drawEdit = [&] {
        clearScrollBuffer();
        std::cout << "\n" << tc.highlight << "=== Edit Setting ===\n\n" << tc.reset;
        std::cout << tc.label << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
        std::cout << tc.label << "Current: " << tc.reset << tc.highlight << current << tc.reset << "\n";
        std::string displayComment = entry->comment;
        size_t pos = displayComment.find("\n# ");
        if (pos != std::string::npos) displayComment.replace(pos, 3, "\n             ");
        std::cout << "\nDescription: " << displayComment << tc.reset << "\n\n";

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
        std::cout << "\n";

        std::string prompt = std::format(
            "\001{}\002Value\001{}\002 ↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
            UI::Palette::Green,
            tc.label,
            UI::Palette::Yellow,
            tc.reset,
            tc.reset
        );

        RlLine rawInput = readlineWithResize(prompt, drawEdit);

        if (!rawInput || rawInput.get()[0] == 27) return false;
        std::string newVal = trim(rawInput.get());
        if (newVal.empty() || newVal == current) return true;

        if (entry->validate && !entry->validate(newVal)) {
            auto render = [&] {
                SyncOutput sync;
                drawEdit();
                std::cout << "\n" << tc.error << "Invalid value: "
                          << tc.warning << "'" << newVal << "'" << tc.reset << "\n";
            };
            pressEnterToTryWithRepaint(render);
            continue;
        }

        ConfigCaches::g_configCache[key] = newVal;
        applyConfigEffects(ConfigCaches::g_configCache);

        bool saved = flushCache(configPath);

        tc = resolveOptionsTheme();

        auto render = [&] {
            SyncOutput sync;
            drawEdit();
            if (saved) {
                std::cout << std::format("\n{}✓ Updated and saved to: {}{}\n",
                                         tc.label, tc.reset, configPath);
            } else {
                std::cout << tc.warning << "Notice: " << tc.error
                          << "Setting update is memory-only (Disk write failed)."
                          << tc.reset << "\n";
            }
        };
        pressEnterToContinueWithRepaint(render);

        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }

        break;
    }
    return true;
}
