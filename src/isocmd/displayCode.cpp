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
 * Reloads the global ISO list from the database when the dirty flag is set,
 * and handles UI state updates including filtering and pagination.
 *
 * @param filteredFiles Reference to the vector of currently filtered files.
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

    bool needToReload = isoListDirty.exchange(false);

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
        
            sortFilesCaseInsensitive(globalIsoFileList);
        }

        if (needSortingAfterflno) {
            sortFilesCaseInsensitive(globalIsoFileList);
            needSortingAfterflno = false;
        }

        if (umountMvRmBreak) {
            filteringStack.clear();
            filteredFiles.clear();
            isFiltered = false;
        }

        printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);

        isEmpty = globalIsoFileList.empty();
    }

    if (isEmpty) {
        const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");

        const std::string_view warnColor = isOriginal ? originalColors::yellow : theme->warning;
        const std::string_view reset     = originalColors::boldAlt;

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
 * Checks for directories matching the 'iso_' pattern in the mount path and uses
 * stat()-based change detection to determine if the directory list requires a re-sort/refresh.
 * Rescans only when directory metadata changes, and resets state only when iso_ contents differ.
 *
 * @param isoDirs Vector to store found mount paths.
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

    static struct stat lastDirStat{};
    static std::vector<std::string> lastSortedDirs;
    static size_t lastIsoCount = 0;

    struct stat currentDirStat{};
    bool metaChanged = false;

    if (stat(MOUNTED_ISO_PATH.c_str(), &currentDirStat) != 0) {
        // MOUNTED_ISO_PATH missing or inaccessible — treat as empty
        lastSortedDirs.clear();
        lastIsoCount = 0;
        lastDirStat = {};
    } else {
        metaChanged = (currentDirStat.st_mtime != lastDirStat.st_mtime ||
                       currentDirStat.st_nlink != lastDirStat.st_nlink);
        if (metaChanged) {
            std::vector<std::string> newIsoDirs;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH, ec)) {
                if (ec) break;
                if (entry.is_directory()) {
                    auto filename = entry.path().filename().string();
                    if (filename.find("iso_") == 0)
                        newIsoDirs.push_back(entry.path().string());
                }
            }
            sortFilesCaseInsensitive(newIsoDirs);
            lastDirStat = currentDirStat;

            if (newIsoDirs.size() != lastIsoCount || newIsoDirs != lastSortedDirs) {
                lastSortedDirs = std::move(newIsoDirs);
                lastIsoCount   = lastSortedDirs.size();
                pendingIndices.clear();
                hasPendingProcess = false;
            }
        }
    }

    isoDirs = lastSortedDirs;

    if (isoDirs.empty()) {
        clearScrollBuffer();

        const ListTheme* theme = getActiveTheme();
        const bool isOriginal  = (globalTheme == "original");

        const std::string_view warnColor = isOriginal ? originalColors::yellow : theme->warning;
        const std::string_view reset     = originalColors::boldAlt;

        std::cerr << "\n" << warnColor << "No paths matching the '/mnt/iso_{name}' pattern found." << reset << "\n";
        std::cout << color << "\n↵ to return..." << reset;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        std::vector<std::string>().swap(lastSortedDirs);
		lastIsoCount = 0;
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
 * @brief Prepares and displays cached disc image file lists.
 *
 * Restores file lists from the appropriate format cache (BIN/IMG, MDF, NRG, CHD, DAA)
 * when not filtered and when cache state differs from the current view.
 *
 * If required, sorts both the active file list and the corresponding cache,
 * using format-specific mutex protection for thread-safe updates.
 *
 * Finally delegates rendering and interaction to the list display system.
 *
 * @param files Current working file list (may be replaced by cache)
 * @param fileType File extension type ("bin", "img", "mdf", "nrg", "chd", "daa")
 * @param need2Sort Flag indicating whether sorting is required
 * @param isFiltered Filter state affecting cache restoration
 * @param list Controls whether list refresh behavior is executed
 * @param pendingIndices Files queued for processing or conversion
 * @param hasPendingProcess Global processing state flag
 * @param currentPage Pagination state for UI display
 * @param isImportRunning Importer activity flag
 */
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,
                              std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning) {
    clearScrollBuffer();
    
    // Restore from the appropriate cache when not filtered and the cache is valid
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
    : (!isFiltered && !chdFilesCache.empty() && fileType == "chd" &&
       (chdFilesCache.size() != files.size() || !std::equal(chdFilesCache.begin(), chdFilesCache.end(), files.begin())))
        ? (need2Sort = true, chdFilesCache)
    : (!isFiltered && !daaFilesCache.empty() && fileType == "daa" &&   // <-- added DAA branch
       (daaFilesCache.size() != files.size() || !std::equal(daaFilesCache.begin(), daaFilesCache.end(), files.begin())))
        ? (need2Sort = true, daaFilesCache)
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
            } else if (fileType == "nrg") {
                std::lock_guard<std::mutex> lock(nrgCacheMutex);
                sortFilesCaseInsensitive(nrgFilesCache);
            } else if (fileType == "chd") {
                std::lock_guard<std::mutex> lock(chdCacheMutex);
                sortFilesCaseInsensitive(chdFilesCache);
            } else if (fileType == "daa") {   // <-- added DAA sorting
                std::lock_guard<std::mutex> lock(daaCacheMutex);
                sortFilesCaseInsensitive(daaFilesCache);
            }
        }
        needSortingAfterflno = false;
        need2Sort = false;
    }
    
    printList(files, "IMAGE_FILES", "conversions", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
}
