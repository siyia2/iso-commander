// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SEARCH_H
#define SEARCH_H

// C++ Standard Library Headers
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "isocmd/input/readline.h"

/**
 * @brief RAII guard for Readline session state during search operations.
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
class SearchReadlineGuard {
public:
    SearchReadlineGuard()
        : oldCompletion_(rl_attempted_completion_function) {}

    ~SearchReadlineGuard() {
        rl_attempted_completion_function = oldCompletion_;
        rl_bind_key('\t', prevent_readline_keybindings);
        rl_bind_key('\f', rl_insert);
        clear_history();
        resetReadlinePagination();
        reset_custom_keybindingsForSearches();
    }

    // Non-copyable, non-movable
    SearchReadlineGuard(const SearchReadlineGuard&) = delete;
    SearchReadlineGuard& operator=(const SearchReadlineGuard&) = delete;

private:
    rl_completion_func_t* oldCompletion_;
};

/**
 * SEARCH DOCUMENTATION & HELP
 */

/**
 * @brief Displays usage help for directory-related prompts.
 * @param isCpMv     True for Copy/Move prompts, false for ISO search prompts.
 * @param import2ISO True for database import, false for image conversion (ignored if @p isCpMv).
 * @param isCopy     True for Copy, false for Move; selects the tips (only used if @p isCpMv).
 */
void helpSearches(bool isCpMv, bool import2ISO, bool isCopy = true);

/**
 * SEARCH FOR IMAGES
 */

/**
 * @brief Non-owning view of cached disc image file paths per supported format.
 *
 * Holds const references to existing cache vectors so callers can pass all
 * format caches together without copying large path lists. The referenced
 * vectors must remain valid for the entire lifetime of this struct.
 */
struct RamCacheStore {
    const std::vector<std::string>& binImg; ///< Cached BIN/IMG file paths (default fallback).
    const std::vector<std::string>& mdfMds; ///< Cached MDF/MDS file paths.
    const std::vector<std::string>& nrg;    ///< Cached NRG file paths.
    const std::vector<std::string>& chd;    ///< Cached CHD file paths.
    const std::vector<std::string>& daaGbi; ///< Cached DAA/GBI file paths.
};

/**
 * @brief Active disc image format mode flags for cache lookup and clearing.
 *
 * When all flags are `false`, BIN/IMG mode is treated as the default.
 */
struct CacheMode {
    bool mdf = false; ///< Select MDF/MDS cache when `true`.
    bool nrg = false; ///< Select NRG cache when `true`.
    bool chd = false; ///< Select CHD cache when `true`.
    bool daa = false; ///< Select DAA/GBI cache when `true`.
};

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
 * Handles internal commands like *stats or !clean passed via the
 * importISO search input field.
 */
void dispatchDatabaseCommands(
    std::string& inputSearch,
    const std::function<void()>& repaintHeader
);

#endif // SEARCH_H
