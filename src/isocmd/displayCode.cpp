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
#include <unordered_set>
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
#include "../stringListView.h"
#include "../pausePrompts.h"
#include "../sort.h"
#include "../state.h"
#include "../themes.h"

/**
 * @file
 * @brief "Load from a source, then render" entry points.
 *
 * Each function here refreshes one of the list views (ISO database, mounted
 * ISOs, cached image files) and hands the result to @c printList. They share
 * the same blocking policy: SIGINT is ignored and Ctrl+D is disabled for the
 * duration of the call (the SIGINT handler is not restored on return).
 */

void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst = true, bool showPendingIndices = true);

/**
 * @brief Loads ISO files from the database and updates the display.
 *
 * Synchronizes the global ISO list with the database when the dirty flag is
 * set. If a filter is active during a reload, the entries already shown are
 * kept (if they still exist), and only newly discovered ISOs are tested
 * against the filter stack, each level using the text mode (filenames-only or
 * full path) it was committed with. Existing results are not re-evaluated.
 * See @c refreshFilterKeepingResults.
 *
 * @note When no entries remain after the refresh (for example, every filtered
 *       ISO was removed from disk), the filter stack is cleared and the view
 *       resets to the full list to prevent a blank UI. In that case the
 *       function also blocks on @ref pressEnterToReturnWithRepaint with an
 *       "ISO database is empty" message before returning @c false.
 *
 * @note SIGINT is set to @c SIG_IGN and Ctrl+D is disabled for the duration
 *       of the call; the SIGINT handler is not restored on return.
 *
 * @param filteredFiles      Currently filtered files; may be cleared when the
 *                           filter is reset.
 * @param isFiltered         Whether a filter is active. Set to @c false when
 *                           the filter stack is cleared.
 * @param listSubType        Sub-category string passed through to @c printList.
 * @param umountMvRmBreak    When @c true, clears the filter stack,
 *                           @p filteredFiles and @p isFiltered, so the full
 *                           list is shown again.
 * @param pendingIndices     Indices currently marked for processing; cleared
 *                           on a full reload.
 * @param hasPendingProcess  Whether background tasks are active; cleared on a
 *                           full reload.
 * @param currentPage        Current page index for pagination; may be reset
 *                           to @p originalPage on a reload when not filtered.
 * @param originalPage       Page index to restore to on an unfiltered reload.
 * @param state              Shared state forwarded to @c printList.
 * @return @c true if the global ISO list is non-empty after the refresh,
 *         @c false if it is empty (a message is shown and the call blocks
 *         until acknowledged).
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
 * Uses stat-based caching to detect changes in @ref MOUNTED_ISO_PATH: a full
 * re-scan and re-sort runs only when the directory's modification time
 * (@c st_mtime) or link count (@c st_nlink) changes. If the resulting
 * directory list differs from the cached one, the static cache is updated and
 * pending process state is reset.
 *
 * If the mount directory is missing or inaccessible, it is treated as empty.
 * If no ISO directories are found, the function shows a warning and blocks on
 * @ref pressEnterToReturnWithRepaint, then clears the static caches and
 * returns @c false. Otherwise it manages pagination state and delegates to
 * @c printList.
 *
 * @note SIGINT is set to @c SIG_IGN and Ctrl+D is disabled for the duration
 *       of the call; the SIGINT handler is not restored on return.
 *
 * @param isoDirs            [out] Current mount paths.
 * @param filteredFiles      [in/out] Files after filtering; cleared when the
 *                           filter is reset (see @p umountMvRmBreak and the
 *                           size-equality branch below).
 * @param isFiltered         [in/out] Filtering mode toggle; set to @c false
 *                           when the filter stack is cleared.
 * @param umountMvRmBreak    [in] When @c true, resets the filter stack and
 *                           filter state so the full list is shown.
 * @param pendingIndices     [in/out] Items marked for batch operations;
 *                           cleared when the directory list changes.
 * @param hasPendingProcess  [in/out] Background-task flag; cleared when the
 *                           directory list changes.
 * @param currentPage        [in/out] Current pagination index.
 * @param originalPage       [out] Page index backup, written only when the
 *                           filter is reset (i.e. when
 *                           @c filteredFiles.size() == isoDirs.size() or
 *                           @p umountMvRmBreak is set).
 * @param state              [in] Shared state forwarded to @c printList.
 * @return @c true if ISO directories were found and displayed; @c false if
 *         none were found (a warning is shown and the call blocks until
 *         acknowledged).
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
 * When not filtered and the format-specific cache is non-empty and differs
 * from the current view, the working list is replaced from that cache
 * (BIN/IMG, MDF, NRG, CHD, DAA/GBI) and a re-sort is requested.
 *
 * If @p list is false, or a re-sort is pending (@c needSortingAfterflno), the
 * active list is sorted and the corresponding cache is sorted under its
 * format-specific mutex. The function then delegates rendering and
 * interaction to @c printList with @c listType "IMAGE_FILES" and
 * @c listSubType "convert2iso".
 *
 * @note @c fileType "daa" and "gbi" share @c daaGbiFilesCache; the sort
 *       branch matches @c "daa" only.
 *
 * @param files              [in/out] Working file list; may be replaced
 *                           wholesale from a cache.
 * @param fileType           File extension type: "bin", "img", "mdf", "nrg",
 *                           "chd", or "daa" (the "daa" branch also backs
 *                           "gbi" via @c daaGbiFilesCache).
 * @param need2Sort          [in/out] Set to @c true when a cache replacement
 *                           requires a re-sort; cleared after sorting.
 * @param isFiltered         [in] When @c true, cache restoration is skipped.
 * @param list               [in] When @c false, or when a re-sort is pending,
 *                           the sort block runs; otherwise it is skipped.
 * @param pendingIndices     [in/out] Files queued for processing or
 *                           convert2iso.
 * @param hasPendingProcess  [in/out] Global processing state flag.
 * @param currentPage        [in/out] Pagination state for UI display.
 * @param state              [in] Shared state forwarded to @c printList.
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
    : (!isFiltered && !GlobalState::daaGbiFilesCache.empty() && fileType == "daa" &&
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
