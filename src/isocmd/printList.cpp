// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"

/**
 * @file list_renderer.cpp
 * @brief Optimized terminal list rendering with pagination, color themes, and stack-based formatting.
 */

/**
 * @brief Reusable stack buffer for fast integer-to-string conversion without heap allocation.
 * Defaults to 20 characters, sufficient for a 64-bit integer.
 */
template<std::size_t N = 20>
struct IntBuf {
    char data[N];
    std::string_view format(std::size_t value) {
        auto [end, ec] = std::to_chars(data, data + N, value);
        return {data, static_cast<std::size_t>(end - data)};
    }
};

/**
 * @brief Renders formatted lists (ISO, Image, or Mounts) to the terminal.
 * * Performance Note: Uses a large reserved std::string buffer and std::cout.write 
 * to minimize syscall overhead and flickering during high-frequency updates.
 * * @param items The list of strings to display.
 * @param listType Category of the list (e.g., "ISO_FILES").
 * @param listSubType Extension or sub-format details.
 * @param pendingIndices Current user selection indices awaiting processing.
 * @param hasPendingProcess Flag indicating if a process action is staged.
 * @param isFiltered Flag indicating if a search filter is currently active.
 * @param currentPage Mutable reference to the current pagination index.
 * @param isImportRunning Atomic flag to detect if a background scan is active.
 */
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, 
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, 
               size_t& currentPage, std::atomic<bool>& isImportRunning) {
    
    if (items.empty()) return;

    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");
    
    // --- Pagination Logic ---
    const size_t totalItems = items.size();
    const bool disablePagination = (ITEMS_PER_PAGE == 0 || totalItems <= ITEMS_PER_PAGE);
    const size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    
    size_t effectivePage = (disablePagination) ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
    const size_t startIndex = disablePagination ? 0 : (effectivePage * ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);

    // --- Performance: Pre-calculate Gutter Width ---
    IntBuf<> ib1, ib2, ib3, ib4; 
    const size_t maxDigits = ib1.format(endIndex).length();

    // Check for active imports based on list type
    bool isIsoList = (listType == "ISO_FILES");
    bool isChdList = (listType == "CHD_FILES");
    const bool isWithAutoUpdate = isImportRunning.load() && !isFiltered && 
                                  ((isIsoList && !globalIsoFileList.empty()) || 
                                   (isChdList && !globalChdFileList.empty()));

    // --- Color Mapping ---
    std::string_view accentColor = isOriginal ? originalColors::darkCyan : theme->accent;
    std::string_view headColor   = isOriginal ? originalColors::brown    : theme->muted; 
    std::string_view numColor    = isOriginal ? originalColors::yellow   : theme->warning; 
    std::string_view isoColor    = isOriginal ? originalColors::magenta  : theme->accent; 
    std::string_view imgColor    = isOriginal ? originalColors::orange   : theme->highlight; 
    std::string_view chdColor    = isOriginal ? originalColors::purple   : theme->highlight; // CHD Color
    std::string_view mntColor    = isOriginal ? originalColors::blue     : theme->secondary; 
    std::string_view squareColor = originalColors::dimGray;
    std::string_view indexA      = isOriginal ? originalColors::red      : theme->secondary;
    std::string_view indexB      = isOriginal ? originalColors::green    : theme->accent;

    // --- Output Buffering ---
    std::string output;
    output.reserve(((endIndex - startIndex) * 128) + 1024);
    output += '\n';

    // --- Header ---
    if (!disablePagination) {
        output.append(headColor).append("Page ")
              .append(accentColor).append(ib1.format(effectivePage + 1))
              .append(headColor).append("/").append(numColor).append(ib2.format(totalPages))
              .append(headColor).append(" (Items (")
              .append(accentColor).append(ib3.format(startIndex + 1))
              .append("-").append(ib4.format(endIndex)).append(headColor).append(")/").append(numColor)
              .append(ib1.format(totalItems)).append(headColor).append(")");
        
        if (isWithAutoUpdate) {
            std::string label = isIsoList ? "newISOFound" : "newCHDFound";
            output.append(originalColors::dim).append("\n\n[Auto-Update: List restructures if ").append(label).append("]");
        }
        output.append(originalColors::boldAlt).append("\n\n");
    } 
    else if (isWithAutoUpdate) {
        std::string label = isIsoList ? "newISOFound" : "newCHDFound";
        output.append(originalColors::dim).append("[Auto-Update: List restructures if ").append(label).append("]\n\n");
    }

    // --- Main Item Loop ---
    for (size_t i = startIndex; i < endIndex; ++i) {
        const std::string_view seqColor = (i % 2 == 0) ? indexA : indexB;
        std::string_view idxStr = ib1.format(i + 1);
        
        output.append(seqColor);
        if (idxStr.length() < maxDigits) {
            output.append(maxDigits - idxStr.length(), ' ');
        }
        output.append(idxStr);

        if (isFiltered && !filteringStack.empty() && i < filteringStack.back().originalIndices.size()) {
            output.append(":").append(originalColors::boldAlt).append(squareColor); 
            output.append(ib2.format(filteringStack.back().originalIndices[i] + 1));
            output.append(originalColors::boldAlt).append(squareColor).append("^ ");
        } else {
            output.append(". ").append(originalColors::boldAlt);
        }

        const std::string& item = items[i];
        
        // --- Context Sensitive Rendering ---
        if (listType == "ISO_FILES" || listType == "IMAGE_FILES" || listType == "CHD_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(item, listSubType);
            if (!displayConfig::toggleNamesOnly) {
                output.append(isOriginal ? originalColors::boldAlt : theme->muted).append(dir).append(originalColors::boldAlt).append("/");
            }
            
            // Choose color based on type
            std::string_view fileColor = (listType == "ISO_FILES") ? isoColor : 
                                         (listType == "IMAGE_FILES") ? imgColor : chdColor;
            output.append(fileColor).append(fname);
        } 
        else if (listType == "MOUNTED_ISOS") {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(item);
            if (displayConfig::toggleFullListUmount) {
                output.append(mntColor).append(dirPart)
                      .append(isoColor).append(pathPart)
                      .append(squareColor).append(hashPart);
            } else {
                output.append(isoColor).append(pathPart);
            }
        }
        output.append(originalColors::boldAlt).append("\n");
    }

    // --- Footer ---
    if (!disablePagination) {
        output.append("\n").append(headColor).append("Pagination: ");
        if (effectivePage > 0) output.append("[p] ↵ Previous | ");
        if (effectivePage < totalPages - 1) output.append("[n] ↵ Next | ");
        output.append("[g<num>] ↵ Go to | ").append(originalColors::boldAlt).append("\n");
    }

    // --- Pending Processes ---
    if (hasPendingProcess && !pendingIndices.empty()) {
        output.append("\n");
        std::string_view bracketBg = isOriginal ? originalColors::bgNavy : theme->background;
        std::string_view procText   = isOriginal ? originalColors::green  : theme->accent;

        output.append(bracketBg).append("Pending for [")
              .append(procText).append("proc")
              .append(originalColors::boldAlt).append(bracketBg).append("]: ");

        // Color coordination for pending list
        std::string_view pColor = (listType == "IMAGE_FILES") ? imgColor : 
                                  (listType == "CHD_FILES") ? chdColor : isoColor;
        output.append(pColor);
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output.append(pendingIndices[i]);
            if (i < pendingIndices.size() - 1) output.push_back(' ');
        }
        output.append(originalColors::boldAlt).append("\n");
    }

    std::cout.write(output.data(), output.size());
}
