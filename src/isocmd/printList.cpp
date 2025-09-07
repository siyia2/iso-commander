// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"


// For storing isoFiles in RAM
std::vector<std::string> globalIsoFileList;


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, size_t& currentPage, std::atomic<bool>& isImportRunning) {
    
    // Pre-compile colors as string_view for better performance
    static constexpr std::string_view defaultColor = "\033[0;1m";
    static constexpr std::string_view redBold = "\033[31;1m";
    static constexpr std::string_view greenBold = "\033[32;1m";
    static constexpr std::string_view darkCyan = "\033[38;5;37;1m";
    static constexpr std::string_view blueBold = "\033[94;1m";
    static constexpr std::string_view magentaBold = "\033[95;1m";
    static constexpr std::string_view magentaBoldDark = "\033[38;5;105;1m";
    static constexpr std::string_view orangeBold = "\033[1;38;5;208m";
    static constexpr std::string_view gray = "\033[0;2m";
    static constexpr std::string_view grayBold = "\033[38;5;245m";
    static constexpr std::string_view brownBold = "\033[1;38;5;94m";
    static constexpr std::string_view yellowBold = "\033[1;93m";

    const bool disablePagination = (ITEMS_PER_PAGE == 0 || items.size() <= ITEMS_PER_PAGE);
    const size_t totalItems = items.size();
    const size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    
    size_t effectiveCurrentPage = disablePagination ? 0 : currentPage;
    if (totalPages > 0 && effectiveCurrentPage >= totalPages)
        effectiveCurrentPage = totalPages - 1;

    const size_t startIndex = disablePagination ? 0 : (effectiveCurrentPage * ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);
    
    // Pre-calculate invariants
    const size_t currentNumDigits = std::to_string(endIndex).length();
    const bool isIsoWithAutoUpdate = (isImportRunning.load() && listType == "ISO_FILES" && !isFiltered && globalIsoFileList.size() != 0);
    
    // Pre-allocate output buffer based on display mode
    std::string output;
    size_t estimatedCharsPerLine;
    estimatedCharsPerLine = ((displayConfig::toggleNamesOnly && (listType != "MOUNTED_ISOS")) || listType == "MOUNTED_ISOS")
                        ? 50
                        : 100;
    output.reserve((endIndex - startIndex) * estimatedCharsPerLine + 100); // +100 Extra for headers/footers
    
    // Header
    output += '\n';
    
    if (!disablePagination) {
        output += brownBold;
        output += "Page ";
        output += darkCyan;
        output += std::to_string(effectiveCurrentPage + 1);
        output += brownBold;
        output += '/';
        output += yellowBold;
        output += std::to_string(totalPages);
        output += brownBold;
        output += " (Items (";
        output += darkCyan;
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

    // Pre-compute padding string
    const std::string padding(currentNumDigits, ' ');
    
    // Cache type checks
    const bool isIsoFiles = (listType == "ISO_FILES");
    const bool isMountedIsos = (listType == "MOUNTED_ISOS");
    const bool isImageFiles = (listType == "IMAGE_FILES");
    
    // Main loop - optimized for minimal allocations
    for (size_t i = startIndex; i < endIndex; ++i) {
        // Alternate colors efficiently
        const std::string_view sequenceColor = (i % 2 == 0) ? redBold : greenBold;
        
        // Handle index display
        if (isFiltered && i < filteringStack.back().originalIndices.size()) {
            const size_t originalIndex = filteringStack.back().originalIndices[i] + 1;
            const std::string filteredStr = std::to_string(i + 1);
            
            output += sequenceColor;
            output.append(padding.data(), currentNumDigits - filteredStr.length());
            output += filteredStr;
            output += ':';
            output += defaultColor;
            output += magentaBoldDark;
            output += std::to_string(originalIndex);
            output += defaultColor;
            output += "^ ";
        } else {
            const std::string indexStr = std::to_string(i + 1);
            output += sequenceColor;
            output.append(padding.data(), currentNumDigits - indexStr.length());
            output += indexStr;
            output += ". ";
            output += defaultColor;
        }
        
        // Handle content display - minimize string operations
        const std::string& currentItem = items[i];
        
        if (isIsoFiles) {
            auto [dir, fname] = extractDirectoryAndFilename(currentItem, listSubType);
            if (!displayConfig::toggleNamesOnly) {
                output += dir;
                output += defaultColor;
                output += '/';
            }
            output += magentaBold;
            output += fname;
        } else if (isMountedIsos) {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(currentItem);
            if (displayConfig::toggleFullListUmount) {
                output += blueBold;
                output += dirPart;
                output += magentaBold;
                output += pathPart;
                output += grayBold;
                output += hashPart;
            } else {
                output += magentaBold;
                output += pathPart;
            }
        } else if (isImageFiles) {
            auto [dir, fname] = extractDirectoryAndFilename(currentItem, "conversions");
            if (!displayConfig::toggleNamesOnly) {
                output += dir;
                output += defaultColor;
                output += '/';
            }
            output += orangeBold;
            output += fname;
        }
        
        output += defaultColor;
        output += '\n';
    }

    // Footer
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
    
    // Pending indices
    if (hasPendingProcess && !pendingIndices.empty()) {
        output += "\n\033[1;35mPending: ";
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output += "\033[1;93m";
            output += pendingIndices[i];
            if (i < pendingIndices.size() - 1) {
                output += ' ';
            }
        }
        output += "\033[1;35m ([\033[1;92mproc\033[1;35m] ↵ to process [\033[1;93mclr\033[1;35m] ↵ to clear)\033[0;1m\n";
    }

    // Single write to stdout
    std::cout << output;
}
