// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "isocmd/database/databaseOps.h"
#include "isocmd/filter/filter.h"
#include "isocmd/input/inputHandling.h"
#include "isocmd/core/globalMutexes.h"
#include "isocmd/ui/rendering.h"
#include "isocmd/input/readline.h"
#include "isocmd/select/select.h"
#include "isocmd/core/sharedRefreshState.h"
#include "isocmd/core/state.h"
#include "isocmd/ui/themes.h"
#include "isocmd/ui/verbose.h"

using namespace select_shared;

namespace {

    // ─── ISO select screen ───────────────────────────────────────────────────────
    namespace iso_select {

        using select_common::LoopAction;
        using select_common::isFilterCommand;


        /**
         * @brief Runs a batch operation on user-selected ISO indices and reports results.
         *
         * Resolves the active list (filtered view unless @p useSourceList is true),
         * dispatches to the mount/umount, write2usb, or cp/mv/rm pipeline, then
         * invokes handleSelectIsoFilesResults() to print the outcome and possibly
         * clear readline history.
         *
         * @param inputString   Raw user input containing 1-based indices or ranges.
         * @param isFiltered    Whether a filter is currently active.
         * @param filteredFiles Filtered view of the ISO list (used when @p isFiltered
         *                      and @p useSourceList is false).
         * @param isoDirs       Mounted ISO directory list (used for unmount operations).
         * @param operation     Target action ("mount", "umount", "cp", "mv", "rm",
         *                      "write2usb").
         * @param umountMvRmBreak Set true for unmount and forwarded/updated by the
         *                      cp/mv/rm and mount/umount pipelines.
         * @param filterHistory Whether to load/save readline history for cp/mv/rm.
         * @param useSourceList When true, indices refer to the full source list even
         *                      if a filter is active (used for "P", whose indices are
         *                      source indices).
         */
        void processOperation(const std::string& inputString,
                              bool& isFiltered,
                              const std::vector<std::string>& filteredFiles,
                              std::vector<std::string>& isoDirs,
                              const std::string& operation,
                              bool& umountMvRmBreak,
                              bool& filterHistory,
                              bool useSourceList) {

            clearScrollBuffer();
            bool verbose = false;

            const bool isUnmount = (operation == "umount");
            const std::vector<std::string>& sourceList =
                isUnmount ? isoDirs : GlobalState::globalIsoFileList;
            const std::vector<std::string>& activeList =
                (isFiltered && !useSourceList) ? filteredFiles : sourceList;

            if (operation == "mount" || isUnmount) {
                if (isUnmount) umountMvRmBreak = true;
                processInputForMountOrUmount(inputString, activeList, umountMvRmBreak, verbose, isUnmount);
            } else if (operation == "write2usb") {
                writeToUsb(inputString, activeList);
            } else {
                processInputForCpMvRm(inputString, activeList, operation, umountMvRmBreak, filterHistory, verbose);
            }

            handleSelectIsoFilesResults(operation, verbose, isFiltered, umountMvRmBreak);
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
         * - **Pending selection:** the watcher never touches the staged selection.
         *   Staged paths survive the reload by construction; the index display is
         *   recomputed by the loader on the main thread when it redraws (see
         *   @c syncIsoPendingDisplay).
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
         * @brief Per-session state and behavior for the interactive ISO select
         *        screen.
         *
         * Owns every mutable field needed by one invocation of
         * selectForIsoFiles() — the active list view, filter stack, pagination,
         * staged selection, and background-thread bookkeeping — and exposes
         * the loop's input handlers as member functions. The dispatcher
         * tries those handlers in a fixed order (ignore, manual refresh,
         * ESC, staged commands, pagination/help, filtering, staging, then
         * execute).
         *
         * Fields mirror @c RefreshState and are bound by reference in the
         * constructor, so state persists across loop iterations and is
         * visible to the watcher thread.
         */
        struct SelectSession {
            const std::string&              operation;
            std::atomic<bool>&              isAtISOList;
            std::vector<std::thread>&       backgroundThreads;
            std::shared_ptr<RefreshState>   refreshState;
            std::vector<std::string>&       isoDirs;
            ReadlineKeybindingGuard&        keybindingGuard;
            const bool                      isUnmount;
            const std::string               operationColor;

            // Aliases into RefreshState (same objects the original used).
            std::vector<std::string>& filteredFiles;
            std::vector<std::string>& pendingIndices;
            bool&                     isFiltered;
            bool&                     hasPendingProcess;
            bool&                     umountMvRmBreak;
            std::string&              listSubtype;
            size_t&                   currentPage;
            size_t&                   originalPage;

            bool needsClrScrn  = true;
            bool filterHistory = false;

            /**
             * @brief Binds per-session state to the shared refresh state and
             *        resets per-entry counters.
             *
             * Reserves capacity for the filtered view, clears the filter,
             * pending selection, pagination, and the umount/mv/rm break flag,
             * and selects the list subtype from @p op.
             *
             * @param op      Target action; also drives @c isUnmount and
             *                @c operationColor.
             * @param atList  Atomic flag set while the list view is active.
             * @param threads Background-thread vector shared with the caller.
             * @param state   Shared refresh state whose fields are bound by
             *                reference.
             * @param dirs    Persistent mounted-ISO directory list (unmount
             *                source).
             * @param guard   RAII keybinding guard owned by the caller.
             */
            SelectSession(const std::string& op,
                          std::atomic<bool>& atList,
                          std::vector<std::thread>& threads,
                          std::shared_ptr<RefreshState> state,
                          std::vector<std::string>& dirs,
                          ReadlineKeybindingGuard& guard)
                : operation(op),
                  isAtISOList(atList),
                  backgroundThreads(threads),
                  refreshState(std::move(state)),
                  isoDirs(dirs),
                  keybindingGuard(guard),
                  isUnmount(op == "umount"),
                  operationColor(pickOperationColor(op)),
                  filteredFiles(refreshState->filteredFiles),
                  pendingIndices(refreshState->pendingIndices),
                  isFiltered(refreshState->isFiltered),
                  hasPendingProcess(refreshState->hasPendingProcess),
                  umountMvRmBreak(refreshState->umountMvRmBreak),
                  listSubtype(refreshState->listSubtype),
                  currentPage(refreshState->currentPage),
                  originalPage(refreshState->originalPage) {

                filteredFiles.reserve(1000);
                isFiltered        = false;
                hasPendingProcess = false;
                umountMvRmBreak   = false;
                currentPage       = 0;
                originalPage      = 0;

                // Staged selection starts empty on every entry; it is held as paths
                // and projected into pendingIndices for display.
                pendingIndices.clear();

                listSubtype = (op == "mount")     ? "mount"
                            : (op == "write2usb") ? "write2usb"
                                                  : "cp_mv_rm";
            }

            /**
             * @brief Maps the operation name to its ANSI color code.
             *
             * @param op Operation name ("rm", "cp", "mv", "mount", "write2usb",
             *           "umount", or anything else).
             * @return ANSI color code string for the operation, defaulting to
             *         @c UI::Palette::RL_BoldAlt for unknown operations.
             */
            static std::string pickOperationColor(const std::string& op) {
                return std::string(
                    op == "rm"        ? UI::Palette::Red    :
                    op == "cp"        ? UI::Palette::Green  :
                    op == "mv"        ? UI::Palette::Yellow :
                    op == "mount"     ? UI::Palette::Green  :
                    op == "write2usb" ? UI::Palette::Yellow :
                    op == "umount"    ? UI::Palette::Yellow : UI::Palette::RL_BoldAlt
                );
            }

            // ----- helpers shared by several steps ---------------------------------

            /**
             * @brief Drops every active filter and restores the pre-filter page.
             *
             * Clears the filter stack, marks the screen for redraw, and returns
             * @c currentPage to @c originalPage.
             */
            void clearFilter() {
                isFiltered = false;
                filteringStack.clear();
                currentPage = originalPage;
                needsClrScrn = true;
            }

            /**
             * @brief Draws the list for the current mode.
             *
             * Dispatches to the ISO list loader or the mounted-ISOs loader
             * depending on @c isUnmount.
             *
             * @return true on success; false if the display step failed.
             */
            bool drawList() {
                if (!isUnmount) {
                    return loadAndDisplayIso(filteredFiles, isFiltered, listSubtype, umountMvRmBreak,
                                             pendingIndices, hasPendingProcess, currentPage,
                                             originalPage, refreshState);
                }
                return loadAndDisplayMountedISOs(isoDirs, filteredFiles, isFiltered, umountMvRmBreak,
                                                 pendingIndices, hasPendingProcess, currentPage,
                                                 originalPage, refreshState);
            }

            /**
             * @brief Repaints the list on terminal resize while the prompt waits.
             *
             * Runs on the Readline thread (@c rl_event_hook) and uses the same
             * display step as the main loop. Saves and restores
             * @c umountMvRmBreak so the resize repaint cannot clobber loop state.
             */
            void repaintList() {
                const bool savedBreak = umountMvRmBreak;
                drawList();
                umountMvRmBreak = savedBreak;
            }

            // ----- per-iteration steps ---------------------------------------------

            /**
             * @brief Resets per-iteration state at the top of the loop.
             *
             * Clears verbose sets, re-enables Ctrl+D, reinstalls signal
             * handlers, resets the cancellation flag, clears readline
             * history, snapshots @c currentPage into @c originalPage while
             * unfiltered, and (for non-unmount) prunes database entries whose
             * paths have disappeared and marks the list as current.
             */
            void beginIteration() {
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
            }

            /**
             * @brief Puts the cursor on the prompt row and prepares for the
             *        next readline().
             *
             * Redraw path (@c needsClrScrn is true): draw the list, print one
             * blank separator row, and reset @c umountMvRmBreak. No-redraw
             * path: a single @c term::rewindToPromptRow() call returns to the
             * prompt row below the previous prompt. See the prompt-row cursor
             * invariant documented at the top of the select screen.
             *
             * @return false if the display step failed; true otherwise.
             */
            bool showListOrRewind() {
                if (!needsClrScrn) {
                    term::rewindToPromptRow();
                    return true;
                }
                if (!drawList()) return false;
                std::cout << "\n";
                umountMvRmBreak = false;
                return true;
            }

            /**
             * @brief Starts the background refresh watcher if an import is
             *        running and one is not already in flight.
             *
             * No-op for unmount mode or when no import is running. Uses an
             * atomic compare-exchange on @c RefreshState::isWatcherRunning to
             * guarantee at most one watcher per session; the watcher resets
             * the flag when it exits.
             */
            void startWatcherIfNeeded() {
                if (isUnmount || !refreshState->isImportRunning.load()) return;

                bool watcherExpected = false;
                if (!refreshState->isWatcherRunning.compare_exchange_strong(watcherExpected, true))
                    return;

                std::thread([&atList = isAtISOList, state = refreshState]() {
                    refreshListAfterAutoUpdate(atList, state);
                    state->isWatcherRunning.store(false);
                }).detach();
            }

            /**
             * @brief Installs select-screen keybindings and unbinds keys that
             *        are unused for the current mode.
             *
             * In unmount mode, "R" (manual refresh) and "*" are rebound to
             * @c rl_insert so they can appear as normal characters.
             */
            void bindPromptKeys() {
                setup_custom_keybindingsForSelect();

                // Unbind unused hotkeys for the umount prompt.
                if (isUnmount) {
                    rl_bind_keyseq("R", rl_insert);
                    rl_bind_keyseq("*", rl_insert);
                }
            }

            /**
             * @brief Builds the colored prompt string for the current operation.
             *
             * @param pt Theme tokens for primary text and reset.
             * @return Formatted prompt string for @c readlineWithResize().
             */
            std::string buildPrompt(const ReadlineAndPromptTheme& pt) const {
                return std::format("{0}IDX{1} ↵ for \001{2}\002{3}{1}, ? for help: {4}",
                    UI::Palette::RL_idxGreen,
                    pt.primary,
                    operationColor,
                    operation,
                    pt.reset
                );
            }

            /**
             * @brief Handles Ctrl-D / EOF on the prompt.
             *
             * If a filter is active, clears it so the next loop iteration
             * shows the unfiltered list; the caller then leaves the loop.
             */
            void onEndOfInput() {
                if (isFiltered) clearFilter();
            }

            // ----- input handlers (tried in this order) ----------------------------

            /**
             * @brief Swallows empty, comment-like, and suppressed input.
             *
             * Ignores empty lines, lines starting with ';', lines containing
             * ";;", and the "R" refresh key while a background import is
             * running. Marks the screen as not needing a redraw and returns
             * @c LoopAction::Continue for ignored input; otherwise returns
             * @c LoopAction::Proceed.
             *
             * @param input Raw user input.
             * @return @c Continue when the input was ignored, @c Proceed
             *         otherwise.
             */
            LoopAction ignoreInput(const std::string& input) {
                const bool ignored =
                    input.empty() ||
                    input[0] == ';' ||
                    input.find(";;") != std::string::npos ||
                    (input[0] == 'R' && refreshState->isImportRunning.load());
                if (!ignored) return LoopAction::Proceed;
                needsClrScrn = false;
                return LoopAction::Continue;
            }

            /**
             * @brief Handles the manual "R" refresh command.
             *
             * Only fires when not unmounting, the ISO list is non-empty, and
             * no import is already running. Joins and erases any joinable
             * stale worker threads from earlier imports, then spawns a new
             * background import that resets the watcher flag when done.
             *
             * @param input Raw user input.
             * @return @c Continue if a refresh was launched, @c Proceed
             *         otherwise.
             */
            LoopAction manualRefresh(const std::string& input) {
                const bool canRefresh = input == "R" && !isUnmount &&
                                        !GlobalState::globalIsoFileList.empty() &&
                                        !refreshState->isImportRunning.load();
                if (!canRefresh) return LoopAction::Proceed;

                backgroundThreads.erase(
                    std::remove_if(backgroundThreads.begin(), backgroundThreads.end(),
                        [](std::thread& t) {
                            if (t.joinable()) { t.join(); return true; }
                            return false;
                        }),
                    backgroundThreads.end());

                needsClrScrn = true;
                refreshState->isImportRunning.store(true);
                backgroundThreads.emplace_back([state = refreshState] {
                    backgroundDatabaseImport(state);
                    state->isWatcherRunning.store(false);
                });
                return LoopAction::Continue;
            }

            /**
             * @brief Handles the ESC key ('\x1b').
             *
             * With an active filter: clears it and continues. Without a
             * filter: resets pagination and returns to the previous menu.
             *
             * @param input Raw user input.
             * @return @c Continue when a filter was cleared, @c Return when
             *         leaving the screen, @c Proceed otherwise.
             */
            LoopAction escape(const std::string& input) {
                if (input != "\x1b") return LoopAction::Proceed;
                if (isFiltered) {
                    clearFilter();
                    return LoopAction::Continue;
                }
                currentPage = 0;
                return LoopAction::Return;
            }

            /**
             * @brief Clears all staged-selection state.
             *
             * Empties the staged path set, the display index vector, and the
             * pending-process flag.
             */
            void clearPendingSelection() {
                g_isoPending.paths.clear();
                pendingIndices.clear();
                hasPendingProcess = false;
            }

            /**
             * @brief Handles the "P" (process staged) and "clr" (clear staged)
             *        commands.
             *
             * "P" with an empty staged set is a no-op (returns @c Continue).
             * "clr" empties the staged set and requests a redraw. Any other
             * input returns @c Proceed.
             *
             * @param input Raw user input.
             * @return @c Continue when the command was handled, @c Proceed
             *         otherwise.
             */
            LoopAction stagedCommands(const std::string& input) {
                if (input == "P" && g_isoPending.paths.empty()) {
                    needsClrScrn = false;
                    hasPendingProcess = false;
                    return LoopAction::Continue;
                }
                if (input == "clr") {
                    clearPendingSelection();
                    needsClrScrn = true;
                    return LoopAction::Continue;
                }
                return LoopAction::Proceed;
            }

            /**
             * @brief Computes the total page count for the currently displayed
             *        list.
             *
             * Uses @c filteredFiles when a filter is active, otherwise the
             * unmount directory list or the global ISO list. Protected by
             * @c GlobalMutexes::updateListMutex. Returns 0 when
             * @c GlobalState::ITEMS_PER_PAGE is zero (pagination disabled).
             *
             * @return Total number of pages in the active list.
             */
            size_t countPages() {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                const std::vector<std::string>& currentList =
                    isFiltered ? filteredFiles
                               : (isUnmount ? isoDirs : GlobalState::globalIsoFileList);
                if (GlobalState::ITEMS_PER_PAGE == 0) return 0;
                return (currentList.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;
            }

            /**
             * @brief Handles pagination and help commands.
             *
             * Delegates to @c processPaginationHelpAndDisplay(), which
             * recognizes page-navigation and help input and updates
             * pagination state.
             *
             * @param input Raw user input.
             * @return @c Continue when the command was handled, @c Proceed
             *         otherwise.
             */
            LoopAction pagination(const std::string& input) {
                size_t totalPages = countPages();
                bool need2Sort = false;
                const bool handled = processPaginationHelpAndDisplay(
                    input, totalPages, currentPage, isFiltered, needsClrScrn,
                    operation, need2Sort, &isAtISOList, filteredFiles);
                return handled ? LoopAction::Continue : LoopAction::Proceed;
            }

            /**
             * @brief Handles filter-command input.
             *
             * Blocks filtering while a background import is running (sets
             * @c GlobalState::g_filteringIndicator to true and requests a
             * redraw). Otherwise delegates to @c handleFilteringForISO() under
             * @c GlobalMutexes::updateListMutex, restoring keybindings and
             * clearing the filtering indicator on success.
             *
             * @param input Raw user input.
             * @return @c Continue when the command was handled (or filtering
             *         was blocked), @c Proceed otherwise.
             */
            LoopAction filtering(const std::string& input) {
                if (!isFilterCommand(input)) return LoopAction::Proceed;

                if (refreshState->isImportRunning.load()) {
                    // Filtering is locked while a background import is running.
                    GlobalState::g_filteringIndicator.store(true);
                    needsClrScrn = true;
                    return LoopAction::Continue;
                }

                bool handled = false;
                {
                    std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                    FilterEntryContext ctx {
                        .isFiltered    = &isFiltered,
                        .needsClrScrn  = &needsClrScrn,
                        .filterHistory = &filterHistory,
                        .currentPage   = &currentPage,
                        .state         = refreshState,
                        .repaintList   = [this] { repaintList(); }
                    };
                    handled = handleFilteringForISO(input, filteredFiles, operation, operationColor,
                                                      isoDirs, isUnmount, ctx);
                }
                if (!handled) return LoopAction::Proceed;

                GlobalState::g_filteringIndicator.store(false);
                keybindingGuard.restore();
                return LoopAction::Continue;
            }

            /**
             * @brief Stages an index selection ("3 5-9;") as paths.
             *
             * Bare numbers resolve against the current view (filtered or
             * source list); "n^" numbers are source indices. Delegates to
             * @c pending::stagePending() under
             * @c GlobalMutexes::updateListMutex.
             *
             * @param input Raw user input containing an index expression.
             * @return @c Continue when the input was staged, @c Proceed
             *         otherwise.
             */
            LoopAction stageSelection(const std::string& input) {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                const std::vector<std::string>& sourceList = *g_isoPending.source;
                const std::vector<std::string>& viewList   = isFiltered ? filteredFiles : sourceList;
                const bool staged = pending::stagePending(input, g_isoPending.paths, hasPendingProcess,
                                                          needsClrScrn, viewList, sourceList);
                return staged ? LoopAction::Continue : LoopAction::Proceed;
            }

            /**
             * @brief Resolves the staged path set to source indices, dropping
             *        vanished files.
             *
             * Returns the resolved indices compressed into a semicolon-
             * terminated token list suitable for passing to
             * @c processOperation(). Runs under
             * @c GlobalMutexes::updateListMutex.
             *
             * @return Semicolon-terminated index string; empty if nothing
             *         resolves.
             */
            std::string resolveStagedBatch() {
                std::lock_guard<std::mutex> lock(GlobalMutexes::updateListMutex);
                return pending::joinTokens(pending::compressRuns(
                    pending::resolveIndices(g_isoPending.paths, *g_isoPending.source, true)));
            }

            /**
             * @brief Executes the staged selection against the source list.
             *
             * Resolves the staged batch, runs @c processOperation() with
             * @c useSourceList=true, then clears the staged selection. A
             * no-op when the resolved batch is empty.
             */
            void runPending() {
                const std::string batch = resolveStagedBatch();
                if (batch.empty()) {
                    pendingIndices.clear();
                    hasPendingProcess = false;
                    return;
                }
                processOperation(batch, isFiltered, filteredFiles, isoDirs,
                                 operation, umountMvRmBreak, filterHistory,
                                 /*useSourceList=*/true);
                clearPendingSelection();
            }

            /**
             * @brief Executes a direct index selection.
             *
             * Runs @c processOperation() with @c useSourceList=false so the
             * indices resolve against the current view (filtered or source).
             *
             * @param input Raw user input containing indices or ranges.
             */
            void runDirect(const std::string& input) {
                processOperation(input, isFiltered, filteredFiles, isoDirs,
                                 operation, umountMvRmBreak, filterHistory,
                                 /*useSourceList=*/false);
                clearPendingSelection();
            }

            /**
             * @brief Runs the active batch command ("P" or a direct selection).
             *
             * Installs RAII guards that drop @c isAtISOList during processing
             * and set the filtering indicator on entry, marks the screen for
             * redraw, then delegates to @c runPending() or @c runDirect().
             *
             * @param input Raw user input ("P" or an index expression).
             */
            void execute(const std::string& input) {
                // --- RAII: atomic flag guards for the processing block ---
                AtomicFlagGuard processingGuard(isAtISOList, false);
                AtomicFlagGuard filteringGuard(GlobalState::g_filteringIndicator, true);

                needsClrScrn = true;

                if (input == "P") runPending();
                else              runDirect(input);
            }

            // ----- dispatcher ------------------------------------------------------

            /**
             * @brief Tries each input handler in order and executes unhandled
             *        input as an operation.
             *
             * Handler order: ignore, manual refresh, ESC, staged commands,
             * pagination/help, filtering, staging. If every handler returns
             * @c Proceed, executes @c execute() and restores keybindings.
             *
             * @param input Raw user input.
             * @return The first non-@c Proceed action returned by a handler;
             *         otherwise @c LoopAction::Continue after executing the
             *         operation.
             */
            LoopAction dispatch(const std::string& input) {
                using Handler = LoopAction (SelectSession::*)(const std::string&);
                static const Handler handlers[] = {
                    &SelectSession::ignoreInput,
                    &SelectSession::manualRefresh,
                    &SelectSession::escape,
                    &SelectSession::stagedCommands,
                    &SelectSession::pagination,
                    &SelectSession::filtering,
                    &SelectSession::stageSelection,
                };

                for (Handler h : handlers) {
                    const LoopAction action = (this->*h)(input);
                    if (action != LoopAction::Proceed) return action;
                }

                execute(input);              // isAtISOList / indicator restored on return
                keybindingGuard.restore();
                return LoopAction::Continue;
            }
        };

    } // namespace iso_select

} // namespace

// ---------------------------------------------------------------------------
// Global scope: public entry points
// ---------------------------------------------------------------------------

/**
 * @brief Projects the staged ISO selection onto the display state
 * (@p pendingIndices / @p hasPendingProcess) as 1-based SOURCE indices merged
 * into ranges, for the "Pending Indices" line. Does not prune staged paths.
 *
 * Called by @c loadAndDisplayIso and @c loadAndDisplayMountedISOs right
 * before they print (under @c updateListMutex), i.e. against the final,
 * reloaded and sorted list. Every redraw path therefore shows correct
 * numbers: loop redraws, resize repaints and asynchronous import refreshes.
 *
 * @param pendingIndices    Receives the 1-based source indices compressed into ranges for display.
 * @param hasPendingProcess Set to true if there are active pending selections, otherwise false.
 * @param importRunning     True while a background import is rebuilding the list;
 *                          the last good display is kept then.
 * @return true if an ISO select screen owns the selection (the display state
 *         is managed here); false if none is active, in which case the caller
 *         handles its own pending state.
 */
bool syncIsoPendingDisplay(std::vector<std::string>& pendingIndices,
                           bool& hasPendingProcess,
                           bool importRunning) {
    if (!g_isoPending.source) return false;
    if (importRunning) return true;
    // A momentarily empty source list right after an import is still being
    // repopulated; keep the last good display. (Vanished paths are pruned by "P".)
    if (g_isoPending.source->empty() && !g_isoPending.paths.empty()) return true;
    pendingIndices =
        pending::compressRuns(pending::resolveIndices(g_isoPending.paths, *g_isoPending.source, false));
    hasPendingProcess = !pendingIndices.empty();
    return true;
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
 * - **Event-driven refresh:** a detached watcher shares @c RefreshState via
 *   @c std::shared_ptr. It is started only for non-unmount operations while an
 *   import is running, at most once per session, and publishes a pending list
 *   refresh when @c importCV signals completion. The main Readline thread
 *   performs the actual redraw.
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
 * - **Two-phase execution (path-based):** a selection typed with a trailing
 *   semicolon ("3 5-9;") is staged as PATHS (@c g_isoPending). Bare numbers
 *   are what is on screen (the filtered view when filtering); "n^" numbers are
 *   source indices. The @c "P" command resolves the staged paths to source
 *   indices at that moment, drops files that no longer exist, and runs the
 *   operation against the full source list, so a selection staged under
 *   different filters (or none) is always executed correctly. The
 *   "Pending Indices" line shows the same source numbers as the "n:m^"
 *   column, merged into ranges. @c "clr" discards the staged set; @c "P" with
 *   nothing staged is a no-op.
 * - **Manual refresh:** pressing @c "R" (when not unmount, the ISO list is
 *   non-empty, and no import is running) spawns a background database import.
 *   Stale completed threads in @p backgroundThreads are joined and erased
 *   before each new import is launched.
 * - **Persistent directory state:** @c isoDirs is @c static, so its contents
 *   survive re-entry for the lifetime of the process.
 * - **Terminal integrity:** @c \\f and @c \\t are bound to no-ops via Readline
 *   to prevent terminal corruption. PgUp/PgDn are disabled when
 *   @c ITEMS_PER_PAGE is 0.
 * - **Prompt-row invariant:** the prompt is a single line and always lands on
 *   the same row (list, one blank separator row, prompt). When the list is
 *   redrawn the loop prints one @c "\\n" after it; when it is not, a single
 *   @c term::rewindToPromptRow() at the top of the loop returns to the prompt
 *   row. See the "Prompt-row cursor invariant" note near the top of this file.
 * - **Resize repaint:** each @c readline() is wrapped by
 *   @c readlineWithResize, whose lambda re-runs the normal display step so the
 *   list repaints on terminal-width changes while the prompt waits. The same
 *   lambda is passed to the filter handler for the case where the live preview
 *   is off. (Image select repaints with a throwaway sort flag, so a resize
 *   never re-sorts the list.)
 * - **Structure:** the loop body is delegated to @c iso_select::SelectSession,
 *   whose input handlers are tried in a fixed order (ignore, manual refresh,
 *   ESC, staged commands, pagination/help, filtering, staging, execute).
 *
 * @param operation         Target action ("mount", "umount", "cp", "mv", "rm",
 *                          or "write2usb"). Determines list source, colour
 *                          scheme, and dispatch.
 * @param isAtISOList       Set to @c true while the ISO list is displayed
 *                          (non-unmount only); managed by the RAII guards
 *                          during operation execution. Also gates
 *                          watcher-thread refresh requests.
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

    using select_common::LoopAction;

    // --- RAII: keybindings are automatically reset on any exit path ---
    ReadlineKeybindingGuard keybindingGuard;

    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);

    static std::vector<std::string> isoDirs;
    isoDirs.reserve(1000);

    if (!refreshState) refreshState = std::make_shared<RefreshState>();

    iso_select::SelectSession s(operation, isAtISOList, backgroundThreads, refreshState,
                                isoDirs, keybindingGuard);

    pending::IsoPendingScope pendingScope(s.isUnmount
        ? static_cast<const std::vector<std::string>*>(&isoDirs)
        : &GlobalState::globalIsoFileList);

    // --- RAII: isAtISOList state automatically managed ---
    IsoListStateGuard isoListGuard(isAtISOList, [&s]() { return s.isUnmount; });

    while (true) {
        s.beginIteration();

        if (!s.showListOrRewind()) break;

        s.startWatcherIfNeeded();

        const ReadlineAndPromptTheme pt = getPromptTheme();
        s.bindPromptKeys();
        const std::string prompt = s.buildPrompt(pt);

        armPendingInputRestore();
        RlLine rawInput = readlineWithResize(prompt, [&s] { s.repaintList(); });
        rl_startup_hook = nullptr;

        if (!rawInput) {
            s.onEndOfInput();
            break;
        }

        const std::string inputString(rawInput.get());

        const LoopAction action = s.dispatch(inputString);
        if (action == LoopAction::Break)  break;
        if (action == LoopAction::Return) return;
    }
    // Keybinding guard destructor automatically calls reset_custom_keybindings_forSelect().
}
