// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
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
#include "../inputHandling.h"
#include "../sort.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../themes.h"
#include "../verbose.h"

void helpSelections(bool& isAtISOListForHelp);

/**
 * @file pagination.cpp
 * @brief Logic for terminal pagination, navigation commands, and display toggles.
 */

/**
 * @brief Processes standard navigation and help commands for the main application loop.
 *
 * Handles 'PgDn' (next page), 'PgUp' (prev page), 'g<num>' (go to page), '//' (no-op guard),
 * '?' (help), '*' (filename-only toggle), and '~' (full path toggle).
 *
 * @return true if a command was handled (caller should usually continue the loop).
 * @return false if the command was not recognized.
 */
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage,
                                     bool& isFiltered, bool& needsClrScrn, const std::string& operation,
                                     bool& need2Sort, std::atomic<bool>* isAtISOList) {

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

    if (command == "?") {
        bool isAtISOListForHelp = isAtISOList ? isAtISOList->load() : false;
        if (isAtISOList) isAtISOList->store(false);
        helpSelections(isAtISOListForHelp);
        needsClrScrn = true;
        return true;
    }

    if (command == "*") {
        if (!isFiltered && !isUnmount) {
            displayConfig::toggleNamesOnly = !displayConfig::toggleNamesOnly;
            sortAfterFilenamesOnlyFlag();
        }
        if (isConversion) need2Sort = true;
        needsClrScrn = true;
        return true;
    }

    if (command == "~") {
        if      (isMount      && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListMount       = !displayConfig::toggleFullListMount;
        else if (isUnmount)                                       displayConfig::toggleFullListUmount      = !displayConfig::toggleFullListUmount;
        else if (isWrite      && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListWrite2usb   = !displayConfig::toggleFullListWrite2usb;
        else if (isConversion && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListConvert2iso = !displayConfig::toggleFullListConvert2iso;
        else if               (!displayConfig::toggleNamesOnly)   displayConfig::toggleFullListCpMvRm      = !displayConfig::toggleFullListCpMvRm;
        needsClrScrn = true;
        return true;
    }

    return false;
}

/**
 * @brief A self-contained loop for displaying and navigating paginated entries during
 * secondary prompts (like confirming files for Copy/Move).
 *
 * @param entries            The pre-formatted strings to display.
 * @param promptPrefix       Text prepended to the prompt string.
 * @param promptSuffix       Text appended to the prompt string.
 * @param setupEnvironmentFn Callback to refresh environment (e.g., re-printing static headers).
 * @param isPageTurn         Set to true if the last input was a pagination command, false otherwise.
 * @param currentPage        Current page index (0-based). Persists across calls to avoid reset on re-entry.
 * @return The user's non-navigation input string, or "EOF_SIGNAL" on Ctrl+D.
 */
std::string handlePaginatedDisplay(const std::vector<std::string>& entries,
                                  const std::string& promptPrefix,
                                  const std::string& promptSuffix,
                                  const std::function<void()>& setupEnvironmentFn,
                                  bool& isPageTurn,
                                  size_t& currentPage) {

    const PrintListTheme c = getListColors();

    bool disablePagination = (GlobalState::ITEMS_PER_PAGE <= 0 || entries.size() <= GlobalState::ITEMS_PER_PAGE);
    size_t totalEntries = entries.size();
    size_t totalPages = disablePagination ? 1 : ((totalEntries + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE);

    while (true) {
        if (setupEnvironmentFn) setupEnvironmentFn();

        size_t start = disablePagination ? 0 : (currentPage * GlobalState::ITEMS_PER_PAGE);
        size_t end = disablePagination ? totalEntries : std::min(start + GlobalState::ITEMS_PER_PAGE, totalEntries);

        clearScrollBuffer();
        displayErrors();

        // 1. One newline above the list
        std::cout << "\n";

        // 2. Display Pagination Header (Only if enabled)
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

        // 3. Print the entries directly to stdout
        for (size_t i = start; i < end; ++i) {
            std::cout << entries[i];
        }

        // 4. Display Pagination Footer (Only if enabled and more than 1 page)
        if (!disablePagination && totalPages > 1) {
            std::cout << "\n" << c.head;
            if (currentPage > 0)               std::cout << "[PgUp] Prev | ";
            if (currentPage < totalPages - 1)  std::cout << "[PgDn] Next | ";
            std::cout << "[g#] ↵ GoTo | " << UI::Palette::BoldReset << "\n";
        }

        // 5. Construct the final prompt
        // If pagination is disabled, we remove the extra newline that typically
        // sits above the prompt to tighten the layout.
        std::string finalPrompt = promptPrefix + promptSuffix;

        std::unique_ptr<char, decltype(&std::free)> input(readline(finalPrompt.c_str()), &std::free);

        if (!input.get()) return "EOF_SIGNAL";

        std::string userInput = trimWhitespace(input.get());

        if (!userInput.empty()) {
            bool isNavigation = false;

            if (userInput.size() >= 2 && userInput[0] == 'g' && std::isdigit(userInput[1])) {
				isNavigation = true;
                try {
                    size_t requestedPage = std::stoul(userInput.substr(1));
                    if (requestedPage >= 1 && requestedPage <= totalPages) {
                        currentPage = requestedPage - 1;
                        isPageTurn = true;
                        isNavigation = true;
                    }
                } catch (...) {}
            }
            else if (userInput == "PgDn") {
				isNavigation = true;
                if (currentPage < totalPages - 1) {
                    currentPage++;
                    isPageTurn = true;
                    isNavigation = true;
                }
            }
            else if (userInput == "PgUp") {
				isNavigation = true;
                if (currentPage > 0) {
                    currentPage--;
                    isPageTurn = true;
                    isNavigation = true;
                }
            }

            if (isNavigation) continue;
        }

        isPageTurn = false;
        return userInput;
    }
}
