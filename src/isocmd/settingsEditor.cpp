// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <functional>
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
 * @brief Result of a single @c editSetting screen.
 *
 * - @c Advance: the edit was skipped with a blank Enter (or the setting has
 *               no matching entry, so there is nothing to edit). The setting
 *               is removed from the selection (and therefore from the
 *               PgUp / PgDn / g# loop), exactly like @c Updated.
 * - @c Updated: a value was accepted and saved (re-entering the current value
 *               counts too); the setting is removed
 *               from the selection (and therefore from the PgUp / PgDn / g#
 *               loop), then the loop moves on to the setting that followed it
 *               (wrapping to the first pending one) and finishes only when
 *               nothing is left.
 * - @c Next:    PgDn was pressed; go to the next selected setting.
 * - @c Prev:    PgUp was pressed; go to the previous selected setting.
 * - @c GoTo:    "g#" was entered; jump to selected setting number # (see
 *               @c g_editGotoPos). A target equal to the current position is
 *               treated like an invalid target and simply redraws.
 * - @c Abort:   Esc or Ctrl+D; leave the whole multi-edit sequence.
 */
enum class EditNav { Advance, Updated, Next, Prev, GoTo, Abort };

// Target position (1-based, within the current selection) for EditNav::GoTo.
static size_t g_editGotoPos = 0;

/**
 * @brief Legacy Settings Editor repaint store (kept for local consumers).
 *
 * @warning This anonymous-namespace variable and the two accessors below are
 *          @b not what the completion subsystem uses. The live repaint hook
 *          for the completion path is the @c SettingsEditorRepaint::g_fn
 *          object declared @c inline in @c readline.h, which
 *          @c clear_screen_and_buffer consults directly. The accessor pair
 *          here predates that namespace and currently has no callers; it is
 *          retained only as historical scaffolding and should be removed if
 *          nothing else starts using it.
 *
 * The design note that follows the original block ("using accessor functions
 * instead of a shared extern keeps the @c std::function out of any header")
 * no longer applies: the active design does expose the @c std::function
 * through @c readline.h.
 */
namespace {
std::function<void()> g_activeSettingsRepaint;
} // namespace

/**
 * @brief Returns the legacy repaint callback, or an empty @c std::function.
 *
 * @deprecated Not consulted by @c clear_screen_and_buffer; that function uses
 *             @c SettingsEditorRepaint::g_fn instead. Kept only because the
 *             accessor pair is still compiled here; no live caller exists.
 */
const std::function<void()>& getActiveSettingsRepaint() {
    return g_activeSettingsRepaint;
}

/**
 * @brief Installs (or clears, with an empty function) the legacy repaint hook.
 *
 * @deprecated Not called by @c readlineWithRepaint(); that helper calls
 *             @c SettingsEditorRepaint::set(...) instead. Kept only for the
 *             accessor pair's symmetry; no live caller exists.
 */
void setActiveSettingsRepaint(std::function<void()> fn) {
    g_activeSettingsRepaint = std::move(fn);
}

/**
 * @brief Like @c readlineWithResize, but also routes <kbd>Ctrl+L</kbd> clears
 *        through @p repaint.
 *
 * Installs @p repaint as the active body-repaint hook
 * (@c SettingsEditorRepaint::set) for the duration of the
 * @c readlineWithResize call and clears it immediately after, so
 * @c clear_screen_and_buffer (triggered by <kbd>Ctrl+L</kbd>) repaints the
 * full setting screen — header, current value, description, valid values,
 * navigation footer — instead of leaving only the input prompt on screen.
 * @p repaint is also passed to @c readlineWithResize unchanged so
 * terminal-resize repaints keep working.
 *
 * @param prompt  The readline prompt string.
 * @param repaint Full-screen body repaint callback for this prompt.
 * @return The line read by readline, or an empty @c RlLine on EOF / abort.
 */
static RlLine readlineWithRepaint(const std::string& prompt,
                                  const std::function<void()>& repaint) {
    SettingsEditorRepaint::set(repaint);
    RlLine line = readlineWithResize(prompt, repaint);
    SettingsEditorRepaint::set(nullptr);
    return line;
}

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

// Existing handlers from readline.cpp: they replace the line with "PgUp" /
// "PgDn", stash the typed text, and submit via rl_newline (which sets
// rl_done). The stash is cleared by editSetting() after either key is
// observed, so it cannot leak to the next screen. (Redeclared here in case
// readline.h doesn't expose them.)
int pgup_handler(int, int);
int pgdn_handler(int, int);
EditNav editSetting(const std::string& configPath, const std::string& key,
                    const std::vector<std::string>& inputErrors,
                    size_t pos, size_t total);

/**
 * @brief Tab handler that does nothing.
 *
 * Bound to Tab on numeric settings so completion is fully disabled there.
 * Merely clearing @c rl_attempted_completion_function would not be enough:
 * readline would fall back to its default filename completion.
 */
static int noop_tab_handler(int, int) { return 0; }

/**
 * @brief Runs the interactive configuration editor.
 *
 * Displays settings grouped by section and lets the user edit entries by
 * index or restore all defaults.
 *
 * - **Input:** GNU Readline accepts individual selections and tokenized
 *   multi-selections; per-setting validation is handled by @c editSetting.
 * - **Multi-selection navigation:** when several settings are selected, they
 *   are edited one screen at a time. A blank Enter skips the current setting
 *   and a valid value (even one equal to the current value) is saved; either
 *   way the setting is removed from the loop. The loop ends on Esc / Ctrl+D
 *   or once every selected setting has been updated or skipped. PgDn / PgUp
 *   move to the next / previous selected setting without wrapping: at either
 *   end the key is ignored and the matching footer hint is hidden.
 * - **Handled settings leave the loop:** once a setting has been updated
 *   (@c EditNav::Updated) or skipped (@c EditNav::Advance) it is removed from
 *   the selection, so the "(n/total)" indicator, PgUp / PgDn navigation and
 *   "g#" numbering only cover the settings that have not been handled yet.
 *   After either result the loop continues with the setting that followed
 *   the removed one (wrapping to the first remaining one) and ends only when
 *   no setting is left. Settings reached only by navigation (PgUp / PgDn /
 *   g#) stay in the loop.
 * - **Input errors:** token errors from a multi-selection are passed to the
 *   first @c editSetting call and printed above its "Edit Setting" header.
 *   If the input produced only errors and no valid selection, the errors are
 *   listed followed by "No valid input provided", and the editor returns to
 *   the main prompt.
 * - **Persistence:** edits update the in-memory cache and runtime state, then
 *   attempt to write the configuration file. Resetting all settings follows
 *   the same sequence after confirmation.
 * - **Terminal cleanup:** a local RAII guard restores the settings-editor
 *   keybindings on every exit path.
 * - **Signals:** SIGINT is set to @c SIG_IGN for the duration of the editor.
 *   The previous disposition is @b not restored on exit; this matches the
 *   program-wide policy of ignoring Ctrl+C once the interactive UI is up.
 * - **Resize handling:** every screen shares @c paintSettingsBody, so a
 *   resize repaints the full settings view rather than just the last
 *   message. @c drawSettings is the repaint callback for both the main
 *   prompt and the reset confirmation, and @c render serves the post-reset
 *   status screen and the "No valid input provided" screen. Once a reset is
 *   requested, the screen is repainted without the main prompt line, and
 *   neither that line nor the confirmation line is re-echoed afterwards.
 * - **Clear handling:** <kbd>Ctrl+L</kbd> is routed through
 *   @c readlineWithRepaint, so it repaints the full settings body via
 *   @c drawSettings instead of leaving a bare input prompt.
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
            tc.path,
            UI::Palette::RL_Yellow,
            tc.path,
            tc.reset
        );
        armPendingInputRestore();
        RlLine rawInput = readlineWithRepaint(prompt, drawSettings);
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
            RlLine confirmInput = readlineWithRepaint(confirmPrompt, drawSettings);
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
                        } else {
                            std::cout << tc.error << "[✗] Save failed\n" << tc.reset;
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

            // Snapshot of the token errors so they can be handed to editSetting
            // and survive repaint callbacks.
            const std::vector<std::string> inputErrors(
                verboseSets.uniqueErrorTokenMessages.begin(),
                verboseSets.uniqueErrorTokenMessages.end());

            // Only errors, nothing valid to edit: report and return to the main prompt.
            if (choices.empty()) {
                if (!inputErrors.empty()) {
                    auto render = [&] {
                        SyncOutput sync;
                        clearScrollBuffer();
                        std::cout << "\n" << tc.error << "No valid input provided"
                                  << tc.reset << "\n";
                    };
                    pressEnterToContinueWithRepaint(render);
                }
                continue;
            }

            // Valid choices exist: errors (if any) are shown above the first
            // edit screen only.
            const std::vector<std::string> noErrors;
            std::vector<int> ordered(choices.begin(), choices.end());
            std::sort(ordered.begin(), ordered.end());

            size_t i = 0;
            bool showErrors = true;          // token errors only on the first screen
            bool done = false;
            while (!done) {
                // Recomputed every iteration: updated settings are erased from
                // `ordered`, so the total shown in "(n/total)" and used for
                // PgUp/PgDn/g# shrinks accordingly.
                const size_t n = ordered.size();
                EditNav r = editSetting(configPath,
                                        CONFIG_ORDERED_DEFAULTS[ordered[i] - 1].key,
                                        showErrors ? inputErrors : noErrors,
                                        i + 1, n);
                showErrors = false;

                switch (r) {
                    case EditNav::Advance:   // skipped: dropped just like an update
                    case EditNav::Updated:
                        // Skipped and updated settings both leave the
                        // PgUp/PgDn/g# loop. The next setting slides into
                        // index i, so don't increment; past the end, wrap to
                        // the first pending setting. The loop ends only once
                        // nothing is left to edit.
                        ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(i));
                        if (ordered.empty()) done = true;
                        else if (i >= ordered.size()) i = 0;
                        break;
                    case EditNav::Next:    if (i + 1 < n) ++i;        break;
                    case EditNav::Prev:    if (i > 0)     --i;        break;
                    case EditNav::GoTo:    if (g_editGotoPos >= 1 && g_editGotoPos <= n) i = g_editGotoPos - 1; break;
                    case EditNav::Abort:   done = true;               break;
                }
            }
        }
    }
}

/**
 * @brief Interactively edits one configuration setting.
 *
 * Displays the current value, its description, and valid-value guidance.
 * Invalid input returns to the prompt; a blank input skips the edit (the
 * caller drops the setting from the selection). Entering the current value
 * again counts as an update.
 *
 * - **Metadata:** finds the matching @c ConfigEntry for its description,
 *   default, and validation callback.
 * - **Input errors:** any @c inputErrors supplied by the caller are printed
 *   above the "Edit Setting" header on every redraw of this screen.
 * - **Validation:** accepts a new value only when the entry's validator
 *   approves it.
 * - **Runtime update:** stores the accepted value in @c g_configCache and
 *   calls @c applyConfigEffects immediately, so the change takes effect
 *   without restarting the application.
 * - **Persistence:** attempts to save the updated cache with @c flushCache.
 *   If writing fails, the runtime change remains active but is memory-only.
 * - **Display refresh:** changing @c filenames_only also calls
 *   @c sortAfterFilenamesOnlyFlag.
 * - **Navigation:** when @p total is greater than one, PgDn / PgUp are bound
 *   for the lifetime of this call and return @c EditNav::Next / @c Prev so the
 *   caller can step through the selected settings (no wrap-around; at the
 *   first/last setting the key just redraws this screen). The bindings are
 *   restored on every exit path. An "(n/total)" indicator is shown in the
 *   header and a "[PgUp] Prev | [PgDn] Next | [g#] ↵ GoTo" footer lists the
 *   available actions. Entering "g#" jumps to the #-th selected setting
 *   (@c EditNav::GoTo); an invalid or same-screen target just redraws.
 * - **Resize handling:** @c drawEdit is reused for the value prompt and for
 *   the invalid-value and success messages, keeping the edit screen (and any
 *   input errors above it) visible when the terminal is resized during
 *   either input or confirmation.
 * - **Clear handling:** <kbd>Ctrl+L</kbd> is routed through
 *   @c readlineWithRepaint, so it repaints the full edit screen via
 *   @c drawEdit instead of leaving only the "Value …" prompt on screen.
 * - **Numeric settings:** Tab completion is disabled for numeric settings
 *   (@c pagination, @c live_filter_limit, and any key containing
 *   @c thread_cap or @c _lines). Tab is bound to a no-op and no completion
 *   function is installed, so readline's filename-completion fallback never
 *   triggers. Numeric keys are classified before the "_list" check, so
 *   @c thread_cap_for_list_sorting / @c thread_cap_for_list_filtering are no
 *   longer mistaken for full/compact list settings.
 * - **Completion mode:** clears @c g_rl_complete_mode for the duration of the
 *   call (and restores it on exit) so the search prompts' stash-and-exit
 *   completion behaviour is never inherited here. Without this, a Tab
 *   completion in this screen would set @c rl_done and drop the user back out
 *   of the prompt after a previous search-prompt visit.
 *
 * @param configPath  Configuration file used to persist the change.
 * @param key         Configuration key to edit.
 * @param inputErrors Token errors from the selection prompt, printed above the
 *                    edit screen header. Callers pass them for the first edit
 *                    of a selection only; defaults to none.
 * @param pos         1-based position of this setting within the selection.
 * @param total       Number of settings in the selection; navigation keys are
 *                    only active when this is greater than one.
 * @return @c EditNav::Updated if a value was accepted, including the same value
 *         as the current one (the caller removes the setting from the
 *         selection); @c EditNav::Advance if the edit was skipped with a blank
 *         Enter or the key has no matching entry (the caller also removes the
 *         setting); @c Next / @c Prev if PgDn /
 *         PgUp was pressed; @c GoTo if "g#" selected another setting;
 *         @c Abort if the user aborted with Ctrl+D or Esc.
 */
EditNav editSetting(const std::string& configPath, const std::string& key,
                    const std::vector<std::string>& inputErrors,
                    size_t pos, size_t total) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = nullptr;
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
        if (e.key == key) { entry = &e; break; }
    }
    if (!entry) return EditNav::Advance;
    reset_custom_keybindingsForSettingsEditor();

    // Numeric settings take a free-form number: no Tab completion at all.
    // Evaluated before the validValues block because some numeric keys
    // (thread_cap_for_list_sorting / thread_cap_for_list_filtering) contain
    // "_list" and would otherwise be mistaken for full/compact settings.
    const bool isNumeric =
        key == "pagination" || key == "live_filter_limit" ||
        key.find("thread_cap") != std::string::npos ||
        key.find("_lines") != std::string::npos;

    // Valid values for Tab completion. These mirror the "Valid values:"
    // strings printed by drawEdit below, so the completion list and the
    // on-screen hint can never drift apart.
    std::vector<std::string> validValues;
    if (isNumeric) {
        // numeric settings: no completion candidates
    } else if (key == "base") {
        validValues = {"charcoal", "black", "gray", "silver", "ash", "white", "steel"};
    } else if (key == "skin") {
        validValues = {"green", "cyan", "white", "purple", "amber", "rose", "gray", "black"};
    } else if (key == "theme") {
        validValues = {"original", "classic", "high_contrast", "neon", "ocean", "sunset",
                       "forest", "midnight", "mono", "retro", "crimson", "dracula",
                       "tokyo", "paper", "sakura"};
    } else if (key == "auto_update" || key == "filenames_only") {
        validValues = {"off", "on"};
    } else if (key.find("_list") != std::string::npos) {
        validValues = {"full", "compact"};
    }

    // Restores PgUp/PgDn and Esc to their default bindings on every exit path.
    //
    // Esc (`\e`) is restored here too as a defensive guarantee: the Settings
    // Editor's completion path does not hijack Esc, but if anything else ever
    // did, we must not leak that binding past this screen.
    struct NavBindGuard {
        bool on;
        ~NavBindGuard() {
            if (on) {
                rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
                rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
            }
            rl_bind_keyseq("\\e", exit_handler);
        }
    } navGuard{ total > 1 };

    // Restores the default completion entry (nullptr) on every exit path, so
    // the interactiveConfigEditor's own prompt is not affected by the value
    // completion we install per-iteration.
    struct CompletionGuard {
        ~CompletionGuard() {
            g_activeSettingValues = nullptr;
            rl_attempted_completion_function = nullptr;
            rl_completion_display_matches_hook = customListingsFunction;
        }
    } completionGuard;

    // The search prompts enable the "stash the current line, exit readline,
    // restore on the next prompt" completion flow by setting
    // RetainAndRestoreReadlineBuffer::g_rl_complete_mode to 1. That flow is
    // wrong for this screen: my_rl_complete() would set rl_done and drop the
    // user out of the value prompt after a Tab completion. Force the mode off
    // for the duration of this call, and restore the previous value on exit so
    // the caller's own flow is unaffected.
    struct CompleteModeGuard {
        int saved = RetainAndRestoreReadlineBuffer::g_rl_complete_mode;
        ~CompleteModeGuard() {
            RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved;
        }
    } completeModeGuard;
    RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;

    std::string current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : entry->defaultValue;

    std::vector<std::string> pendingErrors = inputErrors;

    auto drawEdit = [&] {
        clearScrollBuffer();
        if (!pendingErrors.empty()) {
            std::cout << "\n";
            for (const auto& err : pendingErrors)
                std::cout << err << "\n";
            pendingErrors.clear();
        }
        std::cout << "\n" << tc.str << "=== Edit Setting ===";
        if (total > 1) std::cout << "  ("<< (globalTheme == "original" ? UI::Palette::DarkCyan : tc.highlight) << pos << tc.str <<"/" << tc.warning << total << tc.str << ")";
        std::cout << "\n\n" << tc.reset;
        std::cout << tc.str << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
        std::cout << tc.str << "Current: " << tc.reset << tc.highlight << current << tc.reset << "\n";
        std::string displayComment = entry->comment;
        size_t cpos = displayComment.find("\n# ");
        if (cpos != std::string::npos) displayComment.replace(cpos, 3, "\n             ");
        std::cout << "\nDescription: " << displayComment << tc.reset << "\n\n";

        std::cout << tc.str << "Valid values: " << tc.reset;
        if (key == "base") {
            std::cout << "ash, black, charcoal, gray, silver, steel, white\n";
        } else if (key == "skin") {
            std::cout << "amber, black, cyan, gray, green, purple, rose, white\n";
        } else if (key == "theme") {
            std::cout << "classic, crimson, dracula, forest, high_contrast, midnight, mono,\n"
                        << "               neon, ocean, original, paper, retro, sakura, sunset, tokyo\n";
        } else if (key == "auto_update" || key == "filenames_only") {
            std::cout << "off, on\n";
        } else if (isNumeric) {
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

        // Navigation footer (multi-selection only); a hint is shown only
        // when that direction is available.
        if (total > 1) {
            std::cout << "\n" << tc.str;
            if (pos > 1)     std::cout << "[PgUp] Prev | ";
            if (pos < total) std::cout << "[PgDn] Next | ";
            std::cout << "[g#] ↵ GoTo" << tc.reset << "\n";
        }
    };

    while (true) {
        rl_bind_keyseq("\\e", exit_handler);
        if (total > 1) {
            rl_bind_keyseq("\\e[5~", pgup_handler);
            rl_bind_keyseq("\\e[6~", pgdn_handler);
        }

        // Enable tab completion of this setting's valid values (disabled for
        // numeric settings).
        //
        // Use the minimal, explicit bindings this screen actually needs
        // instead of RestoreReadlineBuffer(). RestoreReadlineBuffer() is the
        // main-prompt helper: it also installs a pending-text startup hook and
        // (previously) a static-captured Ctrl+L lambda, both of which are out
        // of place here. Binding Tab -> my_rl_complete and Ctrl+L ->
        // clear_screen_and_buffer directly is all that is required for value
        // completion + repaint, and it keeps this screen consistent with the
        // search prompts' completion behavior.
        //
        // For numeric settings Tab is bound to a no-op: clearing the
        // completion function alone would let readline fall back to its
        // default filename completion.
        g_activeSettingValues = validValues.empty() ? nullptr : &validValues;
        rl_attempted_completion_function =
            isNumeric ? nullptr : setting_value_completion_entry;
        rl_completion_display_matches_hook = customListingsFunction;
        rl_bind_key('\t', isNumeric ? noop_tab_handler : my_rl_complete);
        rl_bind_key('\f', clear_screen_and_buffer);

        drawEdit();
        std::cout << "\n";

        std::string prompt = std::format(
            "\001{}\002Value \001{}\002↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
            UI::Palette::idxGreen,
            tc.path,
            UI::Palette::Yellow,
            tc.path,
            tc.reset
        );

        RlLine rawInput = readlineWithRepaint(prompt, drawEdit);

        // Match search's post-readline cleanup so a stale startup hook can't
        // fire on the next prompt.
        armPendingInputRestore();
        rl_startup_hook = nullptr;

        if (rawInput) {
            const std::string_view s = rawInput.get();
            if (s == "PgUp" || s == "PgDn") {
                // pgup/pgdn_handler stash the typed text; it belongs to this
                // setting only, so drop it instead of leaking it to the next
                // screen or the main prompt.
                RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
                if (s == "PgDn" && pos < total) return EditNav::Next;
                if (s == "PgUp" && pos > 1)     return EditNav::Prev;
                continue;   // at either end: stay on this screen
            }
        }
        if (!rawInput || rawInput.get()[0] == 27) return EditNav::Abort;
        std::string newVal = trim(rawInput.get());

        // "g#" jumps to the #-th selected setting (multi-selection only).
        // Out-of-range or same-screen targets just redraw, like the paginator.
        if (total > 1 && newVal.size() >= 2 && newVal[0] == 'g' &&
            std::isdigit(static_cast<unsigned char>(newVal[1]))) {
            try {
                size_t target = std::stoul(newVal.substr(1));
                if (target >= 1 && target <= total && target != pos) {
                    g_editGotoPos = target;
                    return EditNav::GoTo;
                }
            } catch (...) {}
            continue;
        }

        // Blank Enter skips: the caller drops the setting from its loop.
        // Re-entering the current value is NOT a skip: it counts as an update
        // (validated, saved, and the setting leaves the loop like any other).
        if (newVal.empty()) return EditNav::Advance;

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
                std::cout << tc.warning << "\nNotice: " << tc.error
                          << "Setting update is memory-only (Disk write failed)."
                          << tc.reset << "\n";
            }
        };
        pressEnterToContinueWithRepaint(render);

        if (key == "filenames_only") {
            sortAfterFilenamesOnlyFlag();
        }

        // The value changed: tell the caller to drop this setting from the
        // PgUp/PgDn/g# loop.
        return EditNav::Updated;
    }
    return EditNav::Advance;   // unreachable; kept for the compiler
}
