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

    const PrintListTheme c = getListColors();

    // --- Pagination Logic ---
    const size_t totalItems = items.size();
    const bool disablePagination = (ITEMS_PER_PAGE == 0 || totalItems <= ITEMS_PER_PAGE);
    const size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    
    size_t effectivePage = (disablePagination) ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
    const size_t startIndex = disablePagination ? 0 : (effectivePage * ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);

    // --- Flags & Config ---
    const bool isIsoMode      = (listType == "ISO_FILES");
    const bool isImgMode      = (listType == "IMAGE_FILES");
    const bool isMountedMode  = (listType == "MOUNTED_ISOS");
    const bool isFileMode     = (isIsoMode || isImgMode);
    const bool showNamesOnly  = displayConfig::toggleNamesOnly;
    const bool showFullUmount = displayConfig::toggleFullListUmount;
    
    IntBuf<> ib1, ib2, ib3, ib4; 
    const size_t maxDigits = ib1.format(endIndex).length();
    const bool isIsoWithAutoUpdate = (isImportRunning.load() && isIsoMode && !globalIsoFileList.empty());

    // --- Output Buffering ---
    std::string output;
    output.reserve(((endIndex - startIndex) * 128) + 1024);
    output += '\n';

    // --- Header ---
    if (!disablePagination) {
        output.append(c.head).append("Page ")
              .append(c.accent).append(ib1.format(effectivePage + 1))
              .append(c.head).append("/").append(c.num).append(ib2.format(totalPages))
              .append(c.head).append(" (Items (")
              .append(c.accent).append(ib3.format(startIndex + 1))
              .append("-").append(ib4.format(endIndex)).append(c.head).append(")/").append(c.num)
              .append(ib1.format(totalItems)).append(c.head).append(")");
        
        if (isIsoWithAutoUpdate) {
            output.append(UI::Palette::Dim).append("\n\n[Auto-Update: List restructures if newISOFound]");
        }
        output.append(UI::Palette::BoldReset).append("\n\n");
    } 
    else if (isIsoWithAutoUpdate) {
        output.append(UI::Palette::Dim).append("[Auto-Update: List restructures if newISOFound]\n\n");
    }

    // --- Main Item Loop ---
    for (size_t i = startIndex; i < endIndex; ++i) {
        const std::string_view seqColor = (i % 2 == 0) ? c.indexA : c.indexB;
        std::string_view idxStr = ib1.format(i + 1);
        
        output.append(seqColor);
        if (idxStr.length() < maxDigits) output.append(maxDigits - idxStr.length(), ' ');
        output.append(idxStr);

        if (isFiltered && !filteringStack.empty() && i < filteringStack.back().originalIndices.size()) {
            output.append(":").append(UI::Palette::BoldReset).append(c.square); 
            output.append(ib2.format(filteringStack.back().originalIndices[i] + 1));
            output.append(UI::Palette::BoldReset).append(c.square).append("^ ");
        } else {
            output.append(". ").append(UI::Palette::BoldReset);
        }

        const std::string& item = items[i];
        
        if (isFileMode) {
            auto [dir, fname] = extractDirectoryAndFilename(item, listSubType);
            if (!showNamesOnly) {
                output.append(c.dir).append(dir).append(UI::Palette::BoldReset).append("/");
            }
            output.append(isIsoMode ? c.iso : c.img).append(fname);
        } 
        else if (isMountedMode) {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(item);
            if (showFullUmount) {
                output.append(c.mnt).append(dirPart).append(UI::Palette::BoldReset)
                      .append(c.iso).append(pathPart)
                      .append(c.square).append(hashPart);
            } else {
                output.append(c.iso).append(pathPart);
            }
        }
        output.append(UI::Palette::BoldReset).append("\n");
    }

    // --- Footer ---
    if (!disablePagination) {
        output.append("\n").append(c.head).append("Pagination: ");
        if (effectivePage > 0) output.append("[PgDn] Previous | ");
        if (effectivePage < totalPages - 1) output.append("[PgUp] Next | ");
        output.append("[g<num>] ↵ Go to | ").append(UI::Palette::BoldReset).append("\n");
    }

    // --- Pending Processes ---
    if (hasPendingProcess && !pendingIndices.empty()) {
        output.append("\n");
        output.append(c.bracketBg).append("Pending Indices [")
              .append(c.procText).append("P")
              .append(UI::Palette::BoldReset).append(c.bracketBg).append("]: ");

        output.append(!isImgMode ? c.iso : c.img);
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output.append(pendingIndices[i]);
            if (i < pendingIndices.size() - 1) output.push_back(' ');
        }
        output.append(UI::Palette::BoldReset).append("\n");
    }

    std::cout.write(output.data(), output.size());
}
