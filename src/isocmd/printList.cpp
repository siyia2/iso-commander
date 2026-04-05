// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"


// Reusable stack buffer for fast integer-to-string conversion without heap allocation
template<std::size_t N = 20>
struct IntBuf {
    char data[N];
    std::string_view format(std::size_t value) {
        auto [end, ec] = std::to_chars(data, data + N, value);
        return {data, static_cast<std::size_t>(end - data)};
    }
};


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, 
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, 
               size_t& currentPage, std::atomic<bool>& isImportRunning) {
    
    if (items.empty()) return;

    static constexpr std::string_view defaultColor = "\033[0;1m";
    static constexpr std::string_view darkCyan     = "\033[38;5;37;1m";
    static constexpr std::string_view magentaBold   = "\033[95;1m";
    static constexpr std::string_view brownBold     = "\033[1;38;5;94m";
    static constexpr std::string_view yellowBold    = "\033[1;93m";
    static constexpr std::string_view orangeBold    = "\033[1;38;5;208m";
    static constexpr std::string_view blueBold      = "\033[94;1m";
    static constexpr std::string_view gray          = "\033[0;2m";
    static constexpr std::string_view reset         = "\033[0m";

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");
    
    const size_t totalItems = items.size();
    const bool disablePagination = (ITEMS_PER_PAGE == 0 || totalItems <= ITEMS_PER_PAGE);
    const size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    
    size_t effectivePage = (disablePagination) ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
    const size_t startIndex = disablePagination ? 0 : (effectivePage * ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);

    IntBuf<> ib1, ib2, ib3, ib4; 
    const size_t maxDigits = ib1.format(totalItems).length();
    const bool isIsoWithAutoUpdate = (isImportRunning.load() && listType == "ISO_FILES" && !isFiltered && !globalIsoFileList.empty());

    std::string output;
    output.reserve(((endIndex - startIndex) * 128) + 1024);
    
    // Exact match of your original start
    output += '\n';

    if (!disablePagination) {
        output.append(brownBold).append("Page ");
        output.append(isOriginal ? darkCyan : theme->accent).append(ib1.format(effectivePage + 1));
        output.append(brownBold).append("/").append(yellowBold).append(ib2.format(totalPages));
        output.append(brownBold).append(" (Items (");
        output.append(isOriginal ? darkCyan : theme->accent).append(ib3.format(startIndex + 1));
        output.append("-").append(ib4.format(endIndex)).append(brownBold).append(")/").append(yellowBold);
        output.append(ib1.format(totalItems)).append(brownBold).append(")");
        
        if (isIsoWithAutoUpdate) {
            output.append(gray).append("\n\n[Auto-Update: List restructures if newISOFound]");
        }
        output.append(defaultColor).append("\n\n"); // The two newlines from your original Header block
    } 
    else if (isIsoWithAutoUpdate) {
        output.append(gray).append("[Auto-Update: List restructures if newISOFound]");
        output.append(defaultColor).append("\n\n");
    }

    // Main Loop (logic unchanged)
    for (size_t i = startIndex; i < endIndex; ++i) {
        const std::string_view seqColor = (i % 2 == 0) ? theme->secondary : theme->accent;
        std::string_view idxStr = ib1.format(i + 1);
        
        output.append(seqColor);
        for (size_t p = 0; p < (maxDigits - idxStr.length()); ++p) output.push_back(' ');
        output.append(idxStr);

        if (isFiltered && !filteringStack.empty() && i < filteringStack.back().originalIndices.size()) {
            output.append(":").append(defaultColor).append("\033[38;5;105;1m");
            output.append(ib2.format(filteringStack.back().originalIndices[i] + 1));
            output.append(defaultColor).append("^ ");
        } else {
            output.append(". ").append(defaultColor);
        }

        const std::string& item = items[i];
        if (listType == "ISO_FILES" || listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(item, listSubType);
            if (!displayConfig::toggleNamesOnly) {
                output.append(isOriginal ? defaultColor : theme->muted).append(dir).append(defaultColor).append("/");
            }
            output.append(listType == "ISO_FILES" ? (isOriginal ? magentaBold : theme->accent) 
                                                  : (isOriginal ? orangeBold : theme->highlight));
            output.append(fname);
        } else if (listType == "MOUNTED_ISOS") {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(item);
            if (displayConfig::toggleFullListUmount) {
                output.append(isOriginal ? blueBold : theme->secondary).append(dirPart);
                output.append(isOriginal ? magentaBold : theme->accent).append(pathPart);
                output.append(isOriginal ? "\033[38;5;245m" : theme->muted).append(hashPart);
            } else {
                output.append(isOriginal ? magentaBold : theme->accent).append(pathPart);
            }
        }
        output.append(reset).append("\n");
    }

    // Footer (logic unchanged)
    if (!disablePagination) {
        output.append("\n").append(brownBold).append("Pagination: ");
        if (effectivePage > 0) output.append("[p] ↵ Previous | ");
        if (effectivePage < totalPages - 1) output.append("[n] ↵ Next | ");
        output.append("[g<num>] ↵ Go to | ").append(defaultColor).append("\n");
    }

    if (hasPendingProcess && !pendingIndices.empty()) {
        output.append("\n\033[1;48;5;19mPending for [\033[1;92mproc\033[0;1;48;5;19m]: ");
        std::string_view pColor = (listType != "IMAGE_FILES" ? magentaBold : orangeBold);
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output.append(pColor).append(pendingIndices[i]);
            if (i < pendingIndices.size() - 1) output.push_back(' ');
        }
        output.append(reset).append("\033[0;1m\n");
    }

    std::cout.write(output.data(), output.size());
}
