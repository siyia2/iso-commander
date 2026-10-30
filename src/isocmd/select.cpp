// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../databaseOps.h"
#include "../filtering.h"
#include "../inputHandling.h"
#include "../globalMutexes.h"
#include "../rendering.h"
#include "../readline.h"
#include "../select.h"
#include "../sharedRefreshState.h"
#include "../state.h"
#include "../themes.h"
#include "../verbose.h"

/**
 * @brief Routes validated user input to the correct filesystem or mount
 * operation.
 *
 * Picks the active file list from the UI state (filtered vs. global) and the
 * operation, then dispatches to the matching handler.
 *
 * - **List selection:** @p filteredFiles when @p isFiltered; otherwise
 *   @c globalIsoFileList (or @p isoDirs for unmounts).
 * - **UI locking:** sets @p isAtISOList to @c false so no concurrent refresh
 *   runs during a blocking operation.
 * - **Destructive safety:** sets @p umountMvRmBreak so the view is refreshed
 *   after move, remove, or unmount.
 *
 * @param inputString      Raw user string (usually indices or paths).
 * @param[in,out] isFiltered Updated if the operation invalidates the current
 *                           filter.
 * @param filteredFiles    The currently visible subset of files.
 * @param isoDirs          Currently active mount points.
 * @param operation        Operation identifier for logging and UI feedback.
 * @param umountMvRmBreak  Forces a view refresh after destructive actions.
 * @param filterHistory    Clears/updates the readline search history.
 */
void processOperationForSelectedIsoFiles(const std::string& inputString,
                                         bool& isFiltered,
                                         const std::vector<std::string>& filteredFiles,
										 std::vector<std::string>& isoDirs,
										 const std::string& operation,
										 bool& umountMvRmBreak,
										 bool& filterHistory) {

    clearScrollBuffer();
    bool verbose = false;
    bool isUnmount = false;

    if (operation == "mount" || operation == "umount") {
        if (operation == "umount") isUnmount = true;
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles :
                                                    (isUnmount ? isoDirs : GlobalState::globalIsoFileList);

        if (operation == "umount") {
            umountMvRmBreak = true;
        }

        processInputForMountOrUmount(inputString, activeList, umountMvRmBreak, verbose, isUnmount);
    } else if (operation == "write2usb") {
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalState::globalIsoFileList;
        writeToUsb(inputString, activeList);
    } else {
        const std::vector<std::string>& activeList = isFiltered ? filteredFiles : GlobalState::globalIsoFileList;
        processInputForCpMvRm(inputString, activeList, operation, umountMvRmBreak, filterHistory, verbose);
    }

    handleSelectIsoFilesResults(operation, verbose, isFiltered, umountMvRmBreak);
}

/**
 * @brief Parses and queues semicolon-delimited indices for batch processing.
 *
 * Separates "induction" input (e.g. `1 2 3;`) from direct commands so the user
 * can stage multiple indices into a pending queue without executing them
 * immediately.
 *
 * - **Trigger:** requires a semicolon (@c ';').
 * - **Sanity check:** aborts if a forward slash (@c '/') is present, assuming
 *   the input is a direct path rather than an index list.
 * - **Deduplication:** an internal @c std::unordered_set ensures only unique
 *   indices are appended to @p pendingIndices.
 *
 * @param inputString           The raw line from @c readline.
 * @param[out] pendingIndices   Vector where unique staged tokens are appended.
 * @param[out] hasPendingProcess Set to true if at least one new index was queued.
 * @param[out] needsClrScrn     Signals the UI to refresh the updated queue.
 * @return true if the input was valid induction syntax and was queued; false
 *         if it should be treated as a standard command.
 */
bool handlePendingInduction(const std::string& inputString, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& needsClrScrn) {
    if (inputString.find(';') == std::string::npos || inputString.find('/') != std::string::npos) {
        return false;
    }

    std::string indicesInput = inputString.substr(0, inputString.find(';'));

    while (!indicesInput.empty() && std::isspace(indicesInput.back())) {
        indicesInput.pop_back();
    }

    if (!indicesInput.empty()) {
        std::istringstream iss(indicesInput);
        std::string token;
        std::unordered_set<std::string> uniqueTokens;

        for (const auto& index : pendingIndices) {
            uniqueTokens.insert(index);
        }

        std::vector<std::string> newIndices;

        while (iss >> token) {
            if (uniqueTokens.find(token) == uniqueTokens.end()) {
                newIndices.push_back(token);
                uniqueTokens.insert(token);
            }
        }

        pendingIndices.insert(pendingIndices.end(), newIndices.begin(), newIndices.end());

        if (!pendingIndices.empty()) {
            hasPendingProcess = true;
            needsClrScrn = true;
            return true;
        }
    }
    return false;
}


/**
 * @brief Executes a batch operation by sanitizing and dispatching pending
 * indices.
 *
 * Sanitizes staged index tokens, serializes valid leading numeric/range
 * portions into a space-delimited string, and passes the result to the
 * supplied callback.
 *
 * @param inputString     User command; must be exactly "P".
 * @param pendingIndices  Staged index tokens; not modified.
 * @param hasPendingProcess Whether a pending queue exists.
 * @param processBatch    Callback that executes the sanitized indices.
 * @return true if the batch was dispatched; false otherwise.
 */
template <typename ProcessBatch>
bool handlePendingProcess(const std::string& inputString,
                          const std::vector<std::string>& pendingIndices,
                          bool hasPendingProcess,
                          ProcessBatch&& processBatch) {
    if (inputString != "P" || !hasPendingProcess || pendingIndices.empty()) {
        return false;
    }

    std::string combinedIndices;

    for (const std::string& token : pendingIndices) {
        if (token.empty() ||
            !std::isdigit(static_cast<unsigned char>(token[0]))) {
            continue;
        }

        size_t i = 0;

        while (i < token.size() &&
               std::isdigit(static_cast<unsigned char>(token[i]))) {
            ++i;
        }

        while (i + 1 < token.size() &&
               token[i] == '-' &&
               std::isdigit(static_cast<unsigned char>(token[i + 1]))) {
            ++i;

            while (i < token.size() &&
                   std::isdigit(static_cast<unsigned char>(token[i]))) {
                ++i;
            }
        }

        if (!combinedIndices.empty()) {
            combinedIndices += ' ';
        }

        combinedIndices += token.substr(0, i);
    }

    if (combinedIndices.empty()) {
        return false;
    }

    std::forward<ProcessBatch>(processBatch)(combinedIndices);
    return true;
}


/**
 * @brief Background watcher that requests an ISO-list redraw after an import
 * completes.
 *
 * Waits on a condition variable until the import thread signals completion,
 * then publishes a pending refresh request instead of touching the terminal or
 * Readline. The actual redraw happens later, on the main thread, inside
 * Readline's event hook @c checkPendingRefresh — the only thread permitted to
 * call Readline functions or write to the terminal.
 *
 * - **Thread safety:** holds the shared @c RefreshState alive via
 *   @c std::shared_ptr; the handoff to @c GlobalState::g_pendingRefreshState is
 *   itself guarded by a mutex, since a bare @c shared_ptr is not safe to read
 *   and write concurrently.
 * - **Event-driven:** blocks on @c RefreshState::importCV rather than polling.
 * - **No direct Readline access:** only stores the target state and publishes
 *   @c PendingRefreshKind::IsoList; the redraw (including @c rl_on_new_line()
 *   and @c rl_redisplay()) happens later in @c checkPendingRefresh.
 * - **Indicator reset:** clears @c GlobalState::g_filteringIndicator so any
 *   active filter/sync status stops being displayed once the import/refresh
 *   flow finishes.
 * - **Auto-termination:** runs once and exits; not a loop.
 *
 * @param isAtISOList  Atomic flag; a refresh is only requested if the user is
 *                     in the list view.
 * @param state        Shared state providing @c isImportRunning (the CV
 *                     predicate), @c importMutex / @c importCV, the display
 *                     context (@c filteredFiles, @c isFiltered,
 *                     @c listSubtype), the list state (@c pendingIndices,
 *                     @c hasPendingProcess, @c umountMvRmBreak) and pagination
 *                     (@c currentPage, @c originalPage). Retained via
 *                     @c GlobalState::g_pendingRefreshState until consumed by
 *                     @c checkPendingRefresh on the main thread.
 */
void refreshListAfterAutoUpdate(std::atomic<bool>& isAtISOList,
                                std::shared_ptr<RefreshState> state) {
    {
        std::unique_lock<std::mutex> lock(state->importMutex);
        state->importCV.wait(lock, [&] {
            return !state->isImportRunning.load(std::memory_order_acquire);
        });
    }
    if (isAtISOList.load()) {
    {
        std::lock_guard<std::mutex> lk(GlobalMutexes::readLineMutex);
        GlobalState::g_pendingRefreshState = state;                        // set state first
    }
        GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::IsoList); // then publish
    }
    GlobalState::g_filteringIndicator.store(false);
}

/**
 * @brief Interactive TUI for batch ISO operations (mount, umount, cp, mv, rm,
 * write2usb).
 *
 * Paginated, multi-layer filterable interface for selecting ISO files and
 * executing system operations. Manages terminal state, background imports, and
 * operation dispatch inside a readline-driven event loop.
 *
 * - **Dynamic context:** toggles between @c GlobalState::globalIsoFileList and
 *   active mount points (@c isoDirs) based on @p operation; unmount operations
 *   skip database cleanup and disable the manual-refresh keybinding (R). The
 *   operation colour is chosen from @p operation at entry (red for rm, green
 *   for cp/mount, yellow for mv/write2usb/umount).
 * - **RAII keybindings:** @c ReadlineKeybindingGuard calls
 *   @c setup_custom_keybindingsForSelect() on construction and
 *   @c reset_custom_keybindingsForSelect() on destruction, so cleanup runs on
 *   every exit path (break, ESC return, exception) without manual resets. The
 *   guard is scoped to the whole function.
 * - **RAII atomic flags:** @c AtomicFlagGuard sets @p isAtISOList to @c false
 *   for the processing block and restores it on scope exit.
 *   @c GlobalState::g_filteringIndicator is likewise wrapped by its own
 *   @c AtomicFlagGuard during the same block. For non-unmount operations,
 *   @c IsoListStateGuard restores @p isAtISOList to its prior value on scope
 *   exit.
 * - **Event-driven refresh:** a detached watcher thread shares the
 *   @c RefreshState (via @c std::shared_ptr). It is spawned only for
 *   non-unmount operations while an import is running, at most once per import
 *   session (guarded by @c isWatcherRunning), and redraws the list when the
 *   import thread signals completion via @c importCV.
 * - **Stacked filtering:** supports successive narrowing via @c filteringStack.
 *   Reads/writes of @c filteredFiles and sizing evaluations are protected by
 *   @c GlobalMutexes::updateListMutex to avoid races with the watcher thread.
 *   On ESC (@c '\\x1b', typically bound to the @c '<' key via
 *   @c setup_custom_keybindingsForSelect): if a filter is active, clears the
 *   filter stack, restores @c currentPage to @c originalPage, and continues;
 *   if no filter is active, sets @c currentPage to 0 and returns to the
 *   previous menu (keybinding cleanup handled by the RAII guard).
 * - **Filtering lock enforcement:** attempting to filter while a background
 *   import is running blocks the operation and sets
 *   @c GlobalState::g_filteringIndicator to @c true ("Filtering locked during
 *   sync") to notify the user.
 * - **Two-phase execution:** indices are staged into @c pendingIndices (a
 *   @c std::vector<std::string>) and executed in batch with the @c "P"
 *   command; @c "clr" discards the pending set. @c "P" with an empty pending
 *   set displays a warning and continues.
 * - **Manual refresh:** pressing @c "R" (when not unmount, the ISO list is
 *   non-empty, and no import is running) spawns a background database import.
 *   Stale completed threads in @p backgroundThreads are joined and erased
 *   before each new import is launched.
 * - **Persistent directory state:** @c isoDirs is @c static, so its contents
 *   survive re-entry for the lifetime of the process.
 * - **Terminal integrity:** @c \\f and @c \\t are bound to no-ops via Readline
 *   to prevent terminal corruption, and ANSI escapes (@c \\033[1A\\033[K,
 *   @c \\033[1B\\033[K) keep the interface feeling static during input.
 *   PgUp/PgDn are disabled when @c ITEMS_PER_PAGE is 0.
 * - **Resize repaint:** each @c readline() is wrapped by
 *   @c readlineWithResize, whose lambda re-runs the normal display step so the
 *   list repaints on terminal-width changes while the prompt waits. The same
 *   lambda is passed to the filter handler for the case where the live preview
 *   is off. (Image select repaints with a throwaway sort flag, so a resize
 *   never re-sorts the list.)
 *
 * @param operation         Target action ("mount", "umount", "cp", "mv", "rm",
 *                          or "write2usb"). Determines list source, colour
 *                          scheme, and dispatch.
 * @param isAtISOList       Set to @c true while the ISO list is displayed
 *                          (non-unmount only); managed by the RAII guards
 *                          during operation execution. Also gates
 *                          watcher-thread repaints.
 * @param backgroundThreads Joinable worker threads (from manual R-press
 *                          imports), retained for lifetime management; stale
 *                          completed threads are joined and erased before each
 *                          new import.
 * @param refreshState      Shared UI state and condition variable used to
 *                          synchronize the watcher with the active import
 *                          session. If @c nullptr, a new @c RefreshState is
 *                          constructed internally.
 */
void selectForIsoFiles(const std::string& operation,
                       std::atomic<bool>& isAtISOList,
                       std::vector<std::thread>& backgroundThreads,
                       std::shared_ptr<RefreshState> refreshState) {

    // --- RAII: keybindings are automatically reset on any exit path ---
    ReadlineKeybindingGuard keybindingGuard;

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    static std::vector<std::string> isoDirs;
    isoDirs.reserve(1000);

    if (!refreshState) refreshState = std::make_shared<RefreshState>();

    std::vector<std::string>& filteredFiles = refreshState->filteredFiles;
    std::vector<std::string>& pendingIndices = refreshState->pendingIndices;
    bool& isFiltered                         = refreshState->isFiltered;
    bool& hasPendingProcess                  = refreshState->hasPendingProcess;
    bool& umountMvRmBreak                    = refreshState->umountMvRmBreak;
    std::string& listSubtype                 = refreshState->listSubtype;
    size_t& currentPage                      = refreshState->currentPage;
    size_t& originalPage                     = refreshState->originalPage;

    filteredFiles.reserve(1000);
    isFiltered        = false;
    hasPendingProcess = false;
    umountMvRmBreak   = false;
    currentPage       = 0;
    originalPage      = 0;

    bool needsClrScrn  = true;
    bool filterHistory = false;

    std::string operationColor = std::string(
        operation == "rm"        ? UI::Palette::Red    :
        operation == "cp"        ? UI::Palette::Green  :
        operation == "mv"        ? UI::Palette::Yellow :
        operation == "mount"     ? UI::Palette::Green  :
        operation == "write2usb" ? UI::Palette::Yellow :
        operation == "umount"    ? UI::Palette::Yellow : UI::Palette::RL_BoldAlt
    );

    const bool isUnmount = (operation == "umount");
    listSubtype = (operation == "mount") ? "mount" : (operation == "write2usb") ? "write2usb" : "cp_mv_rm";

    // --- RAII: isAtISOList state automatically managed ---
    IsoListStateGuard isoListGuard(isAtISOList, [&isUnmount]() { return isUnmount; });

    while (true) {
        clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();
        GlobalState::g_operationCancelled.store(false);
        filterHistory = false;
        clear_history();

        if (!isFiltered) originalPage = currentPage;

        if (!isUnmount) {
            removeNonExistentPathsFromDatabase(GlobalState::globalIsoFileList);
            isAtISOList.store(true);
        }

        if (needsClrScrn) {
            if (!isUnmount) {
                if (!loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, refreshState))
                    break;
            } else {
                if (!loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak, pendingIndices, hasPendingProcess, currentPage, originalPage, refreshState))
                    break;
            }
            std::cout << "\n\n";
            umountMvRmBreak = false;
        }

        if (refreshState->isImportRunning.load() && !isUnmount) {
            bool watcherExpected = false;
            if (refreshState->isWatcherRunning.compare_exchange_strong(watcherExpected, true)) {
                std::thread([&isAtISOList, refreshState]() {
                    refreshListAfterAutoUpdate(isAtISOList, refreshState);
                    refreshState->isWatcherRunning.store(false);
                }).detach();
            }
        }

        std::cout << "\033[2A\033[K";

        const ReadlineAndPromptTheme pt = getPromptTheme();

        setup_custom_keybindingsForSelect();

        // Unbind unused hotkeys for the umount prompt.
        if (isUnmount) {
            rl_bind_keyseq("R", rl_insert);
            rl_bind_keyseq("*", rl_insert);
        }

        std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

        std::string prompt =
            "\n" +
            prefix +
            std::string(UI::Palette::RL_idxGreen) + "IDX" +
            pt.primary + " ↵ for " + "\001" +
            operationColor + "\002" + operation +
            pt.primary + ", ? for help: " +
            pt.reset;

        // Repaints the list on terminal resize while the prompt waits. Runs on
        // the Readline thread (rl_event_hook); uses the same display step as
        // above.
        auto repaintList = [&] {
            const bool savedBreak = umountMvRmBreak;
            if (!isUnmount)
                loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak,
                                  pendingIndices, hasPendingProcess, currentPage, originalPage,
                                  refreshState);
            else
                loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak,
                                          pendingIndices, hasPendingProcess, currentPage, originalPage,
                                          refreshState);
            umountMvRmBreak = savedBreak;
        };

        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, repaintList);
        rl_startup_hook = nullptr;

        if (!rawInput) break;

        std::string inputString(rawInput.get());

        if (inputString[0] == ';' || inputString.find(";;") != std::string::npos) {
            needsClrScrn = false;
            continue;
        }

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        if (inputString[0] == 'R' && refreshState->isImportRunning.load()) {
            std::cout << "\033[1B\033[K";
            needsClrScrn = false;
            continue;
        }

        if (inputString == "R" && !isUnmount && !GlobalState::globalIsoFileList.empty() && !refreshState->isImportRunning.load()) {
            backgroundThreads.erase(
                std::remove_if(backgroundThreads.begin(), backgroundThreads.end(),
                    [](std::thread& t) {
                        if (t.joinable()) { t.join(); return true; }
                        return false;
                    }),
                backgroundThreads.end()
            );
            needsClrScrn = true;
            refreshState->isImportRunning.store(true);
            backgroundThreads.emplace_back([refreshState] {
                backgroundDatabaseImport(refreshState);
                refreshState->isWatcherRunning.store(false);
            });
            continue;
        }

        if (inputString == "\x1b") {
            if (isFiltered) {
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                needsClrScrn = true;
                continue;
            } else {
                currentPage = 0;
                return;
            }
        }

        if (inputString == "P" && pendingIndices.empty()) {
            std::cout << "\033[1B\033[K";
            needsClrScrn = false;
            hasPendingProcess = false;
            continue;
        }

        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        size_t totalPages = 0;
        {
            std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
            const std::vector<std::string>& currentList = isFiltered ? filteredFiles : (isUnmount ? isoDirs : GlobalState::globalIsoFileList);
            totalPages = (GlobalState::ITEMS_PER_PAGE != 0) ? ((currentList.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE) : 0;
        }
        bool need2Sort = false;

        bool validCommand = processPaginationHelpAndDisplay(
            inputString,
            totalPages,
            currentPage,
            isFiltered,
            needsClrScrn,
            operation,
            need2Sort,
            &isAtISOList,
            filteredFiles
        );

        if (validCommand) continue;

        // --- RAII: lock guard automatically releases on scope exit ---
        bool filteringHandled = false;
        {
            if ((inputString == "/" || (!inputString.empty() && inputString[0] == '/')) && !refreshState->isImportRunning.load()) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                if (handleFilteringForISO(inputString, filteredFiles, isFiltered, needsClrScrn,
                                          filterHistory, operation, operationColor, isoDirs, isUnmount, currentPage,
                                          refreshState, repaintList)) {
                    GlobalState::g_filteringIndicator.store(false);
                    filteringHandled = true;
                }
            } else if ((inputString == "/" || (!inputString.empty() && inputString[0] == '/')) && refreshState->isImportRunning.load()) {
                GlobalState::g_filteringIndicator.store(true);
                needsClrScrn = true;
                continue;
            }
        }

        if (filteringHandled) {
            keybindingGuard.restore();
            std::cout << "\033[1B\033[K";
            continue;
        }

        bool pendingHandled = handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn);
        if (pendingHandled) continue;

        // --- RAII: atomic flag guard for the processing block ---
        bool pendingExecuted = false;
        {
            AtomicFlagGuard processingGuard(isAtISOList, false);
            AtomicFlagGuard filteringGuard(GlobalState::g_filteringIndicator, true);

            needsClrScrn = true;

            pendingExecuted = handlePendingProcess(
                inputString,
                pendingIndices,
                hasPendingProcess,
                [&](const std::string& combinedIndices) {
                    processOperationForSelectedIsoFiles(
                        combinedIndices,
                        isFiltered,
                        filteredFiles,
                        isoDirs,
                        operation,
                        umountMvRmBreak,
                        filterHistory);
                });

            if (!pendingExecuted) {
                processOperationForSelectedIsoFiles(inputString, isFiltered, filteredFiles, isoDirs,
                                                    operation, umountMvRmBreak, filterHistory);
            }
        }
        // isAtISOList automatically restored here.
        keybindingGuard.restore();
        if (pendingExecuted) continue;
    }
    // Keybinding guard destructor automatically calls reset_custom_keybindingsForSelect().
}

/**
 * @brief Interactive TUI for selecting disc images to convert to ISO.
 *
 * Paginated, filterable interface for choosing BIN/IMG, MDF, NRG, CHD, or
 * DAA/GBI files and dispatching them to the convert2iso pipeline. Drives the
 * same Readline event loop as @c selectForIsoFiles, but over the image cache
 * for the requested @p fileType, with the matching operation name.
 *
 * - **File type mapping:** @p fileType selects the extension(s), the display
 *   name, and the conversion operation (@c "ccd2iso", @c "mdf2iso",
 *   @c "nrg2iso", @c "chd2iso", @c "daa2iso"); "bin" and "img" both map to
 *   the BIN/IMG conversion.
 * - **RAII keybindings:** @c ReadlineKeybindingGuard installs select-mode
 *   bindings on construction and resets them on destruction, so cleanup runs
 *   on every exit path.
 * - **Resize repaint:** each @c readline() is wrapped in
 *   @c readlineWithResize with a lambda that re-runs the display step. A
 *   throwaway sort flag is used so a resize never re-sorts the list.
 * - **Cancellation:** ESC clears the filter (restoring the unfiltered cache
 *   for this file type and the original page) if a filter is active,
 *   otherwise resets the page and returns to the caller. The @c R key is
 *   unbound because this prompt has no manual refresh.
 * - **Two-phase execution:** indices are staged into @p pendingIndices and
 *   executed with @c "P" (or dispatched directly when the input is not batch
 *   syntax). Verbose output, when enabled, is printed after each dispatch.
 *
 * @param fileType  "bin", "img", "mdf", "nrg", "chd", or "daa".
 * @param files     The image file list being filtered/selected; updated in
 *                  place by filtering and by ESC-restore.
 * @param list      Toggled by the "ls" command (from the caller) to view the
 *                  RAM cache.
 * @param state     Shared @c RefreshState forwarded to
 *                  @c handleFilteringConvert2ISO for display coordination.
 */
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files,
                         bool& list, std::shared_ptr<RefreshState> state) {

    // --- RAII: keybindings are automatically reset on any exit path ---
    ReadlineKeybindingGuard keybindingGuard;

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    std::vector<std::string> pendingIndices;
    bool hasPendingProcess = false;

    size_t currentPage = 0;
    size_t originalPage = currentPage;

    bool isFiltered = false;
    bool needsClrScrn = true;
    bool filterHistory = false;
    bool need2Sort = true;

    std::string fileExtension;
    std::string fileExtensionWithOutDots;
    std::string operation;
    if (fileType == "bin" || fileType == "img") {
        fileExtension = ".bin/.img";
        operation = "ccd2iso";
        fileExtensionWithOutDots = "BIN/IMG";
    } else if (fileType == "mdf") {
        fileExtension = ".mdf";
        fileExtensionWithOutDots = "MDF";
        operation = "mdf2iso";
    } else if (fileType == "nrg") {
        fileExtension = ".nrg";
        fileExtensionWithOutDots = "NRG";
        operation = "nrg2iso";
    } else if (fileType == "chd") {
        fileExtension = ".chd";
        fileExtensionWithOutDots = "CHD";
        operation = "chd2iso";
    } else if (fileType == "daa") {
        fileExtension = ".daa/.gbi";
        fileExtensionWithOutDots = "DAA/GBI";
        operation = "daa2iso";
    } else {
        fileExtension = "";
        fileExtensionWithOutDots = "FILES";
    }

    while (true) {
        clearGlobalVerboseSets();
        enable_ctrl_d();
        setupSignalHandlerCancellations();

        GlobalState::g_operationCancelled.store(false);
        bool verbose = false;

        if (!isFiltered) originalPage = currentPage;

        clear_history();
        if (needsClrScrn) {
            loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list,
                                     pendingIndices, hasPendingProcess, currentPage,
                                     state);
            std::cout << "\n\n";
        }

        std::cout << "\033[2A\033[K";

        const ReadlineAndPromptTheme pt = getPromptTheme();

        setup_custom_keybindingsForSelect();

        // Unused in convert2iso prompts.
        rl_bind_keyseq("R", rl_insert);

        std::string prefix = isFiltered ? (pt.filter + "F⊳ ") : "";

        std::string prompt =
            "\n" +
            prefix +
            std::string(UI::Palette::RL_idxGreen) + "IDX" +
            pt.primary   + " ↵ for " +
            pt.highlight + operation +
            pt.primary   + ", ? for help: " +
            pt.reset;

        // Repaints the list on terminal resize. A throwaway sort flag keeps a
        // resize from re-sorting the list.
        auto repaintList = [&] {
            bool sortNow = false;
            loadAndDisplayImageFiles(files, fileType, sortNow, isFiltered, list,
                                     pendingIndices, hasPendingProcess, currentPage,
                                     state);
        };

        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, repaintList);
        rl_startup_hook = nullptr;

        if (!rawInput) break;

        std::string inputString(rawInput.get());

        if (inputString == "\x1b") {
            clearScrollBuffer();
            if (isFiltered) {
                // Restore original unfiltered file list.
                if (fileType == "bin" || fileType == "img") {
                    files = GlobalState::binImgFilesCache;
                } else if (fileType == "mdf") {
                    files = GlobalState::mdfMdsFilesCache;
                } else if (fileType == "nrg") {
                    files = GlobalState::nrgFilesCache;
                } else if (fileType == "chd") {
                    files = GlobalState::chdFilesCache;
                } else if (fileType == "daa") {
                    files = GlobalState::daaGbiFilesCache;
                }
                needsClrScrn = true;
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                need2Sort = false;
                continue;
            } else {
                currentPage = 0;
                need2Sort = false;
                return;
            }
        }

        if (inputString == "P" && pendingIndices.empty()) {
            std::cout << "\033[1B\033[K";
            hasPendingProcess = false;
            needsClrScrn = false;
            continue;
        }

        if (inputString == "clr") {
            pendingIndices.clear();
            hasPendingProcess = false;
            needsClrScrn = true;
            continue;
        }

        if (inputString[0] == ';' || inputString.find(";;") != std::string::npos) {
            needsClrScrn = false;
            continue;
        }

        if (rawInput.get()[0] == '\0') {
            needsClrScrn = false;
            continue;
        }

        size_t totalPages = (GlobalState::ITEMS_PER_PAGE != 0)
            ? ((files.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE)
            : 0;

        bool validCommand = processPaginationHelpAndDisplay(
            inputString,
            totalPages,
            currentPage,
            isFiltered,
            needsClrScrn,
            operation,
            need2Sort,
            nullptr,
            files
        );

        if (validCommand) continue;

        if (handlePendingProcess(
                inputString,
                pendingIndices,
                hasPendingProcess,
                [&](const std::string& combinedIndices) {
                    processInputForConversions(
                        combinedIndices,
                        files,
                        fileType == "mdf",
                        fileType == "nrg",
                        fileType == "chd",
                        fileType == "daa",
                        verbose);
                })) {
            keybindingGuard.restore();

            needsClrScrn = true;

            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages,
                             verboseSets.operationCompleted,
                             verboseSets.operationSkipped,
                             verboseSets.operationFailed, 3);
            }

            continue;
        }

        if (inputString == "/" || (!inputString.empty() && inputString[0] == '/')) {
            handleFilteringConvert2ISO(inputString, files, operation, isFiltered,
                                       needsClrScrn, filterHistory, need2Sort, currentPage,
                                       pendingIndices, hasPendingProcess, state,
                                       repaintList);
            keybindingGuard.restore();
            std::cout << "\033[1B\033[K";
            continue;
        }
        else if (inputString.find(';') != std::string::npos) {
            if (handlePendingInduction(inputString, pendingIndices, hasPendingProcess, needsClrScrn)) {
                continue;
            }
        }
        else {
            processInputForConversions(inputString, files,
                                       (fileType == "mdf"),
                                       (fileType == "nrg"),
                                       (fileType == "chd"),
                                       (fileType == "daa"),
                                       verbose);
            needsClrScrn = true;
            if (verbose) {
                verbosePrint(verboseSets.uniqueErrorTokenMessages,
                            verboseSets.operationCompleted,
                            verboseSets.operationSkipped,
                            verboseSets.operationFailed, 3);
                needsClrScrn = true;
            }
            keybindingGuard.restore();
        }
    }
    // Keybinding guard destructor automatically calls reset_custom_keybindingsForSelect().
}
