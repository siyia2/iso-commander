// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"
#include "../themes.h"

/**
 * @brief Loads ISO files from the database and updates the display using atomic pointer swaps.
 * * When the dirty flag is set, this function loads fresh data into a new buffer outside 
 * the critical section, then atomically updates 'globalIsoFilesPtr'. This ensures 
 * thread safety by preventing background threads from accessing a vector currently 
 * being cleared or resized.
 * * @details 
 * - If a filter is active during a reload, it is re-applied via syncFilteringStackForIso.
 * - Handles 'needSortingAfterflno' by cloning and replacing the global list to avoid 
 * race conditions during sorting.
 * - Updates pagination and resets pending operation states upon a full reload.
 *
 * @param filteredFiles    Reference to the vector of currently filtered files.
 * @param isFiltered       Boolean flag indicating if a filter is active.
 * @param listSubType      String representing the sub-category of the list (for UI).
 * @param umountMvRmBreak  Flag to clear filters and force a full refresh.
 * @param pendingIndices   Vector of indices marked for processing (cleared on reload).
 * @param hasPendingProcess Boolean flag indicating if there are staged operations.
 * @param currentPage      Current page index (restored to originalPage on reload).
 * @param originalPage     Backup of the page index before a filter was applied.
 * @param isImportRunning  Atomic flag preventing UI interference during active imports.
 * * @return true if the list contains items, false if the database/cache is empty.
 */
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    
    bool needToReload = isoListDirty.exchange(false);
    
    if (needToReload) {
        // 1. Create a NEW vector and load data into it outside the lock
        auto freshList = std::make_shared<std::vector<std::string>>();
        loadFromDatabase(*freshList);
        sortFilesCaseInsensitive(*freshList);

        std::lock_guard<std::mutex> lock(updateListMutex);
        
        // 2. Atomic pointer swap: replace the global shared_ptr
        globalIsoFilesPtr = freshList;

        currentPage = originalPage;
        pendingIndices.clear();
        hasPendingProcess = false;
        
        // 3. Sync filtering using the new data
        syncFilteringStackForIso(*globalIsoFilesPtr, filteringStack, filteredFiles, isFiltered);
    }

    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(updateListMutex);

        if (needSortingAfterflno) {
            // If sorting is needed, we must clone and replace to keep it thread-safe
            auto sortedList = std::make_shared<std::vector<std::string>>(*globalIsoFilesPtr);
            sortFilesCaseInsensitive(*sortedList);
            globalIsoFilesPtr = sortedList;
            needSortingAfterflno = false;
        }

        if (umountMvRmBreak) {
            filteringStack.clear();
            filteredFiles.clear();
            isFiltered = false;
        }

        clearScrollBuffer();
        
        // Use the dereferenced pointer for printing
        printList(isFiltered ? filteredFiles : *globalIsoFilesPtr, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
        
        isEmpty = globalIsoFilesPtr->empty();
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
    : (!isFiltered && !daaGbiFilesCache.empty() && fileType == "daa" &&               // <-- DAA branch
       (daaGbiFilesCache.size() != files.size() || !std::equal(daaGbiFilesCache.begin(), daaGbiFilesCache.end(), files.begin())))
        ? (need2Sort = true, daaGbiFilesCache)
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
            } else if (fileType == "daa") {
                std::lock_guard<std::mutex> lock(daaGbiCacheMutex);
                sortFilesCaseInsensitive(daaGbiFilesCache);
            }
        }
        needSortingAfterflno = false;
        need2Sort = false;
    }
    
    printList(files, "IMAGE_FILES", "convert2iso", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);
}
