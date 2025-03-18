// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"



// For storing isoFiles in RAM
std::vector<std::string> globalIsoFileList;

// Mutex to prevent race conditions when live updating ISO list
std::mutex updateListMutex;


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered) {
    static const char* defaultColor = "\033[0m";
    static const char* bold = "\033[1m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* blueBold = "\033[94;1m";
    static const char* magenta = "\033[95m";
    static const char* magentaBold = "\033[95;1m";
    static const char* orangeBold = "\033[1;38;5;208m";
    static const char* grayBold = "\033[38;5;245m";
    static const char* brownBold = "\033[1;38;5;130m";
    static const char* yellowBold = "\033[1;93m";

    bool disablePagination = (ITEMS_PER_PAGE == 0 || items.size() <= ITEMS_PER_PAGE);
    size_t totalItems = items.size();
    size_t totalPages = disablePagination ? 1 : (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    size_t effectiveCurrentPage = disablePagination ? 0 : currentPage;

    if (totalPages > 0 && effectiveCurrentPage >= totalPages)
        effectiveCurrentPage = totalPages - 1;

    size_t startIndex = disablePagination ? 0 : (effectiveCurrentPage * ITEMS_PER_PAGE);
    size_t endIndex = disablePagination ? totalItems : std::min(startIndex + ITEMS_PER_PAGE, totalItems);

    std::ostringstream output;
    output << "\n";

    if (!disablePagination) {
        output << brownBold << "Page " << (effectiveCurrentPage + 1) << "/" << totalPages
               << " (Items (" << (startIndex + 1) << "-" << endIndex << ")/\033[1;93m" << totalItems << brownBold << ")"
               << defaultColor << "\n\n";
    }

    // Calculate padding based on current page's maximum index
    size_t currentNumDigits = std::to_string(endIndex).length();
    
    for (size_t i = startIndex; i < endIndex; ++i) {
        const char* sequenceColor = (i % 2 == 0) ? red : green;
        std::string directory, filename, displayPath, displayHash;
        
        // Get the item to display
        std::string currentItem = items[i];

        if (listType == "ISO_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(currentItem, listSubType);
            directory = dir;
            filename = fname;
        } else if (listType == "MOUNTED_ISOS") {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(currentItem);
            directory = dirPart;
            displayPath = pathPart;
            displayHash = hashPart;
        } else if (listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(currentItem, "conversions");
            directory = dir;
            filename = fname;
        }

        // Display index - if filtered, show original index
        size_t currentIndex;
        if (isFiltered && i < filteringStack.back().originalIndices.size()) {
            // Use the original index from our stack (adding 1 for display)
            currentIndex = filteringStack.back().originalIndices[i] + 1;
            // Display both the filtered index and the original index
            std::string filteredIndexStr = std::to_string(i + 1);
            std::string originalIndexStr = std::to_string(currentIndex);
            filteredIndexStr.insert(0, currentNumDigits - filteredIndexStr.length(), ' ');
            
            output << sequenceColor << filteredIndexStr << "." << defaultColor << bold;
            output << "^(" << yellowBold << originalIndexStr << defaultColor << bold << ") ";
        } else {
            // Just use the regular index for non-filtered items
            currentIndex = i + 1;
            std::string indexStr = std::to_string(currentIndex);
            indexStr.insert(0, currentNumDigits - indexStr.length(), ' ');
            
            output << sequenceColor << indexStr << ". " << defaultColor << bold;
        }
        
        if (listType == "ISO_FILES") {
            output << directory << defaultColor << bold << "/" << magenta << filename;
        } else if (listType == "MOUNTED_ISOS") {
            if (displayConfig::toggleFullListUmount)
                output << blueBold << directory << magentaBold << displayPath << grayBold << displayHash;
            else
                output << magentaBold << displayPath;
        } else if (listType == "IMAGE_FILES") {
            if (!directory.empty() && !filename.empty())
                output << directory << "/" << orangeBold << filename;
            else
                output << currentItem;
        }
        output << defaultColor << "\n";
    }

    if (!disablePagination) {
        output << "\n" << brownBold << "Pagination: ";
        if (effectiveCurrentPage > 0) output << "[p] ↵ Previous | ";
        if (effectiveCurrentPage < totalPages - 1) output << "[n] ↵ Next | ";
        output << "[g<num>] ↵ Go to | " << defaultColor << "\n";
    }
    
    // Display pending indices if there are any
    if (hasPendingProcess && !pendingIndices.empty()) {
        output << "\n\033[1;35mPending: ";
        for (size_t i = 0; i < pendingIndices.size(); ++i) {
            output << "\033[1;93m" << pendingIndices[i];
            if (i < pendingIndices.size() - 1) {
                output << " ";
            }
        }
        output << "\033[1;35m ([\033[1;92mproc\033[1;35m] ↵ to process [\033[1;93mclr\033[1;35m] ↵ to clear)\033[0;1m\n";
    }

    std::cout << output.str();
}


// Blacklist function for MDF BIN IMG NRG
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg) {
    const std::string filenameLower = entry.filename().string();
    const std::string ext = entry.extension().string();
    std::string extLower = ext;
    toLowerInPlace(extLower);

    // Default mode: .bin and .img files
    if (!blacklistMdf && !blacklistNrg) {
        if (!((extLower == ".bin" || extLower == ".img"))) {
            return false;
        }
    } 
    // MDF mode
    else if (blacklistMdf) {
        if (extLower != ".mdf") {
            return false;
        }
    } 
    // NRG mode
    else if (blacklistNrg) {
        if (extLower != ".nrg") {
            return false;
        }
    }

    // Blacklisted keywords (previously commented out)
    std::unordered_set<std::string> blacklistKeywords = {};
    
    // Convert filename to lowercase without extension
    std::string filenameLowerNoExt = filenameLower;
    filenameLowerNoExt.erase(filenameLowerNoExt.size() - ext.size());

    // Check blacklisted keywords
    for (const auto& keyword : blacklistKeywords) {
        if (filenameLowerNoExt.find(keyword) != std::string::npos) {
            return false;
        }
    }

    return true;
}
