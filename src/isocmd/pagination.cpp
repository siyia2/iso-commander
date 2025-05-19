// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


// Default max entries per listed page
size_t ITEMS_PER_PAGE = 25;


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
	
	if (command == "*") {
		displayConfig::toggleNamesOnly = !displayConfig::toggleNamesOnly;
		if (isUnmount && !displayConfig::toggleNamesOnly) {
			displayConfig::toggleFullListUmount = true;
		}
		std::thread([] {
			std::lock_guard<std::mutex> lock(updateListMutex);
			sortFilesCaseInsensitive(globalIsoFileList);
		}).detach(); // Launch in background and detach
		std::thread([] {
			std::lock_guard<std::mutex> lock(binImgCacheMutex);
			sortFilesCaseInsensitive(binImgFilesCache);
		}).detach();
		std::thread([] {
			std::lock_guard<std::mutex> lock(mdfMdsCacheMutex);
			sortFilesCaseInsensitive(mdfMdsFilesCache);
		}).detach();
		std::thread([] {
			std::lock_guard<std::mutex> lock(nrgCacheMutex);
			sortFilesCaseInsensitive(nrgFilesCache);
		}).detach();
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
    // Determine whether pagination is needed
    bool disablePagination = (ITEMS_PER_PAGE <= 0 || entries.size() <= ITEMS_PER_PAGE);
    
    // Calculate total pages (if pagination is enabled)
    size_t totalPages = disablePagination ? 1 : ((entries.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
    size_t currentPage = 0; // Start at the first page
    size_t totalEntries = entries.size(); // Total number of items

    while (true) {
        // Call the setup function if provided (e.g., to clear screen, set terminal settings)
        if (setupEnvironmentFn) {
            setupEnvironmentFn();
        }

        // Calculate the range of items to display for the current page
        size_t start = disablePagination ? 0 : (currentPage * ITEMS_PER_PAGE);
        size_t end = disablePagination ? totalEntries : std::min(start + ITEMS_PER_PAGE, totalEntries);

        // Clear the scroll buffer (for terminal output)
        clearScrollBuffer();

        // Display any accumulated error messages before showing the entries
        displayErrors(uniqueErrorMessages);

        std::ostringstream pageContent;

        // Display pagination info if applicable
        if (!disablePagination) {
            pageContent << "\033[1;38;5;130mPage \033[38;5;37;1m" << (currentPage + 1) << "\033[1;38;5;130m/\033[1;93m" << totalPages
                        << "\033[1;38;5;130m (Items (\033[38;5;37;1m" << (start + 1) << "-" << end << "\033[1;38;5;130m)/\033[1;93m" << totalEntries << "\033[1;38;5;130m)"
                        << "\033[0m\n\n";
        }

        // Append entries for the current page to the output
        for (size_t i = start; i < end; ++i) {
            pageContent << entries[i];
        }

        // If pagination is enabled, show navigation options
        if (!disablePagination && totalPages > 1) {
            pageContent << "\n\033[1;38;5;130mPagination: ";
            if (currentPage > 0) pageContent << "[p] ↵ Previous | ";
            if (currentPage < totalPages - 1) pageContent << "[n] ↵ Next | ";
            pageContent << "[g<num>] ↵ Go to | \033[0m\n";
        }

        // Construct the prompt with the current page content
        std::string prompt = promptPrefix + pageContent.str() + promptSuffix;

        // Read user input using readline (allocates memory, needs to be freed)
        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);

        // If readline returns null (e.g., CTRL+D), return a special signal
        if (!input.get()) {
            return "EOF_SIGNAL";
        }

        std::string userInput = trimWhitespace(input.get()); // Remove leading/trailing whitespace

        // Process navigation commands
        if (!userInput.empty()) {
            bool isNavigation = false;

            // Check if user input is a page jump command (e.g., "g2" to go to page 2)
            if (userInput.size() >= 2 && userInput[0] == 'g' && std::isdigit(userInput[1])) {
                try {
                    size_t requestedPage = std::stoul(userInput.substr(1)); // Extract page number
                    isPageTurn = true;
                    isNavigation = true;
                    if (requestedPage >= 1 && requestedPage <= totalPages) {
                        currentPage = requestedPage - 1; // Convert to zero-based index
                    }
                } catch (const std::exception&) {
                    isPageTurn = true;
                    isNavigation = true;
                }
            }
            // Move to the next page
            else if (userInput == "n") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage < totalPages - 1) {
                    currentPage++;
                }
            }
            // Move to the previous page
            else if (userInput == "p") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage > 0) {
                    currentPage--;
                }
            }

            // If navigation occurred, restart the loop to display the new page
            if (isNavigation) {
                continue;
            }
        }

        // If no navigation was performed, return the user input for further processing
        isPageTurn = false;
        return userInput;
    }
}
