// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @file pagination.cpp
 * @brief Logic for terminal pagination, navigation commands, and display toggles.
 */

/**
 * @brief Processes standard navigation and help commands for the main application loop.
 * * Handles 'n' (next), 'p' (prev), 'g<num>' (go to), and special toggles like '*' 
 * (filename-only mode) and '~' (full path toggle).
 * * @return true if a pagination/help command was handled (caller should usually continue the loop).
 * @return false if the command was not recognized as a pagination/help command.
 */
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& isFiltered, bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, bool& need2Sort, std::atomic<bool>& isAtISOList) {
    
    if (command.find("//") != std::string::npos) {
        return true;
    }
    
    if (totalPages > 0 && currentPage >= totalPages) {
        currentPage = totalPages - 1;
    }
    
    if (command == "n") {
        if (totalPages > 0 && currentPage < totalPages - 1) {
            currentPage++;
            needsClrScrn = true;
        }
        return true;
    }

    if (command == "p") {
        if (currentPage > 0) {
            currentPage--;
            needsClrScrn = true;
        }
        return true;
    }

    if (command.size() >= 2 && command[0] == 'g' && std::isdigit(command[1])) {
        try {
            int pageNum = std::stoi(command.substr(1)) - 1; 
            if (totalPages > 0 && pageNum >= 0 && pageNum < static_cast<int>(totalPages)) {
                currentPage = pageNum;
                needsClrScrn = true;
            }
        } catch (...) { /* Ignore invalid formats */ }
        return true;
    }
    
    if (command == "?") {
        isAtISOList.store(false);
        helpSelections();
        needsClrScrn = true;
        return true;
    }
    
    if (command == "*" && !isFiltered) {
        if (!isUnmount) {
            displayConfig::toggleNamesOnly = !displayConfig::toggleNamesOnly;
            
            auto sortJob = [](std::vector<std::string>& list, std::mutex& mtx) {
                std::lock_guard<std::mutex> lock(mtx);
                sortFilesCaseInsensitive(list);
            };

            std::thread(sortJob, std::ref(globalIsoFileList), std::ref(updateListMutex)).detach();
            std::thread(sortJob, std::ref(binImgFilesCache), std::ref(binImgCacheMutex)).detach();
            std::thread(sortJob, std::ref(mdfMdsFilesCache), std::ref(mdfMdsCacheMutex)).detach();
            std::thread(sortJob, std::ref(nrgFilesCache), std::ref(nrgCacheMutex)).detach();
        }
        
        if (isConversion) need2Sort = true;
        needsClrScrn = true;
        return true;
    } 
    else if (command == "*" && isFiltered) return true;
        
    if (command == "~") {
        if (isMount && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListMount = !displayConfig::toggleFullListMount;
        else if (isUnmount) displayConfig::toggleFullListUmount = !displayConfig::toggleFullListUmount;
        else if (isWrite && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListWrite2usb = !displayConfig::toggleFullListWrite2usb;
        else if (isConversion && !displayConfig::toggleNamesOnly) displayConfig::toggleFullListConvert2iso = !displayConfig::toggleFullListConvert2iso;
        else if (!displayConfig::toggleNamesOnly) displayConfig::toggleFullListCpMvRm = !displayConfig::toggleFullListCpMvRm;
        
        needsClrScrn = true;
        return true;
    }

    return false;
}

/**
 * @brief A self-contained loop for displaying and navigating paginated entries during 
 * secondary prompts (like confirming files for Copy/Move).
 * * @param entries The pre-formatted strings to display.
 * @param setupEnvironmentFn Callback to refresh environment (e.g., re-printing static headers).
 * @return The user's non-navigation input string, or "EOF_SIGNAL" on Ctrl+D.
 */
std::string handlePaginatedDisplay(const std::vector<std::string>& entries, 
                                  std::unordered_set<std::string>& uniqueErrorMessages, 
                                  const std::string& promptPrefix, 
                                  const std::string& promptSuffix, 
                                  const std::function<void()>& setupEnvironmentFn, 
                                  bool& isPageTurn) {

    const bool isOriginal = (globalTheme == "original");
    const MainTheme* theme = getActiveTheme();

    // Map to originalColors RGB values
    std::string_view labelCol = isOriginal ? originalColors::brown  : theme->muted;
    std::string_view valueCol = isOriginal ? originalColors::cyan    : theme->accent;
    std::string_view totalCol = isOriginal ? originalColors::yellow  : theme->warning;
    std::string_view resetCol = originalColors::boldAlt; 

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

        if (!disablePagination && totalPages > 1) {
            // RESTORED: Everything on this line uses labelCol until the very end
            pageContent << "\n" << labelCol << "Pagination: ";
            if (currentPage > 0)                pageContent << "[p] ↵ Previous | ";
            if (currentPage < totalPages - 1)  pageContent << "[n] ↵ Next | ";
            pageContent << "[g<num>] ↵ Go to | " << resetCol << "\n";
        }

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
                } catch (...) { isNavigation = true; }
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
