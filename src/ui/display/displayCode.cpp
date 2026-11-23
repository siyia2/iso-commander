// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
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
#include "isocmd/core/caches.h"
#include "isocmd/database/databaseOps.h"
#include "isocmd/ui/syncOutput.h"
#include "isocmd/filter/filter.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/core/globalMutexes.h"
#include "isocmd/ui/stringListView.h"
#include "isocmd/ui/pausePrompts.h"
#include "isocmd/core/sharedRefreshState.h"
#include "isocmd/util/sort.h"
#include "isocmd/core/lists.h"
#include "isocmd/core/uiState.h"
#include "isocmd/ui/themes.h"

// =============================================================================
// External declarations
// =============================================================================

void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst = true, bool showPendingIndices = true);

// Defined in select.cpp. While an ISO select screen is active it owns the staged
// selection as PATHS and projects it onto the display state (source indices,
// merged into ranges) against the list as it is right now. Returns false when
// no select screen is active; the caller then manages its own pending state.
bool syncIsoPendingDisplay(std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool importRunning);

// =============================================================================
// Constants
// =============================================================================

/**
 * @brief Base path where ISO files are mounted.
 */
const std::string MOUNTED_ISO_PATH = "/mnt/ISOs";

// =============================================================================
// ISO database view
// =============================================================================

namespace {

    /**
     * @brief Applies a freshly loaded ISO list to the global state and updates filtering.
     *
     * Captures previous paths when a filter is active so newly discovered entries
     * can be evaluated incrementally while preserving existing filtered results.
     * Resets @p currentPage to @p originalPage when unfiltered.
     *
     * @param freshList     Newly loaded ISO paths retrieved from the database.
     * @param filteredFiles Currently filtered files; updated via @c refreshFilterKeepingResults.
     * @param isFiltered    Whether a filter is currently active.
     * @param currentPage   Current pagination index; reset to @p originalPage when unfiltered.
     * @param originalPage  Target page index when reloading without an active filter.
     */
    void applyReloadedIsoList(std::vector<std::string>& freshList,
                               std::vector<std::string>& filteredFiles,
                               bool& isFiltered,
                               size_t& currentPage,
                               size_t originalPage) {
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

        sortFilesCaseInsensitive(GlobalState::globalIsoFileList);

        // Keep what is already shown; test only newly found ISOs.
        refreshFilterKeepingResults(GlobalState::globalIsoFileList, oldPaths,
                                    filteringStack, filteredFiles, isFiltered);
    }

    /**
     * @brief Synchronizes staged selection indices or clears pending state on reload.
     *
     * Projects staged selection paths onto the current list when an import/select
     * screen is active; otherwise clears pending indices and state on a reload.
     *
     * @param pendingIndices    Indices currently marked for processing.
     * @param hasPendingProcess Whether a process is staged.
     * @param needToReload      Whether a database reload occurred during this cycle.
     * @param state             Shared refresh state used to check active import status.
     */
    void reconcilePendingSelection(std::vector<std::string>& pendingIndices,
                                     bool& hasPendingProcess,
                                     bool needToReload,
                                     const std::shared_ptr<RefreshState>& state) {
        // Staged selection: project the paths onto the final (reloaded, sorted)
        // list. With no select screen active, keep the old behaviour of
        // dropping pending state on a reload.
        const bool pendingTracked = syncIsoPendingDisplay(
            pendingIndices, hasPendingProcess,
            state && state->isImportRunning.load(std::memory_order_acquire));

        if (!pendingTracked && needToReload) {
            pendingIndices.clear();
            hasPendingProcess = false;
        }
    }

    /**
     * @brief Renders an empty-database notice and blocks until the user acknowledges.
     *
     * Clears the scroll buffer, prints the empty-database prompt using the current
     * list color theme, and delegates blocking/repaint handling to
     * @c pressEnterToReturnWithRepaint.
     */
    void handleEmptyIsoDatabase() {
        auto render = [] {
            const PrintListTheme c = getListColors();
            SyncOutput sync;
            clearScrollBuffer();
            std::cout << c.num << "\nISO database is empty. Choose 'ImportISO' from the Main Menu."
                      << c.dir << "\n";
        };

        pressEnterToReturnWithRepaint(render);
    }

} // namespace

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
 * @param pendingIndices     Indices currently marked for processing. While an
 *                           ISO select screen is active they are re-projected
 *                           from the staged paths onto the final list just
 *                           before printing (see @c syncIsoPendingDisplay);
 *                           otherwise they are cleared on a full reload.
 * @param hasPendingProcess  Whether a process is staged; maintained like
 *                           @p pendingIndices.
 * @param currentPage        Current page index for pagination; may be reset
 *                           to @p originalPage on a reload when not filtered.
 * @param originalPage       Page index to restore to on an unfiltered reload.
 * @param state              Shared state forwarded to @c printList.
 * @return @c true if the global ISO list is non-empty after the refresh,
 *         @c false if it is empty (a message is shown and the call blocks
 *         until acknowledged).
 */
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles,
                     bool& isFiltered,
                     const std::string& listSubType,
                     bool& umountMvRmBreak,
                     std::vector<std::string>& pendingIndices,
                     bool& hasPendingProcess,
                     size_t& currentPage,
                     size_t& originalPage,
                     std::shared_ptr<RefreshState> state) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const bool needToReload = GlobalState::isoListDirty.exchange(false);
    std::vector<std::string> freshList;

    if (needToReload) {
        loadFromDatabase(freshList);
    }

    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);

        if (needToReload) {
            applyReloadedIsoList(freshList, filteredFiles, isFiltered, currentPage, originalPage);
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

        reconcilePendingSelection(pendingIndices, hasPendingProcess, needToReload, state);

        // Use either the recently refreshed filteredFiles or the global master list.
        printList(isFiltered ? filteredFiles : GlobalState::globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, currentPage, state, true, true);

        isEmpty = GlobalState::globalIsoFileList.empty();
    }

    if (isEmpty) {
        handleEmptyIsoDatabase();
        return false;
    }

    return true;
}

// =============================================================================
// Mounted ISOs view
// =============================================================================

namespace {

    /**
     * @brief Scans and case-insensitively sorts subdirectories under @ref MOUNTED_ISO_PATH.
     *
     * Stops early if directory iteration encounters a filesystem error, matching
     * @c std::filesystem::directory_iterator error-code handling.
     *
     * @return Sorted list of mounted ISO directory paths.
     */
    std::vector<std::string> scanAndSortMountedIsoDirs() {
        std::vector<std::string> dirs;
        std::error_code ec;

        for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_directory()) {
                dirs.push_back(entry.path().string());
            }
        }

        sortFilesCaseInsensitive(dirs);
        return dirs;
    }

    /**
     * @brief Refreshes the cached mounted ISO list when directory metadata changes.
     *
     * Compares @c st_mtime and @c st_nlink against the previous @c stat snapshot.
     * If @ref MOUNTED_ISO_PATH is missing or inaccessible, resets the cache and
     * reports no directory list change.
     *
     * @param[in,out] lastDirStat    Cached filesystem metadata for @ref MOUNTED_ISO_PATH.
     * @param[in,out] lastSortedDirs Cached sorted directory paths.
     * @param[in,out] lastIsoCount   Cached count of entries in @p lastSortedDirs.
     * @return @c true if @p lastSortedDirs was updated with new contents; @c false otherwise.
     */
    bool refreshMountedIsoCache(struct stat& lastDirStat,
                                std::vector<std::string>& lastSortedDirs,
                                size_t& lastIsoCount) {
        struct stat currentDirStat{};
        if (stat(MOUNTED_ISO_PATH.c_str(), &currentDirStat) != 0) {
            lastSortedDirs.clear();
            lastIsoCount = 0;
            lastDirStat = {};
            return false;
        }

        const bool metaChanged = (currentDirStat.st_mtime != lastDirStat.st_mtime ||
                                  currentDirStat.st_nlink != lastDirStat.st_nlink);
        if (!metaChanged) {
            return false;
        }

        std::vector<std::string> newIsoDirs = scanAndSortMountedIsoDirs();
        lastDirStat = currentDirStat;

        if (newIsoDirs.size() != lastIsoCount || newIsoDirs != lastSortedDirs) {
            lastSortedDirs = std::move(newIsoDirs);
            lastIsoCount = lastSortedDirs.size();
            return true;
        }

        return false;
    }

    /**
     * @brief Displays the empty-mount warning, waits for acknowledgment, and clears caches.
     *
     * Resolves the warning color once using the active theme and @c globalTheme,
     * blocks on @ref pressEnterToReturnWithRepaint, and releases static and parse
     * caches after the prompt returns.
     *
     * @param[in,out] lastSortedDirs Static cached directory list to swap-clear.
     * @param[in,out] lastIsoCount   Static cached directory count to reset to zero.
     */
    void handleEmptyMountedIsos(std::vector<std::string>& lastSortedDirs, size_t& lastIsoCount) {
        const MainTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");

        const std::string_view warnColor = isOriginal ? UI::Palette::Yellow : theme->warning;
        const std::string_view reset = UI::Palette::BoldReset;

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
    }

    /**
     * @brief Updates filter state and synchronizes pending selection indices after a refresh.
     *
     * @param[in]     isoDirs           Current list of mounted ISO directories.
     * @param[in,out] filteredFiles     Filtered entries; cleared when filter state resets.
     * @param[in,out] isFiltered        Active filter flag; reset to @c false on reset.
     * @param[in]     umountMvRmBreak   Forces a filter reset when @c true.
     * @param[in,out] pendingIndices    Staged selection indices updated via @c syncIsoPendingDisplay.
     * @param[in,out] hasPendingProcess Staged-process state flag updated alongside @p pendingIndices.
     * @param[in]     currentPage       Active page index saved on filter reset.
     * @param[out]    originalPage      Receives @p currentPage when filter state is reset.
     * @param[in]     dirsChanged       Whether the mounted directory list changed this pass.
     */
    void syncFilterAndPendingSelection(const std::vector<std::string>& isoDirs,
                                       std::vector<std::string>& filteredFiles,
                                       bool& isFiltered,
                                       bool umountMvRmBreak,
                                       std::vector<std::string>& pendingIndices,
                                       bool& hasPendingProcess,
                                       size_t currentPage,
                                       size_t& originalPage,
                                       bool dirsChanged) {
        if (filteredFiles.size() == isoDirs.size() || umountMvRmBreak) {
            originalPage = currentPage;
            filteringStack.clear();
            isFiltered = false;
            filteredFiles.clear();
        }

        const bool pendingTracked = syncIsoPendingDisplay(pendingIndices, hasPendingProcess, false);

        if (!pendingTracked && dirsChanged) {
            pendingIndices.clear();
            hasPendingProcess = false;
        }
    }

} // namespace

/**
 * @brief Scans, filters, and renders currently mounted ISO directories.
 *
 * Uses stat-based caching to detect changes in @ref MOUNTED_ISO_PATH: a full
 * re-scan and re-sort runs only when the directory's modification time
 * (@c st_mtime) or link count (@c st_nlink) changes. If the resulting
 * directory list differs from the cached one, the static cache is updated. The
 * pending selection is then handled as in @c loadAndDisplayIso: re-projected
 * from the staged paths while an ISO select screen is active, otherwise reset.
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
 * @param[out]    isoDirs           Current mount paths.
 * @param[in,out] filteredFiles     Files after filtering; cleared when the
 *                                  filter is reset (see @p umountMvRmBreak and the
 *                                  size-equality branch).
 * @param[in,out] isFiltered        Filtering mode toggle; set to @c false
 *                                  when the filter stack is cleared.
 * @param[in]     umountMvRmBreak   When @c true, resets the filter stack and
 *                                  filter state so the full list is shown.
 * @param[in,out] pendingIndices    Items marked for batch operations; see
 *                                  @c syncIsoPendingDisplay.
 * @param[in,out] hasPendingProcess Staged-process flag; maintained like
 *                                  @p pendingIndices.
 * @param[in,out] currentPage       Current pagination index.
 * @param[out]    originalPage      Page index backup, written only when the
 *                                  filter is reset (i.e. when
 *                                  @c filteredFiles.size() == isoDirs.size() or
 *                                  @p umountMvRmBreak is set).
 * @param[in]     state             Shared state forwarded to @c printList.
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

    const bool dirsChanged = refreshMountedIsoCache(lastDirStat, lastSortedDirs, lastIsoCount);
    isoDirs = lastSortedDirs;

    if (isoDirs.empty()) {
        handleEmptyMountedIsos(lastSortedDirs, lastIsoCount);
        return false;
    }

    syncFilterAndPendingSelection(isoDirs, filteredFiles, isFiltered, umountMvRmBreak,
                                  pendingIndices, hasPendingProcess, currentPage,
                                  originalPage, dirsChanged);

    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "", pendingIndices,
              hasPendingProcess, currentPage, state, true, true);
    return true;
}

// =============================================================================
// Cached image files view
// =============================================================================

namespace {

    /**
     * @brief Binds a format-specific file cache to its synchronization mutex.
     */
    struct CacheTarget {
        std::vector<std::string>* cache;
        std::mutex* mutex;
    };

} // namespace

/**
 * @brief Resolves the cache and mutex pair for a given disc image extension.
 *
 * Maps supported format identifiers to their corresponding global cache and
 * mutex instances, returning null pointers for unmapped types such as
 * @c "img" or @c "gbi".
 *
 * @param fileType Disc image extension identifier.
 * @return Target cache and mutex pointers for @p fileType.
 */
CacheTarget resolveCacheTarget(const std::string& fileType) {
    if (fileType == "bin") {
        return {&GlobalState::binImgFilesCache, &GlobalMutexes::binImgCacheMutex};
    }
    if (fileType == "mdf") {
        return {&GlobalState::mdfMdsFilesCache, &GlobalMutexes::mdfMdsCacheMutex};
    }
    if (fileType == "nrg") {
        return {&GlobalState::nrgFilesCache, &GlobalMutexes::nrgCacheMutex};
    }
    if (fileType == "chd") {
        return {&GlobalState::chdFilesCache, &GlobalMutexes::chdCacheMutex};
    }
    if (fileType == "daa") {
        return {&GlobalState::daaGbiFilesCache, &GlobalMutexes::daaGbiCacheMutex};
    }
    return {nullptr, nullptr};
}

/**
 * @brief Replaces the working list from the target cache when it differs.
 *
 * Checks whether the current view is unfiltered and the cache is populated
 * with different contents. When a mismatch is found, copies the cached list
 * into @p files and flags @p need2Sort.
 *
 * @param[in,out] files      Working file list to update from cache.
 * @param[in]     target     Resolved cache and mutex pair.
 * @param[in]     isFiltered When @c true, skips cache restoration.
 * @param[in,out] need2Sort  Set to @c true when @p files is replaced.
 */
void restoreFromCacheIfNeeded(std::vector<std::string>& files, const CacheTarget& target,
                                        bool isFiltered, bool& need2Sort) {
    if (isFiltered || !target.cache || target.cache->empty()) {
        return;
    }

    const bool contentMismatch = (target.cache->size() != files.size()) ||
        !std::equal(target.cache->begin(), target.cache->end(), files.begin());

    if (!contentMismatch) {
        return;
    }

    need2Sort = true;
    files = *target.cache;
}

/**
 * @brief Sorts the working list and its backing cache when requested.
 *
 * Executes case-insensitive sorting on @p files and, if available, on
 * @p target.cache under @p target.mutex when @p need2Sort is set, then
 * resets pending sort flags.
 *
 * @param[in,out] files      Working file list to sort.
 * @param[in]     target     Resolved cache and mutex pair.
 * @param[in,out] need2Sort  Cleared after sort handling completes.
 */
void applyPendingSort(std::vector<std::string>& files, const CacheTarget& target, bool& need2Sort) {
    if (need2Sort) {
        sortFilesCaseInsensitive(files);

        if (target.cache && target.mutex) {
            std::lock_guard<std::mutex> lock(*target.mutex);
            sortFilesCaseInsensitive(*target.cache);
        }
    }

    GlobalState::needSortingAfterflno = false;
    need2Sort = false;
}

/**
 * @brief Prepares and displays cached disc image file lists.
 *
 * When not filtered and the format-specific cache is non-empty and differs
 * from the current view, the working list is replaced from that cache
 * (BIN/IMG, MDF, NRG, CHD, DAA). GBI shares the DAA/GBI cache storage but
 * retains its original behavior of not restoring from that cache here.
 *
 * If @p list is false, or a re-sort is pending (@c needSortingAfterflno), the
 * active list is sorted and the corresponding cache is sorted under its
 * format-specific mutex. The function then delegates to @c printList with
 * @c listType "IMAGE_FILES" and @c listSubType "convert2iso".
 *
 * @param files              [in/out] Working file list; may be replaced
 *                           wholesale from a cache.
 * @param fileType           File extension type: "bin", "img", "mdf", "nrg",
 *                           "chd", "daa", or "gbi".
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
    const CacheTarget target = resolveCacheTarget(fileType);

    restoreFromCacheIfNeeded(files, target, isFiltered, need2Sort);

    if (!list || GlobalState::needSortingAfterflno) {
        applyPendingSort(files, target, need2Sort);
    }

    printList(files, "IMAGE_FILES", "convert2iso", pendingIndices, hasPendingProcess, currentPage, state, true, true);
}
