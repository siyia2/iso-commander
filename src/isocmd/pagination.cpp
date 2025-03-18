// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// Main pagination function
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, std::atomic<bool>& isAtISOList) {
	
	// To fix a hang
	if (command.find("//") != std::string::npos) {
		// true to continue loop in main
		return true;
	}
	
    // Handle "next" command
    if (command == "n") {
        if (currentPage < totalPages - 1) {
            currentPage++;
            needsClrScrn = true;
        }
        return true;
    }

    // Handle "prev" command
    if (command == "p") {
        if (currentPage > 0) {
            currentPage--;
            needsClrScrn = true;
        }
        return true;
    }

    // Handle go-to specific page command (e.g., "g3" goes to page 3)
    if (command.size() >= 2 && command[0] == 'g' && std::isdigit(command[1])) {
        try {
            int pageNum = std::stoi(command.substr(1)) - 1; // convert to 0-based index
            if (pageNum >= 0 && pageNum < static_cast<int>(totalPages)) {
                currentPage = pageNum;
                needsClrScrn = true;
            }
        } catch (...) {
            // Ignore invalid page numbers
        }
        return true;
    }
    
     // Handle special commands
	if (command == "?") {
		isAtISOList.store(false);
		helpSelections();
		needsClrScrn = true;
		return true;
	}
        
	if (command == "~") {
		// Toggle full list display based on operation type
		if (isMount) displayConfig::toggleFullListMount = !displayConfig::toggleFullListMount;
		else if (isUnmount) displayConfig::toggleFullListUmount = !displayConfig::toggleFullListUmount;
		else if (isWrite) displayConfig::toggleFullListWrite = !displayConfig::toggleFullListWrite;
		else if (isConversion)  displayConfig::toggleFullListConversions = !displayConfig::toggleFullListConversions;
		else displayConfig::toggleFullListCpMvRm = !displayConfig::toggleFullListCpMvRm;
		needsClrScrn = true;
		return true;
	}

    // If no valid command was found
    return false;
}


// Function that handles all pagination logic for selected entries in Cp/Mv
std::string handlePaginatedDisplay(const std::vector<std::string>& entries, std::unordered_set<std::string>& uniqueErrorMessages, const std::string& promptPrefix, const std::string& promptSuffix, const std::function<void()>& setupEnvironmentFn, bool& isPageTurn) {
    // Setup pagination parameters
    bool disablePagination = (ITEMS_PER_PAGE <= 0 || entries.size() <= ITEMS_PER_PAGE);
    size_t totalPages = disablePagination ? 1 : ((entries.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
    size_t currentPage = 0;
    size_t totalEntries = entries.size();

    while (true) {
        if (setupEnvironmentFn) {
            setupEnvironmentFn();
        }

        size_t start = disablePagination ? 0 : (currentPage * ITEMS_PER_PAGE);
        size_t end = disablePagination ? totalEntries : std::min(start + ITEMS_PER_PAGE, totalEntries);

        clearScrollBuffer();
        displayErrors(uniqueErrorMessages);

        std::ostringstream pageContent;
        if (!disablePagination) {
            pageContent << "\033[1;38;5;130mPage " << (currentPage + 1) << "/" << totalPages
                        << " (Items (" << (start + 1) << "-" << end << ")/\033[1;93m" << totalEntries << "\033[1;38;5;130m)"
                        << "\033[0m\n\n";
        }

        for (size_t i = start; i < end; ++i) {
            pageContent << entries[i];
        }

        if (!disablePagination && totalPages > 1) {
            pageContent << "\n\033[1;38;5;130mPagination: ";
            if (currentPage > 0) pageContent << "[p] ↵ Previous | ";
            if (currentPage < totalPages - 1) pageContent << "[n] ↵ Next | ";
            pageContent << "[g<num>] ↵ Go to | \033[0m\n";
        }

        std::string prompt = promptPrefix + pageContent.str() + promptSuffix;
        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);

        // When readline returns null (CTRL+D), return a special signal
        if (!input.get()) {
            return "EOF_SIGNAL";
        }

        std::string userInput = trimWhitespace(input.get());

        // Navigation commands
        if (!userInput.empty()) {
            bool isNavigation = false;

            if (userInput.size() >= 2 && userInput[0] == 'g' && std::isdigit(userInput[1])) {
                try {
                    size_t requestedPage = std::stoul(userInput.substr(1));
                    isPageTurn = true;
                    isNavigation = true;
                    if (requestedPage >= 1 && requestedPage <= totalPages) {
                        currentPage = requestedPage - 1;
                    }
                } catch (const std::exception&) {
                    isPageTurn = true;
                    isNavigation = true;
                }
            } else if (userInput == "n") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage < totalPages - 1) {
                    currentPage++;
                }
            } else if (userInput == "p") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage > 0) {
                    currentPage--;
                }
            }

            if (isNavigation) {
                continue;
            }
        }

        isPageTurn = false;
        return userInput;
    }
}
