// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/stat.h>

// Project Headers
#include "../caches.h"
#include "../databaseOps.h"
#include "../rendering.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../globalMutexes.h"
#include "../rendering.h"
#include "../stringListView.h"
#include "../pausePrompts.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"

void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst = true, bool showPendingIndices = true);

/**
 * @brief Loads ISO files from the database and updates the display.
 *
 * Synchronizes the global ISO list with the database when the dirty flag is set.
 * If a filter is active during a reload, the entries already shown are kept
 * (if they still exist), and only newly discovered ISOs are tested against the
 * filter stack, each level using the text mode (filenames-only or full path)
 * it was committed with. Existing results are not re-evaluated. See
 * @c refreshFilterKeepingResults.
 *
 * @note If no entries remain after the refresh (for example, every filtered
 * ISO was removed from disk), the filter stack is cleared and the view resets
 * to the full list to prevent a blank UI.
 *
 * @param filteredFiles      Reference to the vector of currently filtered files.
 * @param isFiltered         Boolean flag indicating if a filter is active.
 * @param listSubType        String representing the sub-category of the list.
 * @param umountMvRmBreak    Flag to force a break/refresh in the UI state.
 * @param pendingIndices     Vector of indices currently marked for processing.
 * @param hasPendingProcess  Boolean flag indicating if there are active background tasks.
 * @param currentPage        Current page index for pagination.
 * @param originalPage       Backup of the page index before filtering.
 * @param state              Shared state with import flag and CV for event coordination.
 * @return true if the list contains items, false if the cache is empty.
 */
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
size_t& currentPage, size_t& originalPage, std::shared_ptr<RefreshState> state) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    bool needToReload = GlobalState::isoListDirty.exchange(false);
    std::vector<std::string> freshList;

    if (needToReload) {
        loadFromDatabase(freshList);
    }

    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);

        if (needToReload) {
            // Remember which paths existed before the reload, so the filter
            // refresh can tell new ISOs from old ones. Only needed while a
            // filter is active.
            std::unordered_set<std::string> oldPaths;
            if (isFiltered) {
                oldPaths.reserve(GlobalState::globalIsoFileList.size());
                oldPaths.insert(GlobalState::globalIsoFileList.begin(),
                                GlobalState::globalIsoFileList.end());
            }

            GlobalState::globalIsoFileList = std::move(freshList);

            // Only reset to originalPage if we are NOT actively filtered
            if (!isFiltered) {
                currentPage = originalPage;
            }
            pendingIndices.clear();
            hasPendingProcess = false;
            sortFilesCaseInsensitive(GlobalState::globalIsoFileList);

            // Keep what is already shown; test only newly found ISOs.
            refreshFilterKeepingResults(GlobalState::globalIsoFileList, oldPaths,
                                        filteringStack, filteredFiles, isFiltered);
        }

        if (GlobalState::needSortingAfterflno) {
            sortFilesCaseInsensitive(GlobalState::globalIsoFileList);
            GlobalState::needSortingAfterflno = false;
        }

        if (umountMvRmBreak) {
            filteringStack.clear();
            filteredFiles.clear();
            isFiltered = false;
        }

        // Use either the recently refreshed filteredFiles or the global master list
        printList(isFiltered ? filteredFiles : GlobalState::globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, currentPage, state, true, true);

        isEmpty = GlobalState::globalIsoFileList.empty();
    }

    if (isEmpty) {
        auto render = [&] {
            const PrintListTheme c = getListColors();
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << c.num << "\nISO database is empty. Choose 'ImportISO' from the Main Menu."
                      << c.dir << "\n";
        };

        pressEnterToReturnWithRepaint(render);
        return false;
    }

    return true;
}

/**
 * @brief Base path where ISO files are mounted.
 */
const std::string MOUNTED_ISO_PATH = "/mnt/ISOs";

/**
 * @brief Scans, filters, and renders currently mounted ISO directories.
 *
 * Uses stat-based caching to detect changes in MOUNTED_ISO_PATH. A full re-scan
 * and re-sort occur only if the directory's modification time or link count changes.
 *
 * If changes are detected and the resulting directory list differs from the cache:
 * - Updates the static cache.
 * - Resets pending process states and indices.
 *
 * If no ISOs are found, it clears internal caches and displays a warning.
 * Otherwise, it manages pagination state and invokes the UI rendering (printList).
 *
 * @note Temporarily ignores SIGINT and disables Ctrl+D during execution.
 *
 * @param isoDirs            [out] Vector populated with current mount paths.
 * @param filteredFiles      [in/out] List of files post-filtering.
 * @param isFiltered         [in/out] Boolean toggle for filtering mode.
 * @param umountMvRmBreak    [in] UI flag to force a refresh/reset of the view.
 * @param pendingIndices     [in/out] Tracks items marked for batch operations.
 * @param hasPendingProcess  [in/out] Flag indicating active background tasks.
 * @param currentPage        [in/out] Current UI pagination index.
 * @param originalPage       [out] Backup of page index when filtering is reset.
 * @param state              [in] Shared state with import flag and CV for event coordination.
 * @return true if ISOs were found and displayed; false if the directory is empty.
 */
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles,
                               bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices,
                               bool& hasPendingProcess, size_t& currentPage,
                               size_t& originalPage, std::shared_ptr<RefreshState> state) {

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
        // Message + wait. The warning color is resolved once (it depends on
        // globalTheme at call time); the repaint lambda only reprints it. The
        // cache cleanup side effects run once, after the wait.
        const MainTheme* theme = getActiveTheme();
        const bool isOriginal  = (globalTheme == "original");

        const std::string_view warnColor = isOriginal ? UI::Palette::Yellow : theme->warning;
        const std::string_view reset     = UI::Palette::BoldReset;

        auto render = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            std::cerr << "\n" << warnColor
                      << "No paths matching the '/mnt/ISOs/{isoDirName}' pattern found."
                      << reset << "\n";
        };

        pressEnterToReturnWithRepaint(render);

        std::vector<std::string>().swap(lastSortedDirs);
        lastIsoCount = 0;
        GlobalCaches::cachedParsesForUmount.clear();

        return false;
    }

    if (filteredFiles.size() == isoDirs.size() || umountMvRmBreak) {
        originalPage = currentPage;
        filteringStack.clear();
        isFiltered = false;
        filteredFiles.clear();
    }

    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "", pendingIndices, hasPendingProcess, currentPage, state, true, true);
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
 * @param files            Current working file list (may be replaced by cache)
 * @param fileType         File extension type ("bin", "img", "mdf", "nrg", "chd", "daa", "gbi")
 * @param need2Sort        Flag indicating whether sorting is required
 * @param isFiltered       Filter state affecting cache restoration
 * @param list             Controls whether list refresh behavior is executed
 * @param pendingIndices   Files queued for processing or convert2iso
 * @param hasPendingProcess Global processing state flag
 * @param currentPage      Pagination state for UI display
 * @param state            Shared state with import flag and CV for event coordination
 */
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,
                              std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::shared_ptr<RefreshState> state) {

    // Restore from the appropriate cache when not filtered and the cache is valid
    files =
    (!isFiltered && !GlobalState::binImgFilesCache.empty() && (fileType == "bin" || fileType == "img") &&
     (GlobalState::binImgFilesCache.size() != files.size() || !std::equal(GlobalState::binImgFilesCache.begin(), GlobalState::binImgFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalState::binImgFilesCache)
    : (!isFiltered && !GlobalState::mdfMdsFilesCache.empty() && fileType == "mdf" &&
       (GlobalState::mdfMdsFilesCache.size() != files.size() || !std::equal(GlobalState::mdfMdsFilesCache.begin(), GlobalState::mdfMdsFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalState::mdfMdsFilesCache)
    : (!isFiltered && !GlobalState::nrgFilesCache.empty() && fileType == "nrg" &&
       (GlobalState::nrgFilesCache.size() != files.size() || !std::equal(GlobalState::nrgFilesCache.begin(), GlobalState::nrgFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalState::nrgFilesCache)
    : (!isFiltered && !GlobalState::chdFilesCache.empty() && fileType == "chd" &&
       (GlobalState::chdFilesCache.size() != files.size() || !std::equal(GlobalState::chdFilesCache.begin(), GlobalState::chdFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalState::chdFilesCache)
    : (!isFiltered && !GlobalState::daaGbiFilesCache.empty() && fileType == "daa" &&               // <-- DAA branch
       (GlobalState::daaGbiFilesCache.size() != files.size() || !std::equal(GlobalState::daaGbiFilesCache.begin(), GlobalState::daaGbiFilesCache.end(), files.begin())))
        ? (need2Sort = true, GlobalState::daaGbiFilesCache)
    : files;

    if (!list || (list && GlobalState::needSortingAfterflno)) {
        if (need2Sort) {
            sortFilesCaseInsensitive(files);
            if (fileType == "bin" || fileType == "img") {
                std::lock_guard<std::mutex> lock(GlobalMutexes::binImgCacheMutex);
                sortFilesCaseInsensitive(GlobalState::binImgFilesCache);
            } else if (fileType == "mdf") {
                std::lock_guard<std::mutex> lock(GlobalMutexes::mdfMdsCacheMutex);
                sortFilesCaseInsensitive(GlobalState::mdfMdsFilesCache);
            } else if (fileType == "nrg") {
                std::lock_guard<std::mutex> lock(GlobalMutexes::nrgCacheMutex);
                sortFilesCaseInsensitive(GlobalState::nrgFilesCache);
            } else if (fileType == "chd") {
                std::lock_guard<std::mutex> lock(GlobalMutexes::chdCacheMutex);
                sortFilesCaseInsensitive(GlobalState::chdFilesCache);
            } else if (fileType == "daa") {
                std::lock_guard<std::mutex> lock(GlobalMutexes::daaGbiCacheMutex);
                sortFilesCaseInsensitive(GlobalState::daaGbiFilesCache);
            }
        }
        GlobalState::needSortingAfterflno = false;
        need2Sort = false;
    }

    printList(files, "IMAGE_FILES", "convert2iso", pendingIndices, hasPendingProcess, currentPage, state, true, true);
}
