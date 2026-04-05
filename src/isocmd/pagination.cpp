// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"


// Main pagination function
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& isFiltered, bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, bool& need2Sort, std::atomic<bool>& isAtISOList) {
	
	// To fix a hang
	if (command.find("//") != std::string::npos) {
		// true to continue loop in main
		return true;
	}
	
	// Added proper page validation
	if (totalPages > 0 && currentPage >= totalPages) {
		currentPage = totalPages - 1;
	}
	
    // Handle "next" command
    if (command == "n") {
        if (totalPages > 0 && currentPage < totalPages - 1) {
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
            if (totalPages > 0 && pageNum >= 0 && pageNum < static_cast<int>(totalPages)) {
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
	
	if (command == "*" && !isFiltered) {
		// Async sorting when enabling filename-only mode
		if (!isUnmount) {
			displayConfig::toggleNamesOnly = !displayConfig::toggleNamesOnly;
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
		}
		
		// Flag to initialize list sorting immediately for convert2ISO only
		if (isConversion) need2Sort = true;

		needsClrScrn = true;
		return true;
	} // Do not change to filename-only mode when list is already filtered to maintain index^ validity 
	else if (command == "*" && isFiltered ) return true;
        
	if (command == "~") {
		// Toggle full list display based on operation type
		if (isMount && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListMount = !displayConfig::toggleFullListMount;
		else if (isUnmount) displayConfig::toggleFullListUmount = !displayConfig::toggleFullListUmount;
		else if (isWrite && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListWrite = !displayConfig::toggleFullListWrite;
		else if (isConversion && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListConversions = !displayConfig::toggleFullListConversions;
		else if (!displayConfig::toggleNamesOnly) displayConfig::toggleFullListCpMvRm = !displayConfig::toggleFullListCpMvRm;
		
		needsClrScrn = true;
		return true;
	}
    // If no valid command was found
    return false;
}


// Function that handles all pagination logic for selected entries in Cp/Mv
std::string handlePaginatedDisplay(const std::vector<std::string>& entries, 
                                  std::unordered_set<std::string>& uniqueErrorMessages, 
                                  const std::string& promptPrefix, 
                                  const std::string& promptSuffix, 
                                  const std::function<void()>& setupEnvironmentFn, 
                                  bool& isPageTurn) {

    // 1. Theme Selection
    const bool isOriginal = (globalTheme == "original");
    const ListTheme* theme = getActiveTheme();

    // Semantic color mapping
    // Brown/Muted for labels, Accent for numbers/values
    std::string_view labelCol = isOriginal ? "\033[1;38;5;94m" : theme->muted;
    std::string_view valueCol = isOriginal ? "\033[38;5;37;1m" : theme->accent;
    std::string_view totalCol = isOriginal ? "\033[1;93m"      : theme->accent;
    std::string_view resetCol = "\033[0;1m";

    bool disablePagination = (ITEMS_PER_PAGE <= 0 || entries.size() <= ITEMS_PER_PAGE);
    size_t totalPages = disablePagination ? 1 : ((entries.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
    size_t currentPage = 0; 
    size_t totalEntries = entries.size();

    while (true) {
        if (setupEnvironmentFn) setupEnvironmentFn();

        size_t start = disablePagination ? 0 : (currentPage * ITEMS_PER_PAGE);
        size_t end = disablePagination ? totalEntries : std::min(start + ITEMS_PER_PAGE, totalEntries);

        clearScrollBuffer();
        displayErrors(uniqueErrorMessages);

        std::ostringstream pageContent;

        // 2. Themed Header Construction
        if (!disablePagination) {
            pageContent
                << labelCol << "Page "
                << valueCol << (currentPage + 1)
                << labelCol << "/" << totalCol << totalPages
                << labelCol << " (Items ("
                << valueCol << (start + 1) << "-" << end
                << labelCol << ")/" << totalCol << totalEntries
                << labelCol << ")"
                << resetCol << "\n\n";
        }

        for (size_t i = start; i < end; ++i) {
            pageContent << entries[i];
        }

        // 3. Themed Navigation Footer
        if (!disablePagination && totalPages > 1) {
            pageContent << "\n" << labelCol << "Pagination: ";
            if (currentPage > 0)               pageContent << "[p] ↵ Previous | ";
            if (currentPage < totalPages - 1)  pageContent << "[n] ↵ Next | ";
            pageContent << "[g<num>] ↵ Go to | " << resetCol << "\n";
        }

        // 4. Prompt Construction
        std::string prompt = promptPrefix + pageContent.str() + promptSuffix;

        std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);

        if (!input.get()) return "EOF_SIGNAL";

        std::string userInput = trimWhitespace(input.get());

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
                } catch (...) {
                    isPageTurn = true;
                    isNavigation = true;
                }
            }
            else if (userInput == "n") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage < totalPages - 1) currentPage++;
            }
            else if (userInput == "p") {
                isPageTurn = true;
                isNavigation = true;
                if (currentPage > 0) currentPage--;
            }

            if (isNavigation) continue;
        }

        isPageTurn = false;
        return userInput;
    }
}
