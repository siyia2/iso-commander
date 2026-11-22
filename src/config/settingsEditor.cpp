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
#include "isocmd/config/settings.h"
#include "isocmd/core/state.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/input/readline.h"
#include "isocmd/input/tokenize.h"
#include "isocmd/ui/display.h"
#include "isocmd/ui/pausePrompts.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/ui/themes.h"
#include "isocmd/ui/verbose.h"
#include "isocmd/util/sort.h"

// ============================================================================
// Internal Editor Subsystems
// ============================================================================

namespace {

    /**
     * @brief Batch edit navigation parsing and queue index progression.
     */
    namespace nav {

        /// 1-based target index parsed from a `g<number>` jump command.
        size_t gotoPos = 0;

        /**
         * @brief Translates special navigation keys or jump commands into an `EditNav` action.
         * @param s Raw Readline input string or key token (`"PgUp"`, `"PgDn"`, etc.).
         * @param pos Current 1-based position within the batch queue.
         * @param total Total number of items in the batch queue.
         * @param newVal Trimmed input text used to detect `g<number>` jump syntax.
         * @return Navigation action if matched; `std::nullopt` for standard value/command input.
         */
        std::optional<EditNav> handleInput(const std::string_view& s, size_t pos, size_t total,
                                           const std::string& newVal) {
            if (s == "PgUp" || s == "PgDn") {
                RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();

                if (s == "PgDn") {
                    return (pos < total) ? EditNav::Next : EditNav::Handled;
                }
                return (pos > 1) ? EditNav::Prev : EditNav::Handled;
            }

            if (newVal.size() >= 2 && newVal[0] == 'g' &&
                std::isdigit(static_cast<unsigned char>(newVal[1]))) {
                RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
                try {
                    const size_t target = std::stoul(newVal.substr(1));
                    if (target >= 1 && target <= total && target != pos) {
                        gotoPos = target;
                        return EditNav::GoTo;
                    }
                } catch (...) {
                }
                return EditNav::Handled;
            }

            return std::nullopt;
        }

        /**
         * @brief Removes the item at @p i from @p ordered and wraps the index if needed.
         * @param ordered Remaining 1-based setting indices in the batch queue.
         * @param i In/out index into @p ordered.
         * @return `true` if no items remain in @p ordered; `false` otherwise.
         */
        bool removeHandled(std::vector<int>& ordered, size_t& i) {
            ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(i));
            if (ordered.empty()) {
                return true;
            }
            if (i >= ordered.size()) {
                i = 0;
            }
            return false;
        }

        /**
         * @brief Updates batch queue state according to the navigation result from `editSetting`.
         * @param r Navigation result returned by the setting editor.
         * @param ordered Mutable list of remaining 1-based setting indices.
         * @param i In/out current index within @p ordered.
         * @return `true` when the batch edit loop should exit; `false` to continue.
         */
        bool advance(EditNav r, std::vector<int>& ordered, size_t& i) {
            const size_t n = ordered.size();
            switch (r) {
                case EditNav::Advance:
                case EditNav::Updated:
                    return removeHandled(ordered, i);
                case EditNav::Next:
                    if (i + 1 < n) {
                        ++i;
                    }
                    return false;
                case EditNav::Prev:
                    if (i > 0) {
                        --i;
                    }
                    return false;
                case EditNav::GoTo:
                    if (gotoPos >= 1 && gotoPos <= n) {
                        i = gotoPos - 1;
                    }
                    return false;
                case EditNav::Abort:
                    return true;
                case EditNav::Handled:
                    break;
            }
            return false;
        }

    } // namespace nav

    /**
     * @brief GNU Readline configuration, resize-aware prompting, and RAII state guards.
     */
    namespace term {

        /**
         * @brief Readline key handler that performs no action and returns success.
         * @return Always `0`.
         */
        int noopTabHandler(int, int) { return 0; }

        /**
         * @brief Reads a line while registering a repaint callback for terminal resize events.
         * @param prompt Formatted Readline prompt string.
         * @param repaint Callback invoked on resize or editor redraw requests.
         * @return Readline line wrapper containing the entered line or null on EOF/interrupt.
         */
        RlLine readlineWithRepaint(const std::string& prompt,
                                   const std::function<void()>& repaint) {
            SettingsEditorRepaint::set(repaint);
            RlLine line = readlineWithResize(prompt, repaint);
            SettingsEditorRepaint::set(nullptr);
            return line;
        }

        /**
         * @brief Configures completion, tab/form-feed keys, and batch navigation bindings.
         * @param total Number of items in the current edit batch (`> 1` enables PageUp/PageDown).
         * @param isNumeric `true` if the setting expects a numeric value rather than enumerated choices.
         * @param validValues Allowed discrete values for completion when non-empty.
         */
        void setupSettingReadline(size_t total, bool isNumeric,
                                  std::vector<std::string>& validValues) {
            reset_custom_keybindingsForSettingsEditor();

            g_activeSettingValues = validValues.empty() ? nullptr : &validValues;
            rl_attempted_completion_function = isNumeric ? nullptr : setting_value_completion_entry;
            rl_completion_display_matches_hook = customListingsFunction;
            rl_bind_key('\t', isNumeric ? noopTabHandler : my_rl_complete);
            rl_bind_key('\f', clear_list_and_screen);

            rl_bind_keyseq("\\e", exit_handler);
            if (total > 1) {
                rl_bind_keyseq("\\e[5~", pgup_handler);
                rl_bind_keyseq("\\e[6~", pgdn_handler);
            }
        }

        /**
         * @brief RAII guard that restores history navigation and escape bindings on scope exit.
         */
        struct NavBindGuard {
            bool on; ///< Whether PageUp/PageDown were rebound for batch navigation.

            ~NavBindGuard() {
                if (on) {
                    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
                    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
                }
                rl_bind_keyseq("\\e", exit_handler);
            }
        };

        /**
         * @brief RAII guard that clears active completion state and resets key bindings on exit.
         */
        struct CompletionGuard {
            ~CompletionGuard() {
                g_activeSettingValues = nullptr;
                rl_attempted_completion_function = nullptr;
                rl_completion_display_matches_hook = customListingsFunction;
                rl_bind_key('\t', noopTabHandler);
                rl_bind_key('\f', prevent_readline_keybindings);
            }
        };

        /**
         * @brief RAII guard that preserves and restores Readline completion mode flags.
         */
        struct CompleteModeGuard {
            int saved = RetainAndRestoreReadlineBuffer::g_rl_complete_mode;

            ~CompleteModeGuard() {
                RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved;
            }
        };

    } // namespace term

    /**
     * @brief Terminal rendering routines for the settings list, edit view, prompts, and status messages.
     */
    namespace view {

        using ThemeColors = decltype(resolveOptionsTheme());

        /**
         * @brief Renders the main settings overview screen.
         */
        struct SettingsScreen {
            const std::string& configPath; ///< Path to the active configuration file.
            ThemeColors tc;                ///< Resolved theme color palette.

            /**
             * @brief Returns the active value for a configuration entry from cache or default.
             * @param entry Configuration metadata entry.
             * @return Current value string.
             */
            static std::string currentValue(const auto& entry) {
                return ConfigCaches::g_configCache.count(entry.key)
                           ? ConfigCaches::g_configCache[entry.key]
                           : entry.defaultValue;
            }

            /**
             * @brief Selects the ANSI color sequence for a 1-based row index.
             * @param index 1-based row number in the settings table.
             * @return Color escape sequence string view.
             */
            std::string_view rowColor(int index) const {
                if (index == 1) return UI::Palette::BoldReset;
                if (index == 2) return color;
                return tc.data;
            }

            /**
             * @brief Prints a single numbered setting row and optional section header.
             * @param index 1-based row number.
             * @param entry Configuration metadata entry.
             */
            void paintRow(int index, const auto& entry) const {
                if (!entry.section.empty()) {
                    std::cout << tc.accent << "\n--- " << entry.section << " ---\n" << tc.reset;
                }
                const std::string val = currentValue(entry);
                std::cout << tc.warning << std::right << std::setw(2) << index << ". " << tc.reset
                          << tc.label << std::left << std::setw(32) << entry.key << tc.reset
                          << "= " << tc.reset
                          << rowColor(index) << val << tc.reset << "\n";
            }

            /**
             * @brief Prints the header, config path, all setting rows, and action hints.
             */
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

            /**
             * @brief Synchronously clears the scroll buffer and draws the full settings screen.
             */
            void draw() const {
                SyncOutput sync;
                clearScrollBuffer();
                paintBody();
            }
        };

        /**
         * @brief Builds the main editor selection prompt with Readline non-printing markers.
         * @param tc Active theme color palette.
         * @return Formatted prompt string.
         */
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

        /**
         * @brief Builds the value-entry prompt for editing an individual setting.
         * @tparam ThemeT Resolved theme color struct type.
         * @param tc Active theme color palette.
         * @return Formatted value prompt string.
         */
        template <typename ThemeT>
        std::string buildValuePrompt(const ThemeT& tc) {
            return std::format(
                "\001{}\002Value \001{}\002↵ | ↵ \001{}\002Skip\001{}\002: \001{}\002",
                UI::Palette::idxGreen, tc.path, UI::Palette::Yellow, tc.path, tc.reset
            );
        }

        /**
         * @brief Draws the settings screen along with reset confirmation and persistence status.
         * @param screen Settings screen state and theme.
         * @param saved `true` if flushing default values to disk succeeded.
         */
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

        /**
         * @brief Displays a "no valid input" message and waits for Enter with resize repainting.
         * @param tc Active theme color palette.
         */
        void reportNoValidInput(const ThemeColors& tc) {
            auto render = [&] {
                SyncOutput sync;
                clearScrollBuffer();
                std::cout << "\n" << tc.error << "No valid input provided"
                          << tc.reset << "\n";
            };
            pressEnterToContinueWithRepaint(render);
        }

        /**
         * @brief Prints and clears queued token/input error messages above the editor view.
         * @param pendingErrors Mutable queue of error strings to flush.
         */
        void flushPendingErrors(std::vector<std::string>& pendingErrors) {
            if (pendingErrors.empty()) {
                return;
            }

            std::cout << "\n";
            for (const auto& err : pendingErrors) {
                std::cout << err << "\n";
            }
            pendingErrors.clear();
        }

        /**
         * @brief Aligns multi-line comment continuation lines with the `Description: ` label.
         * @param comment Raw setting description comment.
         * @return Indented description string suitable for terminal display.
         */
        std::string formatDescriptionComment(const std::string& comment) {
            std::string displayComment = comment;
            const size_t cpos = displayComment.find("\n# ");
            if (cpos != std::string::npos) {
                displayComment.replace(cpos, 3, "\n             ");
            }
            return displayComment;
        }

        /**
         * @brief Prints valid values, numeric range bounds, or description fallback for a setting.
         * @tparam ThemeT Resolved theme color struct type.
         * @param key Configuration setting key.
         * @param tc Active theme color palette.
         * @param entry Metadata entry for numeric min/max bounds.
         * @param validValues Explicit allowed values when enumerated.
         * @param isNumeric `true` if the setting accepts a numeric value.
         */
        template <typename ThemeT>
        void drawValidValuesSection(const std::string& key, const ThemeT& tc,
                                    const ConfigEntry* entry,
                                    const std::vector<std::string>& validValues,
                                    bool isNumeric) {
            std::cout << tc.str << "Valid values: " << tc.reset;
            if (!validValues.empty()) {
                const size_t wrapAfter = (key == "theme") ? 7 : validValues.size();
                for (size_t k = 0; k < validValues.size(); ++k) {
                    if (k == wrapAfter) {
                        std::cout << "\n               ";
                    } else if (k > 0) {
                        std::cout << ", ";
                    }
                    std::cout << validValues[k];
                    if (k + 1 == wrapAfter && k + 1 < validValues.size()) {
                        std::cout << ",";
                    }
                }
                std::cout << "\n";
                return;
            }

            if (isNumeric) {
                std::cout << "numeric value (min - max: " << entry->minValue
                          << " - " << entry->maxValue << ")\n";
                return;
            }

            std::cout << "Refer to description above\n";
        }

        /**
         * @brief Prints batch navigation controls when editing multiple settings.
         * @tparam ThemeT Resolved theme color struct type.
         * @param tc Active theme color palette.
         * @param pos Current 1-based position in the batch.
         * @param total Total items in the batch.
         */
        template <typename ThemeT>
        void drawBatchNavigationFooter(const ThemeT& tc, size_t pos, size_t total) {
            if (total <= 1) {
                return;
            }

            std::cout << "\n" << tc.str;
            if (pos > 1) {
                std::cout << "[PgUp] Prev | ";
            }
            if (pos < total) {
                std::cout << "[PgDn] Next | ";
            }
            std::cout << "[g#] ↵ GoTo" << tc.reset << "\n";
        }

        /**
         * @brief Renders the single-setting edit screen including metadata, errors, and footer.
         * @tparam ThemeT Resolved theme color struct type.
         * @param key Configuration setting key.
         * @param current Current setting value string.
         * @param tc Active theme color palette.
         * @param entry Metadata entry for the setting.
         * @param validValues Enumerated valid values if applicable.
         * @param isNumeric `true` if the setting is numeric.
         * @param pos 1-based index within the active batch.
         * @param total Total settings in the active batch.
         * @param pendingErrors Error messages to display before clearing.
         */
        template <typename ThemeT>
        void drawEditScreen(const std::string& key, const std::string& current,
                            const ThemeT& tc, const ConfigEntry* entry,
                            const std::vector<std::string>& validValues,
                            bool isNumeric, size_t pos, size_t total,
                            std::vector<std::string>& pendingErrors) {
            clearScrollBuffer();
            flushPendingErrors(pendingErrors);

            const auto currentColor = (key == "base") ? tc.reset
                                    : (key == "skin") ? color
                                    : tc.highlight;

            std::cout << "\n" << tc.str << "=== Edit Setting ===";
            if (total > 1) {
                const auto batchBadgeColor = (globalTheme == "original") ? UI::Palette::DarkCyan : tc.highlight;
                std::cout << "  (" << batchBadgeColor
                          << pos << tc.str << "/" << tc.warning << total << tc.str << ")";
            }
            std::cout << "\n\n" << tc.reset;

            std::cout << tc.str << "Setting: " << tc.reset << tc.warning << key << tc.reset << "\n";
            std::cout << tc.str << "Current: " << tc.reset << currentColor << current << tc.reset << "\n";
            std::cout << "\nDescription: " << formatDescriptionComment(entry->comment) << tc.reset << "\n\n";

            drawValidValuesSection(key, tc, entry, validValues, isNumeric);
            drawBatchNavigationFooter(tc, pos, total);
        }

    } // namespace view

    /**
     * @brief High-level editor workflows for resetting defaults, batch editing, and input dispatch.
     */
    namespace flow {

        /**
         * @brief Checks whether a confirmation response represents an affirmative answer.
         * @param answer Trimmed confirmation input.
         * @return `true` for `"y"` or `"Y"`; `false` otherwise.
         */
        bool isConfirmed(const std::string& answer) {
            return answer == "y" || answer == "Y";
        }

        /**
         * @brief Restores all settings to defaults, reapplies effects, flushes disk, and reports status.
         * @param screen Settings screen state updated with refreshed theme colors.
         */
        void applyDefaultsAndReport(view::SettingsScreen& screen) {
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

            auto render = [&] { view::paintResetStatus(screen, saved); };
            pressEnterToContinueWithRepaint(render);
        }

        /**
         * @brief Prompts to reset all settings to default values and executes reset if confirmed.
         * @param screen Active settings screen state.
         */
        void handleReset(view::SettingsScreen& screen) {
            reset_custom_keybindingsForSettingsEditor();
            const std::string confirmPrompt = std::format(
                "\n\001{}\002Reset all settings to default? (y/n): \001{}\002",
                color,
                UI::Palette::BoldReset
            );

            auto drawSettings = [&] { screen.draw(); };
            drawSettings();

            armPendingInputRestore();
            RlLine confirmInput = term::readlineWithRepaint(confirmPrompt, drawSettings);
            rl_startup_hook = nullptr;

            if (!confirmInput) {
                return;
            }
            if (!isConfirmed(trim(confirmInput.get()))) {
                return;
            }
            applyDefaultsAndReport(screen);
        }

        /**
         * @brief Iteratively edits a sorted list of 1-based setting indices until complete or aborted.
         * @param configPath Path to the configuration file.
         * @param ordered Sorted 1-based setting indices to edit.
         * @param inputErrors Tokenization errors from the initial selection line (shown on first pass).
         */
        void runMultiEdit(const std::string& configPath,
                          std::vector<int> ordered,
                          const std::vector<std::string>& inputErrors) {
            const std::vector<std::string> noErrors;
            size_t i = 0;
            bool showErrors = true;
            bool done = false;

            while (!done) {
                const size_t n = ordered.size();
                const EditNav r = editSetting(configPath,
                                              CONFIG_ORDERED_DEFAULTS[ordered[i] - 1].key,
                                              showErrors ? inputErrors : noErrors,
                                              i + 1, n);
                showErrors = false;
                done = nav::advance(r, ordered, i);
            }
        }

        /**
         * @brief Parses setting indices/ranges from @p input and launches batch editing.
         * @param screen Active settings screen state.
         * @param input Trimmed user selection string.
         */
        void handleSelection(const view::SettingsScreen& screen, const std::string& input) {
            std::vector<std::string> slots(CONFIG_ORDERED_DEFAULTS.size());
            std::unordered_set<int> choices;

            tokenizeInput(input, slots, choices);

            const std::vector<std::string> inputErrors(
                verboseSets.uniqueErrorTokenMessages.begin(),
                verboseSets.uniqueErrorTokenMessages.end());

            if (choices.empty()) {
                if (!inputErrors.empty()) {
                    view::reportNoValidInput(screen.tc);
                }
                return;
            }

            std::vector<int> ordered(choices.begin(), choices.end());
            std::sort(ordered.begin(), ordered.end());
            runMultiEdit(screen.configPath, std::move(ordered), inputErrors);
        }

        /**
         * @brief Dispatches main-menu commands (`?`, `r`/`R`, or numeric selection tokens).
         * @param screen Active settings screen state.
         * @param input Trimmed input line from the main settings prompt.
         */
        void handleMainInput(view::SettingsScreen& screen, const std::string& input) {
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

    } // namespace flow

} // namespace

// ============================================================================
// Public Entry Points
// ============================================================================

/**
 * @brief Runs the interactive prompt loop for a single configuration setting.
 * @param configPath Path to the configuration file on disk.
 * @param key Configuration key to edit.
 * @param inputErrors Initial error messages to surface on the first draw.
 * @param pos 1-based position of this setting in the current batch.
 * @param total Total number of settings in the current batch.
 * @return Navigation outcome (`Abort`, `Advance`, `Updated`, `Next`, `Prev`, or `GoTo`).
 */
EditNav editSetting(const std::string& configPath, const std::string& key,
                    const std::vector<std::string>& inputErrors,
                    size_t pos, size_t total) {
    auto tc = resolveOptionsTheme();
    const ConfigEntry* entry = findConfigEntry(key);
    if (!entry) {
        return EditNav::Advance;
    }

    const bool isNumeric = entry->numeric;
    std::vector<std::string> validValues = entry->values ? *entry->values : std::vector<std::string>{};

    term::NavBindGuard navGuard{total > 1};
    term::CompletionGuard completionGuard;
    term::CompleteModeGuard completeModeGuard;
    RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;

    std::string current = settings_ops::resolveCurrent(key, entry);
    std::vector<std::string> pendingErrors = inputErrors;

    auto drawEdit = [&] {
        view::drawEditScreen(key, current, tc, entry, validValues, isNumeric, pos, total, pendingErrors);
    };

    auto repaintEditOnResize = [&] {
        SyncOutput sync;
        drawEdit();
        std::cout << "\n";
        repaintCompletionListing();
        std::cout << "\033[A";
    };

    while (true) {
        resetReadlinePagination();
        term::setupSettingReadline(total, isNumeric, validValues);

        {
            SyncOutput sync;
            drawEdit();
            std::cout << "\n";
        }

        const std::string prompt = view::buildValuePrompt(tc);
        RlLine rawInput = term::readlineWithRepaint(prompt, repaintEditOnResize);

        armPendingInputRestore();
        rl_startup_hook = nullptr;

        if (!rawInput || rawInput.get()[0] == 27) {
            return EditNav::Abort;
        }

        const std::string newVal = trim(rawInput.get());

        if (const auto navAction = nav::handleInput(rawInput.get(), pos, total, newVal); navAction.has_value()) {
            if (*navAction == EditNav::Next || *navAction == EditNav::Prev || *navAction == EditNav::GoTo) {
                return *navAction;
            }
            continue;
        }

        if (newVal.empty()) {
            return EditNav::Advance;
        }

        if (!settings_ops::validate(entry, newVal, tc, drawEdit)) {
            continue;
        }

        settings_ops::commitSetting(configPath, key, newVal, tc, current, drawEdit);
        return EditNav::Updated;
    }
}

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
 * - **Resize handling:** every screen shares @c view::SettingsScreen::paintBody,
 *   so a resize repaints the full settings view rather than just the last
 *   message. @c view::SettingsScreen::draw is the repaint callback for both the
 *   main prompt and the reset confirmation, and @c view::paintResetStatus serves
 *   the post-reset status screen while @c view::reportNoValidInput serves the
 *   "No valid input provided" screen. Once a reset is requested, the screen
 *   is repainted without the main prompt line, and neither that line nor the
 *   confirmation line is re-echoed afterwards.
 * - **Clear handling:** <kbd>Ctrl+L</kbd> is bound only on the per-setting
 *   value prompt in @c editSetting, where it dismisses any completion list and
 *   repaints the edit screen via @c term::readlineWithRepaint. The binding is
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
        view::SettingsScreen screen{configPath, resolveOptionsTheme()};

        auto drawSettings = [&] { screen.draw(); };
        drawSettings();

        setup_custom_keybindingsForSettingsEditor();

        const std::string prompt = view::buildMainPrompt(screen.tc);
        armPendingInputRestore();
        RlLine rawInput = term::readlineWithRepaint(prompt, drawSettings);
        rl_startup_hook = nullptr;

        if (!rawInput) {
            break;
        }

        const std::string input = trim(rawInput.get());
        if (input == "\x1b") {
            break;
        }

        flow::handleMainInput(screen, input);
    }
}
