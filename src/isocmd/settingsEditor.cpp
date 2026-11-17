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
#include <optional>
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
 * @param cache   Configuration key-value pairs.
 * @param onlyKey If non-empty, apply only the effect of this key; if empty,
 *                apply everything (startup, reset-all).
 */
void applyConfigEffects(const std::map<std::string, std::string>& cache,
                        const std::string& onlyKey = {}) {
    auto want = [&](const char* k) {
        return (onlyKey.empty() || onlyKey == k) && cache.count(k);
    };

    // Keys handled explicitly below; anything else is assumed to belong to
    // the thread/history group.
    static const std::unordered_set<std::string> handledHere = {
        "mount_list", "umount_list", "cp_mv_rm_list", "write2usb_list",
        "convert2iso_lists", "filenames_only", "skin", "theme", "base",
        "pagination", "live_filter_limit"
    };
    if (onlyKey.empty() || !handledHere.count(onlyKey))
        applyThreadCapsAndHistoryLimits(cache);

    if (want("mount_list"))        displayConfig::toggleFullListMount     = (cache.at("mount_list") == "full");
    if (want("umount_list"))       displayConfig::toggleFullListUmount    = (cache.at("umount_list") == "full");
    if (want("cp_mv_rm_list"))     displayConfig::toggleFullListCpMvRm    = (cache.at("cp_mv_rm_list") == "full");
    if (want("write2usb_list"))    displayConfig::toggleFullListWrite2usb = (cache.at("write2usb_list") == "full");
    if (want("convert2iso_lists")) displayConfig::toggleFullListConvert2iso = (cache.at("convert2iso_lists") == "full");
    if (want("filenames_only"))    displayConfig::toggleNamesOnly         = (cache.at("filenames_only") == "on");

    if (want("skin"))  { skin = cache.at("skin"); color = getskin(); }
    if (want("theme")) { globalTheme = cache.at("theme"); }

    if (want("base")) {
        UI::Palette::defaultColor = cache.at("base");
        UI::Palette::updateDefaultColors();
    }

    if (want("pagination")) {
        try { GlobalState::ITEMS_PER_PAGE = std::stoul(cache.at("pagination")); } catch (...) {}
    }
    if (want("live_filter_limit")) {
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

namespace {

using ThemeColors = decltype(resolveOptionsTheme());

// ---------------------------------------------------------------------------
// Screen painting
// ---------------------------------------------------------------------------

// Single definition of the settings screen body (header + table + actions
// line). Shared by the normal draw, the reset confirmation and the post-reset
// repaint, so a resize reconstructs the whole screen.
struct SettingsScreen {
    const std::string& configPath;
    ThemeColors tc;  // refreshed after a reset, so it lives with the screen

    static std::string currentValue(const auto& entry) {
        return ConfigCaches::g_configCache.count(entry.key)
                   ? ConfigCaches::g_configCache[entry.key]
                   : entry.defaultValue;
    }

    std::string_view rowColor(int index) const {
        if (index == 1) return UI::Palette::BoldReset;
        if (index == 2) return color;
        return tc.data;
    }

    void paintRow(int index, const auto& entry) const {
        if (!entry.section.empty()) {
            std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
        }
        std::string val = currentValue(entry);
        std::cout << tc.warning << std::right << std::setw(2) << index << ". " << tc.reset
                  << tc.label << std::left << std::setw(32) << entry.key << tc.reset
                  << "= " << tc.reset
                  << rowColor(index) << val << tc.reset << "\n";
    }

    void paintBody() const {
        std::cout << "\n" << tc.accent << "=== Settings Editor ===\n\n" << tc.reset;
        std::cout << tc.warning << "Config File: " << tc.reset << configPath << "\n";

        int index = 1;
        for (const auto& entry : CONFIG_ORDERED_DEFAULTS) {
            paintRow(index++, entry);
        }
        std::cout << "\n" << tc.accent << "Actions: " << tc.warning << "1-" << (index - 1)
                  << tc.reset << " ↵ Edit | " << tc.warning << "r" << tc.reset << " Reset\n";
    }

    void draw() const {
        SyncOutput sync;
        clearScrollBuffer();
        paintBody();
    }
};

std::string buildMainPrompt(const ThemeColors& tc) {
    return std::format(
        "\n{}IDX\001{}\002 ↵ for {}editor\001{}\002, ? for help: \001{}\002",
        UI::Palette::RL_idxGreen,
        tc.path,
        UI::Palette::RL_Yellow,
        tc.path,
        tc.reset
    );
}

// ---------------------------------------------------------------------------
// Reset flow
// ---------------------------------------------------------------------------

void paintResetStatus(const SettingsScreen& screen, bool saved) {
    const auto& tc = screen.tc;
    SyncOutput sync;
    clearScrollBuffer();
    screen.paintBody();
    std::cout << color << "\nReset all settings to default? (y/n): \n" << UI::Palette::BoldReset;

    std::cout << "\n" << tc.label << "[+] Defaults applied\n" << tc.reset;
    if (saved) {
        std::cout << tc.highlight << "[✔] Saved to disk\n" << tc.reset;
    } else {
        std::cout << tc.error << "[✗] Save failed\n" << tc.reset;
    }
}

void applyDefaultsAndReport(SettingsScreen& screen) {
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
        ConfigCaches::g_configCache[e.key] = e.defaultValue;
    }
    applyConfigEffects(ConfigCaches::g_configCache);
    screen.tc = resolveOptionsTheme();

    bool saved = false;
    {
        SyncOutput sync;
        saved = flushCache(screen.configPath);
    }

    auto render = [&] { paintResetStatus(screen, saved); };
    pressEnterToContinueWithRepaint(render);
}

bool isConfirmed(const std::string& answer) {
    return answer == "y" || answer == "Y";
}

void handleReset(SettingsScreen& screen) {
    reset_custom_keybindingsForSettingsEditor();
    std::string confirmPrompt = std::format(
        "\n\001{}\002Reset all settings to default? (y/n): \001{}\002",
        color,
        UI::Palette::BoldReset
    );

    auto drawSettings = [&] { screen.draw(); };

    // Wipe the IDX prompt line now so the screen looks the same before and
    // after a resize: settings body, then the y/n prompt directly beneath.
    drawSettings();

    armPendingInputRestore();
    RlLine confirmInput = readlineWithRepaint(confirmPrompt, drawSettings);
    rl_startup_hook = nullptr;

    if (!confirmInput) return;
    if (!isConfirmed(trim(confirmInput.get()))) return;
    applyDefaultsAndReport(screen);
}

// ---------------------------------------------------------------------------
// Selection / multi-edit flow
// ---------------------------------------------------------------------------

void reportNoValidInput(const ThemeColors& tc) {
    auto render = [&] {
        SyncOutput sync;
        clearScrollBuffer();
        std::cout << "\n" << tc.error << "No valid input provided"
                  << tc.reset << "\n";
    };
    pressEnterToContinueWithRepaint(render);
}

// Skipped and updated settings both leave the PgUp/PgDn/g# loop. The next
// setting slides into index i, so don't increment; past the end, wrap to the
// first pending setting. Returns true once nothing is left to edit.
bool removeHandled(std::vector<int>& ordered, size_t& i) {
    ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(i));
    if (ordered.empty()) return true;
    if (i >= ordered.size()) i = 0;
    return false;
}

// Applies one navigation result to the selection. Returns true when the loop
// should end.
bool advanceSelection(EditNav r, std::vector<int>& ordered, size_t& i) {
    const size_t n = ordered.size();
    switch (r) {
        case EditNav::Advance:   // skipped: dropped just like an update
        case EditNav::Updated:
            return removeHandled(ordered, i);
        case EditNav::Next:
            if (i + 1 < n) ++i;
            return false;
        case EditNav::Prev:
            if (i > 0) --i;
            return false;
        case EditNav::GoTo:
            if (g_editGotoPos >= 1 && g_editGotoPos <= n) i = g_editGotoPos - 1;
            return false;
        case EditNav::Abort:
            return true;
    }
    return false;
}

void runMultiEdit(const std::string& configPath,
                  std::vector<int> ordered,
                  const std::vector<std::string>& inputErrors) {
    const std::vector<std::string> noErrors;
    size_t i = 0;
    bool showErrors = true;          // token errors only on the first screen
    bool done = false;
    while (!done) {
        // Recomputed every iteration: handled settings are erased from
        // `ordered`, so the total shown in "(n/total)" and used for
        // PgUp/PgDn/g# shrinks accordingly.
        const size_t n = ordered.size();
        EditNav r = editSetting(configPath,
                                CONFIG_ORDERED_DEFAULTS[ordered[i] - 1].key,
                                showErrors ? inputErrors : noErrors,
                                i + 1, n);
        showErrors = false;
        done = advanceSelection(r, ordered, i);
    }
}

void handleSelection(const SettingsScreen& screen, const std::string& input) {
    std::vector<std::string> slots(CONFIG_ORDERED_DEFAULTS.size());
    std::unordered_set<int> choices;

    tokenizeInput(input, slots, choices);

    // Snapshot of the token errors so they can be handed to editSetting
    // and survive repaint callbacks.
    const std::vector<std::string> inputErrors(
        verboseSets.uniqueErrorTokenMessages.begin(),
        verboseSets.uniqueErrorTokenMessages.end());

    // Only errors, nothing valid to edit: report and return to the main prompt.
    if (choices.empty()) {
        if (!inputErrors.empty()) reportNoValidInput(screen.tc);
        return;
    }

    // Valid choices exist: errors (if any) are shown above the first
    // edit screen only.
    std::vector<int> ordered(choices.begin(), choices.end());
    std::sort(ordered.begin(), ordered.end());
    runMultiEdit(screen.configPath, std::move(ordered), inputErrors);
}

// ---------------------------------------------------------------------------
// Main-prompt dispatch
// ---------------------------------------------------------------------------

void handleMainInput(SettingsScreen& screen, const std::string& input) {
    if (input == "?") {
        helpSettingsEditor();
        return;
    }
    if (input == "r" || input == "R") {
        handleReset(screen);
        return;
    }
    handleSelection(screen, input);
}

}  // namespace

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
 * - **Resize handling:** every screen shares @c SettingsScreen::paintBody, so
 *   a resize repaints the full settings view rather than just the last
 *   message. @c SettingsScreen::draw is the repaint callback for both the
 *   main prompt and the reset confirmation, and @c paintResetStatus serves
 *   the post-reset status screen while @c reportNoValidInput serves the
 *   "No valid input provided" screen. Once a reset is requested, the screen
 *   is repainted without the main prompt line, and neither that line nor the
 *   confirmation line is re-echoed afterwards.
 * - **Clear handling:** <kbd>Ctrl+L</kbd> is bound only on the per-setting
 *   value prompt in @c editSetting, where it dismisses any completion list and
 *   repaints the edit screen via @c readlineWithRepaint. The binding is
 *   released when @c editSetting returns, so it is not active on the main
 *   settings prompt or the reset confirmation.
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
        SettingsScreen screen{configPath, resolveOptionsTheme()};

        auto drawSettings = [&] { screen.draw(); };
        drawSettings();

        setup_custom_keybindingsForSettingsEditor();

        const std::string prompt = buildMainPrompt(screen.tc);
        armPendingInputRestore();
        RlLine rawInput = readlineWithRepaint(prompt, drawSettings);
        rl_startup_hook = nullptr;

        if (!rawInput) break;

        const std::string input = trim(rawInput.get());
        if (input == "\x1b") break;

        handleMainInput(screen, input);
    }
}

/**
 * @brief Finds and returns the configuration entry metadata matching the specified key.
 *
 * Iterates through the global ordered configuration defaults list to find a
 * matching configuration key.
 *
 * @param key The configuration key string to search for (e.g., "theme", "base").
 * @return const ConfigEntry* Pointer to the matching configuration entry, or @c nullptr if not found.
 */
const ConfigEntry* findConfigEntry(const std::string& key) {
    for (const auto& e : CONFIG_ORDERED_DEFAULTS) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

/**
 * @brief RAII helper to ensure safe restoration of Readline navigation and escape key bindings.
 *
 * Restores PgUp, PgDn, and Esc (`\e`) keybindings back to their default system handlers
 * upon exiting any scope where custom settings editor bindings were active, preventing
 * keybinding leaks into other prompts.
 */
struct NavBindGuard {
    bool on;
    ~NavBindGuard() {
        if (on) {
            rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
            rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
        }
        rl_bind_keyseq("\\e", exit_handler);
    }
};

/**
 * @brief RAII helper to clean up tab completion hooks and active settings on all exit paths.
 *
 * Resets global pointers like @c g_activeSettingValues, uninstalls custom completion functions,
 * restores standard listing hooks, and binds Tab and Ctrl+L back to no-op handlers so the value
 * prompt's bindings (filename completion fallback, @c clear_list_and_screen) cannot leak outside
 * the settings editor.
 */
struct CompletionGuard {
    ~CompletionGuard() {
        g_activeSettingValues = nullptr;
        rl_attempted_completion_function = nullptr;
        rl_completion_display_matches_hook = customListingsFunction;
        rl_bind_key('\t', noop_tab_handler);
        rl_bind_key('\f', prevent_readline_keybindings);
    }
};

/**
 * @brief RAII helper to toggle global completion mode for the duration of the edit call.
 *
 * Temporarily clears @c RetainAndRestoreReadlineBuffer::g_rl_complete_mode to ensure search-prompt
 * completion stash-and-exit semantics are never inherited by the settings value prompt. Restores
 * the original state automatically upon destruction.
 */
struct CompleteModeGuard {
    int saved = RetainAndRestoreReadlineBuffer::g_rl_complete_mode;
    ~CompleteModeGuard() {
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved;
    }
};

/**
 * @brief Renders the interactive configuration setting edit screen layout.
 *
 * Clears the scroll buffer, prints any pending token errors from selection validation above the header,
 * and formats the current setting key, active value, description comments, valid values or numeric
 * min-max ranges, and multi-selection navigation footers. Designed to be reusable across redraws,
 * invalid input warnings, and terminal resize events.
 *
 * @tparam ThemeT Type representing the active UI theme color configuration.
 * @param key The configuration key being edited.
 * @param current The current string value of the configuration option.
 * @param tc Theme color reference used for rendering headings, highlights, and borders.
 * @param entry Pointer to the metadata @c ConfigEntry containing comments, defaults, and ranges.
 * @param validValues List of valid allowed strings for enumerated settings.
 * @param isNumeric Flag indicating whether the setting accepts free-form numeric values.
 * @param pos 1-based index of this setting within an active multi-setting selection batch.
 * @param total Total number of settings in the current selection batch.
 * @param pendingErrors Reference to token/input error messages to display; cleared after printing.
 */
template <typename ThemeT>
void drawEditScreen(const std::string& key, const std::string& current,
                    const ThemeT& tc, const ConfigEntry* entry,
                    const std::vector<std::string>& validValues,
                    bool isNumeric, size_t pos, size_t total,
                    std::vector<std::string>& pendingErrors)
{
    clearScrollBuffer();
    if (!pendingErrors.empty()) {
        std::cout << "\n";
        for (const auto& err : pendingErrors)
            std::cout << err << "\n";
        pendingErrors.clear();
    }

    std::string_view currentColor = (key == "base") ? tc.reset
                                  : (key == "skin") ? color
                                  : tc.highlight;

    std::cout << "\n" << tc.str << "=== Edit Setting ===";
    if (total > 1) {
        std::cout << "  (" << (globalTheme == "original" ? UI::Palette::DarkCyan : tc.highlight)
                  << pos << tc.str << "/" << tc.warning << total << tc.str << ")";
    }
    std::cout << "\n\n" << tc.reset;
    std::cout << tc.str << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
    std::cout << tc.str << "Current: " << tc.reset << currentColor << current << tc.reset << "\n";

    std::string displayComment = entry->comment;
    size_t cpos = displayComment.find("\n# ");
    if (cpos != std::string::npos) displayComment.replace(cpos, 3, "\n             ");
    std::cout << "\nDescription: " << displayComment << tc.reset << "\n\n";

    std::cout << tc.str << "Valid values: " << tc.reset;
    if (!validValues.empty()) {
        const size_t wrapAfter = (key == "theme") ? 7 : validValues.size();
        for (size_t k = 0; k < validValues.size(); ++k) {
            if (k == wrapAfter)  std::cout << "\n               ";
            else if (k > 0)      std::cout << ", ";
            std::cout << validValues[k];
            if (k + 1 == wrapAfter && k + 1 < validValues.size()) std::cout << ",";
        }
        std::cout << "\n";
    } else if (isNumeric) {
        std::cout << "numeric value (min - max: " << entry->minValue
                  << " - " << entry->maxValue << ")\n";
    } else {
        std::cout << "Refer to description above\n";
    }

    if (total > 1) {
        std::cout << "\n" << tc.str;
        if (pos > 1)     std::cout << "[PgUp] Prev | ";
        if (pos < total) std::cout << "[PgDn] Next | ";
        std::cout << "[g#] ↵ GoTo" << tc.reset << "\n";
    }
}

/**
 * @brief Evaluates multi-selection navigation input tokens such as Page Up, Page Down, or GoTo commands.
 *
 * Parses readline inputs to check if the user triggered batch navigation. Clears pending text buffers
 * when page jumps occur and validates target bounds for explicit numeric `g#` jumps.
 *
 * @spec
 * - **PgUp / PgDn:** Evaluated when @p total > 1, returning @c EditNav::Next or @c EditNav::Prev.
 * - **g# syntax:** Jumps to the specified #-th configuration setting index in the batch.
 *
 * @param s String view of the trimmed user input token.
 * @param pos Current 1-based setting index in the selection.
 * @param total Total number of settings in the selection batch.
 * @param newVal Reference to the working input value string.
 * @return @c std::optional<EditNav> Contains the resulting navigation direction if matched, or @c std::nullopt to stay on screen.
 */
std::optional<EditNav> handleNavigationInput(const std::string_view& s, size_t pos, size_t total, const std::string& newVal) {
    if (s == "PgUp" || s == "PgDn") {
        RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
        if (s == "PgDn" && pos < total) return EditNav::Next;
        if (s == "PgUp" && pos > 1)     return EditNav::Prev;
        return std::nullopt; // stay on screen
    }

    if (total > 1 && newVal.size() >= 2 && newVal[0] == 'g' &&
        std::isdigit(static_cast<unsigned char>(newVal[1]))) {
        try {
            size_t target = std::stoul(newVal.substr(1));
            if (target >= 1 && target <= total && target != pos) {
                g_editGotoPos = target;
                return EditNav::GoTo;
            }
        } catch (...) {}
        return std::nullopt; // invalid target, stay on screen
    }
    return std::nullopt;
}

/**
 * @brief Commits validation, cache updates, runtime effect application, and disk persistence for an accepted value.
 *
 * Stores the validated new configuration value into @c g_configCache, triggers immediate runtime application
 * via @c applyConfigEffects, attempts persistence via @c flushCache, then refreshes @p tc via
 * @c resolveOptionsTheme so subsequent redraws (through @p drawEditCallback) reflect any palette
 * changes caused by the edit (e.g. @c theme, @c skin, @c base). Notifies the user of success or
 * memory-only fallback status. Also triggers specialized flag sorting logic when editing @c filenames_only.
 *
 * @tparam ThemeT Type representing the active UI theme color configuration.
 * @param configPath Configuration file path used to persist changes.
 * @param key Configuration setting key being updated.
 * @param newVal The validated new string value to store.
 * @param tc In/out theme color configuration. Reassigned from @c resolveOptionsTheme after
 *           @c applyConfigEffects runs, so callers' repaint lambdas that capture @p tc by
 *           reference pick up the new palette on the first redraw (not the second).
 * @param current Reference to the current value variable, updated upon success.
 * @param drawEditCallback Lambda callback used to redraw the edit screen during confirmation prompts.
 * @return @c EditNav::Updated indicating successful update and completion for this setting item.
 */
template <typename ThemeT>
EditNav saveAndApplyConfigValue(const std::string& configPath, const std::string& key,
                                const std::string& newVal, ThemeT& tc,   // <-- by reference
                                std::string& current, const std::function<void()>& drawEditCallback)
{
    ConfigCaches::g_configCache[key] = newVal;
    applyConfigEffects(ConfigCaches::g_configCache, key);

    current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : "";
    bool saved = flushCache(configPath);

    tc = resolveOptionsTheme();   // <-- reassign the shared tc

    auto render = [&] {
        SyncOutput sync;
        drawEditCallback();
        if (saved) {
            std::cout << std::format("\n{}✓ Updated and saved to: {}{}\n",
                                     tc.path, tc.reset, configPath);
        } else {
            std::cout << tc.warning << "\nWarning: " << tc.error
                      << "Setting update is memory-only (Disk write failed)."
                      << tc.reset << "\n";
        }
    };
    pressEnterToContinueWithRepaint(render);

    if (key == "filenames_only") {
        sortAfterFilenamesOnlyFlag();
    }

    return EditNav::Updated;
}

/**
 * @brief Interactively edits one configuration setting with full tab completion, validation, and multi-selection navigation support.
 *
 * Displays the current value, description, and valid-value guidance. Invalid input returns to the prompt;
 * a blank input skips the edit (dropping the setting from the selection loop). Re-entering the current value
 * counts as a valid update. Manages readline keybindings, completion hooks, buffer guards, validation checks,
 * runtime application, persistence flushing, and batch navigation (`PgUp`, `PgDn`, `g#`).
 *
 * @param configPath Configuration file path used to persist the change.
 * @param key Configuration key to edit.
 * @param inputErrors Token errors from selection prompts, printed above the edit screen header for the first edit.
 * @param pos 1-based position of this setting within the current selection batch.
 * @param total Number of settings in the selection batch; navigation keys are only active when greater than one.
 * @return @c EditNav::Updated if a value was accepted; @c EditNav::Advance if skipped with a blank Enter or unmatched key;
 *         @c EditNav::Next / @c Prev / @c GoTo for multi-setting navigation flows; or @c EditNav::Abort if aborted via Esc/Ctrl+D.
 */
EditNav editSetting(const std::string& configPath, const std::string& key,
                    const std::vector<std::string>& inputErrors,
                    size_t pos, size_t total) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = findConfigEntry(key);
    if (!entry) return EditNav::Advance;

    reset_custom_keybindingsForSettingsEditor();

    const bool isNumeric = entry->numeric;
    std::vector<std::string> validValues;
    if (entry->values) validValues = *entry->values;

    NavBindGuard navGuard{ total > 1 };
    CompletionGuard completionGuard;
    CompleteModeGuard completeModeGuard;
    RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;

    std::string current = ConfigCaches::g_configCache.count(key) ? ConfigCaches::g_configCache[key] : entry->defaultValue;
    std::vector<std::string> pendingErrors = inputErrors;

    auto drawEdit = [&] {
        drawEditScreen(key, current, tc, entry, validValues, isNumeric, pos, total, pendingErrors);
    };

    // Resize / Ctrl+L repaint. The watcher's contract is that this callback
    // ends one line short of the prompt's input line. The completion listing
    // saves/restores an absolute cursor position (ESC[s / ESC[u), so it must
    // run with the cursor ON the input line, exactly as in the normal path:
    // step down onto it, repaint the listing, then step back up so the
    // watcher's own '\n' lands correctly.
    auto repaintEditOnResize = [&] {
        drawEdit();
        std::cout << "\n";
        repaintCompletionListing();
        std::cout << "\033[A";
    };

    while (true) {
        // Drop any completion snapshot / page state left over from a previous
        // prompt (skipped setting, invalid value, PgUp/PgDn/g#), so a resize
        // on this screen can never repaint another screen's list.
        resetReadlinePagination();

        rl_bind_keyseq("\\e", exit_handler);
        if (total > 1) {
            rl_bind_keyseq("\\e[5~", pgup_handler);
            rl_bind_keyseq("\\e[6~", pgdn_handler);
        }

        g_activeSettingValues = validValues.empty() ? nullptr : &validValues;
        rl_attempted_completion_function = isNumeric ? nullptr : setting_value_completion_entry;
        rl_completion_display_matches_hook = customListingsFunction;
        rl_bind_key('\t', isNumeric ? noop_tab_handler : my_rl_complete);
        // Ctrl+L dismisses a visible completion list (drops the snapshot
        // before the repaint callback runs) and repaints the screen.
        rl_bind_key('\f', clear_list_and_screen);

        drawEdit();
        std::cout << "\n";

        std::string prompt = std::format(
            "\001{}\002Value \001{}\002↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
            UI::Palette::idxGreen, tc.path, UI::Palette::Yellow, tc.path, tc.reset
        );

        RlLine rawInput = readlineWithRepaint(prompt, repaintEditOnResize);

        armPendingInputRestore();
        rl_startup_hook = nullptr;

        if (!rawInput || rawInput.get()[0] == 27) return EditNav::Abort;

        const std::string newVal = trim(rawInput.get());

        auto navAction = handleNavigationInput(rawInput.get(), pos, total, newVal);
        if (navAction.has_value()) {
            if (*navAction == EditNav::Next || *navAction == EditNav::Prev || *navAction == EditNav::GoTo) {
                return *navAction;
            }
            continue;
        }

        // A g#-shaped token that wasn't a valid target: stay in the loop silently.
        // Do not fall through to value validation.
        if (total > 1 && newVal.size() >= 2 && newVal[0] == 'g' &&
            std::isdigit(static_cast<unsigned char>(newVal[1]))) {
            continue;
        }

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

        return saveAndApplyConfigValue(configPath, key, newVal, tc, current, drawEdit);
    }
    return EditNav::Advance;
}
