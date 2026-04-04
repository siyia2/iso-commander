// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"


// Reusable stack buffer — avoid per-iteration heap allocations from std::to_string.
// format() writes via std::to_chars and returns a string_view into the same buffer,
// valid until the next call on that buffer — safe here since we consume it immediately.
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
    
    // 1. Theme Selection & Identity Check
	const ListTheme* theme = getActiveTheme();
	const bool isOriginal = (globalListTheme == "original");

    // Original Fidelity Color Codes
    static constexpr std::string_view defaultColor = "\033[0;1m";
    static constexpr std::string_view darkCyan      = "\033[38;5;37;1m";
    static constexpr std::string_view magentaBold   = "\033[95;1m";
    static constexpr std::string_view brownBold     = "\033[1;38;5;94m";
    static constexpr std::string_view yellowBold    = "\033[1;93m";
    static constexpr std::string_view orangeBold    = "\033[1;38;5;208m";
    static constexpr std::string_view blueBold      = "\033[94;1m";
    static constexpr std::string_view gray          = "\033[0;2m";
    static constexpr std::string_view reset         = "\033[0m";

    // 2. Pagination Logic
    const bool disablePagination = (ITEMS_PER_PAGE == 0 || items.size() <= ITEMS_PER_PAGE);
    const size_t totalItems = items.size();
    const size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    
    size_t effectiveCurrentPage = disablePagination ? 0 : currentPage;
    if (totalPages > 0 && effectiveCurrentPage >= totalPages)
        effectiveCurrentPage = totalPages - 1;

    const size_t startIndex = disablePagination ? 0 : (effectiveCurrentPage * ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);
    const size_t currentNumDigits = std::to_string(totalItems).length();
    
    const bool isIsoWithAutoUpdate = (isImportRunning.load() && listType == "ISO_FILES" && !isFiltered && !globalIsoFileList.empty());
    
    // 3. Output Buffer
    std::string output;
    output.reserve((endIndex - startIndex) * 100 + 512); 
    output += '\n';
    
    // 4. Header
    if (!disablePagination) {
        output += brownBold;
        output += "Page ";
        output += isOriginal ? darkCyan : theme->accent;
        output += std::to_string(effectiveCurrentPage + 1);
        output += brownBold;
        output += "/";
        output += yellowBold;
        output += std::to_string(totalPages);
        output += brownBold;
        output += " (Items (";
        output += isOriginal ? darkCyan : theme->accent;
        output += std::to_string(startIndex + 1);
        output += '-';
        output += std::to_string(endIndex);
        output += brownBold;
        output += ")/";
        output += yellowBold;
        output += std::to_string(totalItems);
        output += brownBold;
        output += ')';
        
        if (isIsoWithAutoUpdate) {
            output += gray;
            output += "\n\n[Auto-Update: List restructures if newISOFound]";
        }
        output += defaultColor;
        output += "\n\n";
    } else if (isIsoWithAutoUpdate) {
        output += gray;
        output += "[Auto-Update: List restructures if newISOFound]";
        output += defaultColor;
        output += "\n\n";
    }

    // 5. Main List Loop
    const std::string padding(currentNumDigits, ' ');
    for (size_t i = startIndex; i < endIndex; ++i) {
        const std::string_view seqColor = (i % 2 == 0) ? theme->secondary : theme->accent;
        const std::string indexStr = std::to_string(i + 1);
        
        output += seqColor;
        output.append(padding.data(), currentNumDigits - indexStr.length());
        output += indexStr;

        // Unified Filter Logic: FilteredIndex:OriginalIndex^ 
        if (isFiltered && !filteringStack.empty() && i < filteringStack.back().originalIndices.size()) {
            output += ':'; 
            output += defaultColor;
            output += "\033[38;5;105;1m"; // magentaBoldDark
            output += std::to_string(filteringStack.back().originalIndices[i] + 1);
            output += defaultColor;
            output += "^ "; 
        } else {
            output += ". ";
            output += defaultColor;
        }

        const std::string& currentItem = items[i];
        if (listType == "ISO_FILES" || listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(currentItem, listSubType);
            if (!displayConfig::toggleNamesOnly) {
                output += isOriginal ? defaultColor : theme->muted;
                output += dir;
                output += defaultColor;
                output += "/";
            }
            
            if (listType == "ISO_FILES") {
                output += isOriginal ? magentaBold : theme->accent;
            } else {
                // IMAGE_FILES: highlight keeps it visually distinct from ISOs
                output += isOriginal ? orangeBold : theme->highlight;
            }
            output += fname;
        } 
        else if (listType == "MOUNTED_ISOS") {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(currentItem);
            
            // Re-introduced toggle logic
            if (displayConfig::toggleFullListUmount) {
                // Show full details: Directory + Path + Hash
                output += isOriginal ? blueBold : theme->secondary;
                output += dirPart;
                output += isOriginal ? magentaBold : theme->accent;
                output += pathPart;
                output += isOriginal ? "\033[38;5;245m" : theme->muted;
                output += hashPart;
            } else {
                // Show path only (Simplified view)
                output += isOriginal ? magentaBold : theme->accent;
                output += pathPart;
            }
        }

        output += reset;
        output += '\n';
    }

    // 6. Footer & Navigation
    if (!disablePagination) {
        output += '\n';
        output += brownBold;
        output += "Pagination: ";
        if (effectiveCurrentPage > 0) output += "[p] ↵ Previous | ";
        if (effectiveCurrentPage < totalPages - 1) output += "[n] ↵ Next | ";
        output += "[g<num>] ↵ Go to | ";
        output += defaultColor;
        output += '\n';
    }

    // 7. Pending Indices
    if (hasPendingProcess && !pendingIndices.empty()) {
        output += "\n\033[1;48;5;19mPending for [\033[1;92mproc\033[0;1;48;5;19m]: "; 
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output += (listType != "IMAGE_FILES" ? magentaBold : orangeBold);
            output += pendingIndices[i];
            if (i < pendingIndices.size() - 1) output += ' ';
        }
        output += reset;
        output += "\033[0;1m\n";
    }

    std::cout << output;
}
