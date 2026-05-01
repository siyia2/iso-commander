// SPDX-License-Identifier: GPL-3.0-or-later

#include "../state.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"
#include "../caches.h"
#include "../sort.h"
#include "../databaseOps.h"
#include "../pausePrompt.h"
#include "../inputHandling.h"

void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, std::vector<std::string>& pendingIndices, 
bool& hasPendingProcess, bool& isFiltered, size_t& currentPage, std::atomic<bool>& isImportRunning);

/**
 * @brief Loads ISO files from the database and updates the display.
 * * Synchronizes the global ISO list with the database when the dirty flag is set.
 * If a filter is active during a reload, it is re-applied to ensure the visible 
 * results and original index mappings remain consistent with the new data.
 *
 * @note If a re-applied filter returns no results, the filter stack is cleared 
 * and the view resets to the full list to prevent a blank UI.
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
    
    bool needToReload = GlobalState::isoListDirty.exchange(false);
    std::vector<std::string> freshList;
    
    if (needToReload) {
        loadFromDatabase(freshList);
    }
    
    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(GlobalCaches::updateListMutex);
        
        if (needToReload) {
            GlobalCaches::globalIsoFileList = std::move(freshList);
            currentPage = originalPage;
            pendingIndices.clear();
            hasPendingProcess = false;
            sortFilesCaseInsensitive(GlobalCaches::globalIsoFileList);
			
			syncFilteringStackForIso(GlobalCaches::globalIsoFileList, filteringStack, filteredFiles, isFiltered);
		}

        if (GlobalState::needSortingAfterflno) {
            sortFilesCaseInsensitive(GlobalCaches::globalIsoFileList);
            GlobalState::needSortingAfterflno = false;
        }

        if (umountMvRmBreak) {
            filteringStack.clear();
            filteredFiles.clear();
            isFiltered = false;
        }

        clearScrollBuffer();
        
        // Use either the recently refreshed filteredFiles or the global master list
        printList(isFiltered ? filteredFiles : GlobalCaches::globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
        
        isEmpty = GlobalCaches::globalIsoFileList.empty();
    }

    if (isEmpty) {
        const PrintListTheme c = getListColors();
        std::cout << "\n" << c.num << "ISO Cache is empty. Choose 'ImportISO' from the Main Menu Options." << c.dir << "\n";
        pressEnterToReturn();
        return false;
    }
    
    return true;
}

/**
 * @brief Base path where ISO files are mounted.
 */
const std::string MOUNTED_ISO_PATH = "/mnt";

/**
 * @brief Scans, filters, and renders currently mounted ISO directories.
 * * Uses stat-based caching to detect changes in MOUNTED_ISO_PATH. A full re-scan 
 * and re-sort occur only if the directory's modification time or link count changes.
 * * If changes are detected and the resulting directory list differs from the cache:
 * - Updates the static cache.
 * - Resets pending process states and indices.
 * * If no ISOs are found, it clears internal caches and displays a warning.
 * Otherwise, it manages pagination state and invokes the UI rendering (printList).
 * * @note Temporarily ignores SIGINT and disables Ctrl+D during execution.
 * * @param isoDirs [out] Vector populated with current mount paths.
 * @param filteredFiles [in/out] List of files post-filtering.
 * @param isFiltered [in/out] Boolean toggle for filtering mode.
 * @param umountMvRmBreak [in] UI flag to force a refresh/reset of the view.
 * @param pendingIndices [in/out] Tracks items marked for batch operations.
 * @param hasPendingProcess [in/out] Flag indicating active background tasks.
 * @param currentPage [in/out] Current UI pagination index.
 * @param originalPage [out] Backup of page index when filtering is reset.
 * @param isImportRunning [in] Atomic flag to prevent UI collisions during imports.
 * @return true if ISOs were found and displayed; false if the directory is empty.
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

        const MainTheme* theme = getActiveTheme();
        const bool isOriginal  = (globalTheme == "original");

        const std::string_view warnColor = isOriginal ? UI::Palette::Yellow : theme->warning;
        const std::string_view reset     = UI::Palette::BoldReset;

        std::cerr << "\n" << warnColor << "No paths matching the '/mnt/iso_{name}' pattern found." << reset << "\n";
        pressEnterToReturn();

        std::vector<std::string>().swap(lastSortedDirs);
		lastIsoCount = 0;
        std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>>().swap(GlobalCaches::cachedParsesForUmount);

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
 * Restores file lists from the appropriate format cache (BIN/IMG, MDF, NRG, CHD, DAA/GBI)
 * when not filtered and when cache state differs from the current view.
 *
 * If required, sorts both the active file list and the corresponding cache,
 * using format-specific mutex protection for thread-safe updates.
 *
 * Finally delegates rendering and interaction to the list display system.
 *
 * @param files Current working file list (may be replaced by cache)
 * @param fileType File extension type ("bin", "img", "mdf", "nrg", "chd", "daa", gbi)
 * @param need2Sort Flag indicating whether sorting is required
 * @param isFiltered Filter state affecting cache restoration
 * @param list Controls whether list refresh behavior is executed
 * @param pendingIndices Files queued for processing or convert2iso
 * @param hasPendingProcess Global processing state flag
 * @param currentPage Pagination state for UI display
 * @param isImportRunning Importer activity flag
 */
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,
                              std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning) {
    clearScrollBuffer();
    
    // Restore from the appropriate cache when not filtered and the cache is valid
    files = 
    (!isFiltered && !GlobalCaches::binImgFilesCache.empty() && (fileType == "bin" || fileType == "img") &&
     (GlobalCaches::binImgFilesCache.size() != files.size() || !std::equal(GlobalCaches::binImgFilesCache.begin(), GlobalCaches::binImgFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalCaches::binImgFilesCache) 
    : (!isFiltered && !GlobalCaches::mdfMdsFilesCache.empty() && fileType == "mdf" &&
       (GlobalCaches::mdfMdsFilesCache.size() != files.size() || !std::equal(GlobalCaches::mdfMdsFilesCache.begin(), GlobalCaches::mdfMdsFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalCaches::mdfMdsFilesCache) 
    : (!isFiltered && !GlobalCaches::nrgFilesCache.empty() && fileType == "nrg" &&
       (GlobalCaches::nrgFilesCache.size() != files.size() || !std::equal(GlobalCaches::nrgFilesCache.begin(), GlobalCaches::nrgFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalCaches::nrgFilesCache)
    : (!isFiltered && !GlobalCaches::chdFilesCache.empty() && fileType == "chd" &&
       (GlobalCaches::chdFilesCache.size() != files.size() || !std::equal(GlobalCaches::chdFilesCache.begin(), GlobalCaches::chdFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalCaches::chdFilesCache)
    : (!isFiltered && !GlobalCaches::daaGbiFilesCache.empty() && fileType == "daa" &&               // <-- DAA branch
       (GlobalCaches::daaGbiFilesCache.size() != files.size() || !std::equal(GlobalCaches::daaGbiFilesCache.begin(), GlobalCaches::daaGbiFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalCaches::daaGbiFilesCache)
    : files;
            
    if (!list || (list && GlobalState::needSortingAfterflno)) {
        if (need2Sort) {
            sortFilesCaseInsensitive(files);
            if (fileType == "bin" || fileType == "img") {
                std::lock_guard<std::mutex> lock(GlobalCaches::binImgCacheMutex);
                sortFilesCaseInsensitive(GlobalCaches::binImgFilesCache);
            } else if (fileType == "mdf") {
                std::lock_guard<std::mutex> lock(GlobalCaches::mdfMdsCacheMutex);
                sortFilesCaseInsensitive(GlobalCaches::mdfMdsFilesCache);
            } else if (fileType == "nrg") {
                std::lock_guard<std::mutex> lock(GlobalCaches::nrgCacheMutex);
                sortFilesCaseInsensitive(GlobalCaches::nrgFilesCache);
            } else if (fileType == "chd") {
                std::lock_guard<std::mutex> lock(GlobalCaches::chdCacheMutex);
                sortFilesCaseInsensitive(GlobalCaches::chdFilesCache);
            } else if (fileType == "daa") {
                std::lock_guard<std::mutex> lock(GlobalCaches::daaGbiCacheMutex);
                sortFilesCaseInsensitive(GlobalCaches::daaGbiFilesCache);
            }
        }
        GlobalState::needSortingAfterflno = false;
        need2Sort = false;
    }
    
    printList(files, "IMAGE_FILES", "convert2iso", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
}
