// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../readline.h"
#include "../rendering.h"
#include "../sort.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @file pagination.cpp
 * @brief Terminal pagination, navigation commands, and display toggles.
 */

void helpSelections(bool& isAtISOListForHelp, const bool& isUnmount, const bool& isMount);

namespace {

/**
 * @brief Handles 'PgDn' (next page) and 'PgUp' (previous page) navigation commands.
 *
 * @param command The input command string from the user.
 * @param totalPages The total number of available pages.
 * @param currentPage Reference to the current page index, updated if navigation occurs.
 * @param needsClrScrn Reference to the flag set to true if the screen needs to be cleared/redrawn.
 * @return true if the command was a pagination command and successfully handled; false otherwise.
 */
bool handlePaginationCommand(const std::string& command, size_t totalPages, size_t& currentPage, bool& needsClrScrn) {
    if (command == "PgDn") {
        if (totalPages > 0 && currentPage < totalPages - 1)
            currentPage++;
        needsClrScrn = true;
        return true;
    }
    if (command == "PgUp") {
        if (currentPage > 0)
            currentPage--;
        needsClrScrn = true;
        return true;
    }
    return false;
}

/**
 * @brief Handles 'g<num>' direct page-jump commands.
 *
 * @param command The input command string from the user.
 * @param totalPages The total number of available pages.
 * @param currentPage Reference to the current page index, updated if jump is valid.
 * @param needsClrScrn Reference to the flag set to true if the screen needs to be cleared/redrawn.
 * @return true if the command matched the go-to pattern and was handled; false otherwise.
 */
bool handleGoToCommand(const std::string& command, size_t totalPages, size_t& currentPage, bool& needsClrScrn) {
    if (command.size() >= 2 && command[0] == 'g' && std::isdigit(command[1])) {
        try {
            int pageNum = std::stoi(command.substr(1)) - 1;
            if (totalPages > 0 && pageNum >= 0 && pageNum < static_cast<int>(totalPages)) {
                currentPage = pageNum;
                needsClrScrn = true;
            }
        } catch (...) {}
        return true;
    }
    return false;
}

/**
 * @brief Handles the '?' help command, updating ISO list state and triggering help selections.
 *
 * @param isUnmount Flag indicating whether the current operation is 'umount'.
 * @param isMount Flag indicating whether the current operation is 'mount'.
 * @param isAtISOList Pointer to an atomic boolean tracking ISO list position state.
 * @param needsClrScrn Reference to the flag set to true if the screen needs to be cleared/redrawn.
 */
void handleHelpCommand(bool isUnmount, bool isMount, std::atomic<bool>* isAtISOList, bool& needsClrScrn) {
    bool isAtISOListForHelp = isAtISOList ? isAtISOList->load() : false;
    if (isAtISOList) isAtISOList->store(false);
    helpSelections(isAtISOListForHelp, isUnmount, isMount);
    GlobalState::g_filteringIndicator.store(false);
    needsClrScrn = true;
}

/**
 * @brief Handles the '*' filename-only toggle command and refreshes sorting/filtering indices.
 *
 * @param isUnmount Flag indicating whether the current operation is 'umount' (disables toggle).
 * @param isFiltered Flag indicating if the current file list is filtered.
 * @param isConversion Flag indicating if the current operation is an ISO conversion.
 * @param need2Sort Reference to a flag indicating whether sorting needs to be re-run.
 * @param needsClrScrn Reference to the flag set to true if the screen needs to be cleared/redrawn.
 * @param filteredFiles Reference to the vector storing filtered files.
 */
void handleToggleNamesCommand(bool isUnmount, bool isFiltered, bool isConversion, bool& need2Sort, bool& needsClrScrn, std::vector<std::string>& filteredFiles) {
    if (!isUnmount) {
        displayConfig::toggleNamesOnly = !displayConfig::toggleNamesOnly;
        sortAfterFilenamesOnlyFlag();

        if (isFiltered && !filteringStack.empty()) {
            refreshFilteredSourceIndices(
                GlobalState::globalIsoFileList,
                filteringStack,
                filteredFiles
            );
        }
    }

    if (isConversion) {
        need2Sort = true;
    }
    needsClrScrn = true;
}

/**
 * @brief Handles the '~' full-path display toggle command based on the active operation context.
 *
 * @param isMount Flag indicating whether the current operation is 'mount'.
 * @param isUnmount Flag indicating whether the current operation is 'umount'.
 * @param isWrite Flag indicating whether the current operation is 'write2usb'.
 * @param isConversion Flag indicating whether the current operation is an ISO conversion.
 * @param needsClrScrn Reference to the flag set to true if the screen needs to be cleared/redrawn.
 */
void handleToggleFullListCommand(bool isMount, bool isUnmount, bool isWrite, bool isConversion, bool& needsClrScrn) {
    if      (isMount      && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListMount       = !displayConfig::toggleFullListMount;
    else if (isUnmount)                                       displayConfig::toggleFullListUmount      = !displayConfig::toggleFullListUmount;
    else if (isWrite      && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListWrite2usb   = !displayConfig::toggleFullListWrite2usb;
    else if (isConversion && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListConvert2iso = !displayConfig::toggleFullListConvert2iso;
    else if               (!displayConfig::toggleNamesOnly)   displayConfig::toggleFullListCpMvRm      = !displayConfig::toggleFullListCpMvRm;
    needsClrScrn = true;
}

} // namespace

/**
 * @brief Processes standard navigation and help commands for the main application loop.
 *
 * Handles 'PgDn' (next page), 'PgUp' (prev page), 'g<num>' (go to page),
 * '//' (no-op guard), '?' (help), '*' (filename-only toggle), and '~'
 * (full-path toggle).
 *
 * @param command The command string entered by the user.
 * @param totalPages Reference to the total number of pages available.
 * @param currentPage Reference to the current page index.
 * @param isFiltered Flag indicating if the file list is currently filtered.
 * @param needsClrScrn Flag set to true if the screen requires a refresh/clear.
 * @param operation The current operation context (e.g., "mount", "umount", "write2usb").
 * @param need2Sort Flag set to true if re-sorting is required after a toggle.
 * @param isAtISOList Pointer to an atomic boolean tracking ISO list state.
 * @param filteredFiles Vector containing filtered filenames.
 * @return true if the command was handled (caller should usually continue the loop);
 *         false if it was not recognized.
 */
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage,
                                     bool& isFiltered, bool& needsClrScrn, const std::string& operation,
                                     bool& need2Sort, std::atomic<bool>* isAtISOList,
                                     std::vector<std::string>& filteredFiles) {

    const bool isMount      = (operation == "mount");
    const bool isUnmount    = (operation == "umount");
    const bool isWrite      = (operation == "write2usb");
    const bool isConversion = (operation == "ccd2iso" || operation == "mdf2iso" ||
                                operation == "nrg2iso" || operation == "chd2iso" ||
                                operation == "daa2iso");

    if (command.find("//") != std::string::npos)
        return true;

    if (totalPages > 0 && currentPage >= totalPages)
        currentPage = totalPages - 1;

    if (handlePaginationCommand(command, totalPages, currentPage, needsClrScrn))
        return true;

    if (handleGoToCommand(command, totalPages, currentPage, needsClrScrn))
        return true;

    if (command == "?") {
        handleHelpCommand(isUnmount, isMount, isAtISOList, needsClrScrn);
        return true;
    }

    if (command == "*") {
        handleToggleNamesCommand(isUnmount, isFiltered, isConversion, need2Sort, needsClrScrn, filteredFiles);
        return true;
    }

    if (command == "~") {
        handleToggleFullListCommand(isMount, isUnmount, isWrite, isConversion, needsClrScrn);
        return true;
    }

    return false;
}

/**
 * @brief Renders a single page of entries with optional pagination headers and footers.
 *
 * Clears the scroll buffer, optionally displays errors, and outputs the subset of
 * entries corresponding to the given page index along with pagination navigation hints.
 *
 * @param entries             Pre-formatted strings to display.
 * @param currentPage         Current page index (0-based) to render.
 * @param disablePagination   True if pagination is bypassed (all items fit on one page).
 * @param totalPages          Total number of available pages.
 * @param c                   Color theme configuration for list output.
 * @param withErrors          True to display error messages before the list, false otherwise.
 */
static void drawPaginatedPage(const std::vector<std::string>& entries,
                       size_t currentPage,
                       bool disablePagination,
                       size_t totalPages,
                       const PrintListTheme& c,
                       bool withErrors) {
    const size_t totalEntries = entries.size();
    const size_t start = disablePagination ? 0 : (currentPage * GlobalState::ITEMS_PER_PAGE);
    const size_t end = disablePagination ? totalEntries : std::min(start + GlobalState::ITEMS_PER_PAGE, totalEntries);

    SyncOutput sync;

    clearScrollBuffer();
    if (withErrors) displayErrors();

    // 1. One newline above the list
    std::cout << "\n";

    // 2. Pagination header (only if enabled)
    if (!disablePagination) {
        std::cout << c.head << "Page "
                  << c.accent << (currentPage + 1)
                  << c.head << "/" << c.num << totalPages
                  << c.head << " (Items ("
                  << c.accent << (start + 1) << "-" << end
                  << c.head << ")/" << c.num << totalEntries
                  << c.head << ")"
                  << UI::Palette::BoldReset << "\n\n";
    }

    // 3. Entries, directly to stdout
    for (size_t i = start; i < end; ++i) {
        std::cout << entries[i];
    }

    // 4. Pagination footer (only if enabled and more than one page)
    if (!disablePagination && totalPages > 1) {
        std::cout << "\n" << c.head;
        if (currentPage > 0)               std::cout << "[PgUp] Prev | ";
        if (currentPage < totalPages - 1)  std::cout << "[PgDn] Next | ";
        std::cout << "[g#] ↵ GoTo" << UI::Palette::BoldReset << "\n";
    }
}

/**
 * @brief Parses and executes navigation commands such as page turns or direct jumps.
 *
 * Recognizes inputs like "PgUp", "PgDn", or page jump commands matching "g<num>".
 * Updates the current page reference and page-turn flag if a valid page change
 * is possible. Recognized navigation commands are consumed even when they
 * cannot change the current page.
 *
 * @param userInput    The trimmed string input provided by the user.
 * @param totalPages   Total number of valid pages available.
 * @param currentPage  Reference to the current page index (modified if navigation succeeds).
 * @param isPageTurn   Reference to the page-turn flag (set to true if the page changes).
 * @return true        If the input is a recognized navigation command.
 * @return false       If the input is not a recognized navigation command.
 */
static bool handleNavigation(const std::string& userInput,
                             size_t totalPages,
                             size_t& currentPage,
                             bool& isPageTurn) {
    if (userInput.empty()) return false;

    if (userInput.size() >= 2 &&
        userInput[0] == 'g' &&
        std::isdigit(static_cast<unsigned char>(userInput[1]))) {
        try {
            const size_t requestedPage = std::stoul(userInput.substr(1));

            if (requestedPage >= 1 && requestedPage <= totalPages) {
                currentPage = requestedPage - 1;
                isPageTurn = true;
            }
        } catch (...) {}

        return true;
    }
    else if (userInput == "PgDn") {
        if (totalPages > 0 && currentPage < totalPages - 1) {
            ++currentPage;
            isPageTurn = true;
        }

        return true;
    }
    else if (userInput == "PgUp") {
        if (currentPage > 0) {
            --currentPage;
            isPageTurn = true;
        }

        return true;
    }

    return false;
}

/**
 * @brief Paginated display loop used by secondary prompts (e.g. confirming
 * files for Copy/Move/Delete).
 *
 * Draws the current page, reads a line, and handles in-loop navigation
 * ('PgDn' / 'PgUp' / 'g<num>'); anything else is returned to the caller.
 *
 * Optionally disables Readline Tab completion for the duration of input,
 * preventing completion matches from being displayed. The previous Tab
 * binding and completion callback are restored automatically afterward.
 *
 * @param entries              Pre-formatted strings to display.
 * @param promptPrefix         Text prepended to the prompt string.
 * @param promptSuffix         Text appended to the prompt string.
 * @param setupEnvironmentFn   Callback to refresh the environment (e.g. re-print
 *                             static headers).
 * @param isPageTurn           Set to true when the last input was a pagination
 *                             command, false otherwise.
 * @param currentPage          Current page index (0-based). Persists across calls
 *                             to avoid resetting on re-entry.
 * @param disableTabCompletion Set to true to disable Tab completion temporarily;
 *                             defaults to false.
 * @return std::string         The user's non-navigation input string, or "EOF_SIGNAL" on Ctrl+D.
 */
std::string handlePaginatedDisplay(const std::vector<std::string>& entries,
                                   const std::string& promptPrefix,
                                   const std::string& promptSuffix,
                                   const std::function<void()>& setupEnvironmentFn,
                                   bool& isPageTurn,
                                   size_t& currentPage,
                                   bool disableTabCompletion = false) {

    const PrintListTheme c = getListColors();

    bool disablePagination = (GlobalState::ITEMS_PER_PAGE <= 0 || entries.size() <= GlobalState::ITEMS_PER_PAGE);
    size_t totalEntries = entries.size();
    size_t totalPages = disablePagination ? 1 : ((totalEntries + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE);

    auto drawPage = [&](bool withErrors) {
        drawPaginatedPage(entries, currentPage, disablePagination, totalPages, c, withErrors);
    };

    while (true) {
        if (setupEnvironmentFn) setupEnvironmentFn();

        drawPage(true);

        const std::string finalPrompt = promptPrefix + promptSuffix;

        const auto repaint = [&] {
            drawPage(false);
            std::cout << promptHeaderForRepaint(finalPrompt) << "\n";
            repaintCompletionListing();
            std::cout << "\033[A";
        };

        RlLine input = readlineWithResize(
            finalPrompt, repaint, disableTabCompletion
        );

        if (!input.get()) return "EOF_SIGNAL";

        std::string userInput = trimWhitespace(input.get());

        if (handleNavigation(userInput, totalPages, currentPage, isPageTurn)) {
            continue;
        }

        isPageTurn = false;
        return userInput;
    }
}
