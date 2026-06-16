// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SEARCHES_H
#define SEARCHES_H

// C++ Standard Library Headers
#include <cstdint>
#include <string>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./readline.h"

/**
 * @brief RAII guard for Readline state and custom keybindings during search operations.
 *
 * This class ensures that Readline's completion function and custom keybindings
 * are safely restored to their default or previous states upon exiting the scope,
 * regardless of how that scope terminates (e.g., successful return or error handling).
 *
 * @details On construction, it captures the current Readline completion function.
 * On destruction, it restores the captured completion function, resets specific
 * keybindings (Tab/Form Feed), clears the history buffer, resets Readline
 * pagination via @c resetReadlinePagination(), and executes
 * @c reset_custom_keybindingsForSearches() to clean up search-specific configurations.
 *
 * @note Designed for search-focused logic (e.g., @c refreshForDatabase and
 * @c promptSearchBinImgChdDaaMdfNrg). Do not use this for selection
 * menus, which require @c reset_custom_keybindingsForSelect.
 */
class KeybindingGuard {
public:
    KeybindingGuard()
        : oldCompletion_(rl_attempted_completion_function) {}

    ~KeybindingGuard() {
        rl_attempted_completion_function = oldCompletion_;
        rl_bind_key('\t', prevent_readline_keybindings);
        rl_bind_key('\f', rl_insert);
        clear_history();
        resetReadlinePagination();
        reset_custom_keybindingsForSearches();
    }

    // Non-copyable, non-movable
    KeybindingGuard(const KeybindingGuard&) = delete;
    KeybindingGuard& operator=(const KeybindingGuard&) = delete;

private:
    rl_completion_func_t* oldCompletion_;
};

/**
 * SEARCH DOCUMENTATION & HELP
 */

/**
 * @brief Displays usage help for search operations.
 * @param isCpMv Whether the current context is Copy/Move.
 * @param import2ISO Whether the context is ISO conversion.
 */
void helpSearches(bool isCpMv, bool import2ISO);


/**
 * DATABASE MANAGEMENT & TELEMETRY
 */

/**
 * @brief Retrieves and prints statistics about the SQLite database.
 * @param databaseFilePath Path to the .db file.
 * @param maxDatabaseSize Size limit for warning thresholds.
 */
void displayDatabaseStatistics(
    const std::string& databaseFilePath,
    std::uintmax_t maxDatabaseSize
);

/**
 * @brief Processes special CLI switches related to database maintenance.
 *
 * Handles internal commands like --refresh or --cleanup passed via the
 * search input field.
 */
void databaseSwitches(
    std::string& inputSearch
);

#endif // SEARCHES_H
