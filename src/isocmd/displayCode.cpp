// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"

/**
 * @namespace displayConfig
 * @brief Default display configuration options for UI lists.
 */
namespace displayConfig {
    bool toggleFullListMount = false;
    bool toggleFullListUmount = true;
    bool toggleFullListCpMvRm = false;
    bool toggleFullListWrite = false;
    bool toggleFullListConversions = false;
    bool toggleNamesOnly = false;
}

/**
 * @brief Loads ISO files from the database and updates the display.
 * * Synchronizes the global ISO list with the database file if modifications are detected
 * and handles UI state updates including filtering and pagination.
 * * @param filteredFiles Reference to the vector of currently filtered files.
 * @param isFiltered Boolean flag indicating if a filter is active.
 * @param listSubType String representing the sub-category of the list.
 * @param umountMvRmBreak Flag to force a break/refresh in the UI state.
 * @param pendingIndices Vector of indices currently marked for processing.
 * @param hasPendingProcess Boolean flag indicating if there are active background tasks.
 * @param currentPage Current page index for pagination.
 * @param originalPage Backup of the page index before filtering.
 * @param isImportRunning Atomic flag monitoring active import operations.
 * @return true if the list contains items, false if the cache is empty.
 */
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning) {

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    static std::mutex lastModifiedMutex;
    static std::filesystem::file_time_type lastModifiedTime;

    bool needToReload = false;

    {
        std::lock_guard<std::mutex> lock(lastModifiedMutex);
        if (std::filesystem::exists(databaseFilePath)) {
            std::filesystem::file_time_type currentModifiedTime =
                std::filesystem::last_write_time(databaseFilePath);
            if (lastModifiedTime == std::filesystem::file_time_type{} ||
                currentModifiedTime > lastModifiedTime) {
                needToReload = true;
            }
            lastModifiedTime = currentModifiedTime;
        } else {
            needToReload = true;
            lastModifiedTime = std::filesystem::file_time_type{};
        }
    }

    clearScrollBuffer();

    std::vector<std::string> freshList;
    if (needToReload) {
        loadFromDatabase(freshList);
    }

    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(updateListMutex);

        if (needToReload) {
            globalIsoFileList = std::move(freshList);
            currentPage = originalPage;
            pendingIndices.clear();
            hasPendingProcess = false;

            if (isFiltered) {
                filteringStack.clear();
                isFiltered = false;
            }

            sortFilesCaseInsensitive(globalIsoFileList);
        }
        
        if (needSortingAfterflno) {
            sortFilesCaseInsensitive(globalIsoFileList);
            needSortingAfterflno = false;
        }

        if (umountMvRmBreak) {
            filteringStack.clear();
            isFiltered = false;
        }

        printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);

        isEmpty = globalIsoFileList.empty();
    }

    if (isEmpty) {
		
		const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");
		
		const std::string_view warnColor   = isOriginal ? originalColors::yellow  : theme->warning;
		const std::string_view reset       = originalColors::reset;

		std::cout << "\n" << warnColor << "ISO Cache is empty. Choose 'ImportISO' from the Main Menu Options." << reset << "\n";
		std::cout << color << "\n↵ to return..." << reset;

		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		return false;
	}

    return true;
}

/**
 * @brief Base path where ISO files are mounted.
 */
const std::string MOUNTED_ISO_PATH = "/mnt";

/**
 * @brief Scans and displays currently mounted ISO directories.
 * * Checks for directories matching the 'iso_' pattern in the mount path and uses 
 * hashing to determine if the directory list requires a re-sort/refresh.
 * * @param isoDirs Vector to store found mount paths.
 * @param filteredFiles Reference to filtered results.
 * @param isFiltered Filter state.
 * @param umountMvRmBreak UI refresh flag.
 * @param pendingIndices Marks items for processing.
 * @param hasPendingProcess Process state flag.
 * @param currentPage Current pagination index.
 * @param originalPage Original page index backup.
 * @param isImportRunning Atomic flag for import status.
 * @return true if mount points exist, false otherwise.
 */
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    
    static size_t previousHash = 0;
    static std::vector<std::string> lastSortedDirs;
    std::vector<std::string> newIsoDirs;

    for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH)) {
        if (entry.is_directory()) {
            auto filename = entry.path().filename().string();
            if (filename.find("iso_") == 0) { 
                newIsoDirs.push_back(entry.path().string());
            }
        }
    }

    size_t currentHash = 0;
    for (const auto& path : newIsoDirs) {
        currentHash += std::hash<std::string>{}(path);
    }
    currentHash += newIsoDirs.size();

    if (currentHash != previousHash) {
        sortFilesCaseInsensitive(newIsoDirs);
        lastSortedDirs = newIsoDirs;
        previousHash = currentHash;
        pendingIndices.clear();
        hasPendingProcess = false;
    } else {
        newIsoDirs = lastSortedDirs;
    }

    isoDirs = std::move(newIsoDirs);

	if (isoDirs.empty()) {
		clearScrollBuffer();
		
		const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");
		
		// Determine colors using ternary operators
		const std::string_view warnColor   = isOriginal ? originalColors::yellow : theme->warning;
		const std::string_view reset       = originalColors::reset;

		std::cerr << "\n" << warnColor << "No paths matching the '/mnt/iso_{name}' pattern found." << reset << "\n";
		std::cout << color << "\n↵ to return..." << reset;

		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		// Clear memory resources
		std::vector<std::string>().swap(isoDirs); 
		std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>>().swap(cachedParsesForUmount);
		
		return false;
	}
	
    clearScrollBuffer();

    if (filteredFiles.size() == isoDirs.size() || umountMvRmBreak) {
        originalPage = currentPage;
        filteringStack.clear();
        isFiltered = false; 
        filteredFiles.clear();
    }
    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);

    return true;
}

/**
 * @brief Loads and displays non-ISO image files (BIN, IMG, MDF, NRG).
 * * Implements a caching mechanism to avoid redundant sorting and leverages specific 
 * mutexes for different image formats to ensure thread-safe updates.
 * * @param files Current list of files to display.
 * @param fileType Extension type of the files (e.g., "bin", "nrg").
 * @param need2Sort Boolean indicating if the list requires sorting.
 * @param isFiltered Filter state.
 * @param list Flag indicating if this is a fresh listing or an update.
 * @param pendingIndices Indices marked for conversion or processing.
 * @param hasPendingProcess Global processing flag.
 * @param currentPage Current page for the display.
 * @param isImportRunning Status of the importer.
 */
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning) {
    clearScrollBuffer();
    
    files = 
    (!isFiltered && !binImgFilesCache.empty() && (fileType == "bin" || fileType == "img") &&
     (binImgFilesCache.size() != files.size() || !std::equal(binImgFilesCache.begin(), binImgFilesCache.end(), files.begin())))
        ? (need2Sort = true, binImgFilesCache) 
    : (!isFiltered && !mdfMdsFilesCache.empty() && fileType == "mdf" &&
       (mdfMdsFilesCache.size() != files.size() || !std::equal(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), files.begin())))
        ? (need2Sort = true, mdfMdsFilesCache) 
    : (!isFiltered && !nrgFilesCache.empty() && fileType == "nrg" &&
       (nrgFilesCache.size() != files.size() || !std::equal(nrgFilesCache.begin(), nrgFilesCache.end(), files.begin())))
        ? (need2Sort = true, nrgFilesCache) 
    : files;
            
    if (!list || (list && needSortingAfterflno)) {
        if (need2Sort) {
            sortFilesCaseInsensitive(files);
                if (fileType == "bin" || fileType == "img") {
                    std::lock_guard<std::mutex> lock(binImgCacheMutex);
                    sortFilesCaseInsensitive(binImgFilesCache);
                } else if (fileType == "mdf") {
                    std::lock_guard<std::mutex> lock(mdfMdsCacheMutex);
                    sortFilesCaseInsensitive(mdfMdsFilesCache);
                } else {
                    std::lock_guard<std::mutex> lock(nrgCacheMutex);
                    sortFilesCaseInsensitive(nrgFilesCache);
                }
            }
            needSortingAfterflno = false;
            need2Sort = false;
    }
    
    printList(files, "IMAGE_FILES", "conversions", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
}
