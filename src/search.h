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
 * @brief RAII guard for readline keybinding and completion state in search functions.
 *
 * Saves the current completion function on construction and restores it
 * along with default keybindings on destruction. Uses
 * @c reset_custom_keybindingsForSearches() for teardown, matching the
 * @c setup_custom_keybindingsForSearches() call in the search function
 * setup lambdas.
 *
 * @note Designed for @c refreshForDatabase and
 *       @c promptSearchBinImgChdDaaMdfNrg. Not suitable for select
 *       functions (those use @c reset_custom_keybindingsForSelect).
 */
class KeybindingGuard {
public:
    KeybindingGuard()
        : oldCompletion_(rl_attempted_completion_function) {}

    ~KeybindingGuard() {
        rl_attempted_completion_function = oldCompletion_;
        rl_bind_key('\t', prevent_readline_keybindings);
        rl_bind_key('\f', rl_insert);
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
