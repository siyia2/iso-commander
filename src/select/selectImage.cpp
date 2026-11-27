// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>

import std;
import isocmd.input.lineEditor;
import isocmd.core.cancellation;
import isocmd.core.lists;
import isocmd.core.uiState;
import isocmd.filter.filter;
import isocmd.input.inputHandling;
import isocmd.input.promptCompletion;
import isocmd.select.select;
import isocmd.select.selectShared;
import isocmd.ui.display.lineEditorResize;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;

using namespace select_shared;

namespace {

	// ─── Image select screen ─────────────────────────────────────────────────────
	namespace image_select {

		using select_common::LoopAction;
		using select_common::isFilterCommand;

		/**
		 * @brief Maps a file type key to its conversion operation name.
		 *
		 * @param fileType File type key ("bin", "mdf", "nrg", "chd", "daa").
		 * @return Conversion operation name ("ccd2iso", "mdf2iso", "nrg2iso",
		 *         "chd2iso", "daa2iso"), or an empty string for an unknown type.
		 */
		std::string operationForFileType(const std::string& fileType) {

			if (fileType == "bin") return "ccd2iso";
			if (fileType == "mdf") return "mdf2iso";
			if (fileType == "nrg") return "nrg2iso";
			if (fileType == "chd") return "chd2iso";
			if (fileType == "daa") return "daa2iso";
			return "";

		}

		/**
		 * @brief Returns the RAM cache backing a file type's unfiltered list.
		 *
		 * Used by ESC to restore the unfiltered list for the active file type,
		 * and as the filter corpus so every prompt re-filters from source.
		 *
		 * @param fileType File type key ("bin", "mdf", "nrg", "chd", "daa").
		 * @return Pointer to the matching cache, or @c nullptr for an unknown
		 *         type.
		 */
		const std::vector<std::string>* imageCacheFor(const std::string& fileType) {

			if (fileType == "bin") return &GlobalState::binImgFilesCache;
			if (fileType == "mdf") return &GlobalState::mdfMdsFilesCache;
			if (fileType == "nrg") return &GlobalState::nrgFilesCache;
			if (fileType == "chd") return &GlobalState::chdFilesCache;
			if (fileType == "daa") return &GlobalState::daaGbiFilesCache;
			return nullptr;

		}

		/**
		 * @brief Per-session state and behavior for the interactive image select
		 *        screen.
		 *
		 * Owns every mutable field needed by one invocation of
		 * selectForImageFiles(): the active file list, filter state,
		 * pagination, staged selection (paths plus the display-projected
		 * indices), and the mode flags that pick the conversion operation.
		 * Exposes the loop's input handlers as member functions; the
		 * dispatcher tries them in a fixed order (ESC, staged commands,
		 * ignored input, pagination/help, "P", filtering, staging, then
		 * direct conversion).
		 */
		struct ImageSession {
			const std::string&              fileType;
			std::vector<std::string>&       files;
			bool&                           list;
			std::shared_ptr<RefreshState>   state;
			LineEditorKeybindingGuard&        keybindingGuard;

			const std::string               operation;
			const bool                      isMdf;
			const bool                      isNrg;
			const bool                      isChd;
			const bool                      isDaa;

			// The unfiltered list for this file type (what ESC restores). While a
			// filter is active, source indices ("n^") refer to this list, and
			// the filter prompt searches this list on every commit.
			const std::vector<std::string>* const cache;

			std::vector<std::string> pendingIndices;   // display projection only
			std::vector<std::string> pendingPaths;     // the real staged selection
			bool   hasPendingProcess = false;

			std::size_t currentPage  = 0;
			std::size_t originalPage = 0;

			bool isFiltered    = false;
			bool needsClrScrn  = true;
			bool filterHistory = false;
			bool need2Sort     = true;
			bool verbose       = false;   // reset at the top of every iteration

			/**
			 * @brief Binds per-session state to the caller's file list and
			 *        derives the mode flags from @p type.
			 *
			 * Sets @c operation to the mapped conversion name and the
			 * @c isMdf/@c isNrg/@c isChd/@c isDaa flags from @p type;
			 * resolves the backing cache pointer used for ESC restores and as
			 * the filter corpus.
			 *
			 * @param type         File type key ("bin", "mdf", "nrg", "chd",
			 *                     "daa").
			 * @param fileList     The active file list to select from; held by
			 *                     reference and updated in place.
			 * @param listFlag     The caller's list-mode flag, held by
			 *                     reference.
			 * @param refreshState Shared refresh state forwarded to
			 *                     filtering/display helpers.
			 * @param guard        RAII keybinding guard owned by the caller.
			 */
			ImageSession(const std::string& type,
			             std::vector<std::string>& fileList,
			             bool& listFlag,
			             std::shared_ptr<RefreshState> refreshState,
			             LineEditorKeybindingGuard& guard)
			    : fileType(type),
			      files(fileList),
			      list(listFlag),
			      state(std::move(refreshState)),
			      keybindingGuard(guard),
			      operation(operationForFileType(type)),
			      isMdf(type == "mdf"),
			      isNrg(type == "nrg"),
			      isChd(type == "chd"),
			      isDaa(type == "daa"),
			      cache(imageCacheFor(type)) {}

			// ----- helpers ---------------------------------------------------------

			/**
			 * @brief Returns the list that "n^" source indices resolve against.
			 *
			 * While a filter is active and a backing cache exists, returns the
			 * unfiltered cache; otherwise returns @c files itself.
			 *
			 * @return Const reference to the source list.
			 */
			const std::vector<std::string>& sourceList() const {

				return (isFiltered && cache) ? *cache : files;

			}

			/**
			 * @brief Returns the original unfiltered list used as the filter
			 *        corpus.
			 *
			 * Prefers the backing RAM cache (the pre-filter list for this file
			 * type) so every filter prompt re-filters from source rather than
			 * narrowing the previous result. Falls back to @c files when no
			 * cache exists (defensive; unknown file type).
			 *
			 * @return Const reference to the unfiltered source list.
			 */
			const std::vector<std::string>& filterSourceList() const {

				return cache ? *cache : files;

			}

			/**
			 * @brief Recomputes the "Pending Indices" display from the staged
			 *        paths.
			 *
			 * Resolves @c pendingPaths against @c sourceList() and compresses
			 * the resulting indices into ranges.
			 */
			void syncPendingDisplay() {

				pendingIndices = pending::compressRuns(
				    pending::resolveIndices(pendingPaths, sourceList(), false));
				hasPendingProcess = !pendingIndices.empty();

			}

			/**
			 * @brief Restores the unfiltered list for the active file type.
			 *
			 * Copies the backing cache (if any) back into @c files, drops the
			 * filter stack, restores @c currentPage from @c originalPage, and
			 * clears @c need2Sort. Shares behavior with the ESC handler.
			 */
			void restoreUnfilteredList() {

				if (cache) files = *cache;
				isFiltered = false;
				filteringStack.clear();
				currentPage = originalPage;
				need2Sort = false;

			}

			/**
			 * @brief Repaints the list on terminal resize.
			 *
			 * Runs on the Line editor thread (@c editorEventHook) and uses the same
			 * display step as the main loop. A throwaway sort flag keeps a
			 * resize from re-sorting the list.
			 */
			void repaintList() {

				bool sortNow = false;
				syncPendingDisplay();
				loadAndDisplayImageFiles(files, fileType, sortNow, isFiltered, list,
				                         pendingIndices, hasPendingProcess, currentPage,
				                         state);

			}

			/**
			 * @brief Prints the verbose results screen for the last conversion
			 *        batch.
			 *
			 * Uses layout mode 3 in verbosePrint, which renders the conversion
			 * summary with the appropriate set ordering.
			 */
			void printVerboseResults() const {

				verbosePrint(verboseSets.uniqueErrorTokenMessages,
				             verboseSets.operationCompleted,
				             verboseSets.operationSkipped,
				             verboseSets.operationFailed, 3);

			}

			/**
			 * @brief Runs the convert2iso pipeline on a selection.
			 *
			 * @param input  Raw user input containing indices or ranges.
			 * @param target The list the indices resolve against.
			 */
			void convert(const std::string& input, std::vector<std::string>& target) {

				processInputForConversions(input, target, isMdf, isNrg, isChd, isDaa, verbose);

			}

			// ----- per-iteration steps ---------------------------------------------

			/**
			 * @brief Resets per-iteration state at the top of the loop.
			 *
			 * Clears verbose sets, re-enables Ctrl+D, reinstalls signal
			 * handlers, resets the cancellation flag and @c verbose, snapshots
			 * @c currentPage into @c originalPage while unfiltered, and clears
			 * line editor history.
			 */
			void beginIteration() {

				clearGlobalVerboseSets();
				enable_ctrl_d();
				setupSignalHandlerCancellations();

				GlobalState::g_operationCancelled.store(false);
				verbose = false;

				if (!isFiltered) originalPage = currentPage;

				editorClearHistory();

			}

			/**
			 * @brief Puts the cursor on the prompt row and prepares for the
			 *        next editLine().
			 *
			 * Redraw path (@c needsClrScrn is true): recompute the pending
			 * display, draw the list, and print one blank separator row.
			 * No-redraw path: a single @c term::rewindToPromptRow() call
			 * returns to the prompt row below the previous prompt. See the
			 * prompt-row cursor invariant documented at the top of the select
			 * screen.
			 */
			void showListOrRewind() {

				if (!needsClrScrn) {
					term::rewindToPromptRow();
					return;
				}
				syncPendingDisplay();
				loadAndDisplayImageFiles(files, fileType, need2Sort, isFiltered, list,
				                         pendingIndices, hasPendingProcess, currentPage,
				                         state);
				std::cout << "\n";

			}

			/**
			 * @brief Installs select-screen keybindings and unbinds keys that
			 *        are unused for this prompt.
			 *
			 * Rebinds "R" to @c editorInsertKey since conversion prompts have no
			 * manual refresh.
			 */
			void bindPromptKeys() {

				setup_custom_keybindingsForSelect();

				// Unused in convert2iso prompts.
				editorBindKeySequence("R", editorInsertKey);

			}

			/**
			 * @brief Builds the colored prompt string for the current operation.
			 *
			 * @param pt Theme tokens for primary/highlight text and reset.
			 * @return Formatted prompt string for @c editLineWithResize().
			 */
			std::string buildPrompt(const LineEditorAndPromptTheme& pt) const {

				std::string prompt(UI::Palette::PromptIndexGreen);
				prompt
				    .append("IDX")
				    .append(pt.primary)
				    .append(" ↵ for ")
				    .append(pt.highlight)
				    .append(operation)
				    .append(pt.primary)
				    .append(", ? for help: ")
				    .append(pt.reset);
				return prompt;

			}

			/**
			 * @brief Handles Ctrl-D / EOF on the prompt.
			 *
			 * If a filter is active, marks the screen for redraw, drops the
			 * filter stack, restores @c currentPage from @c originalPage, and
			 * clears @c need2Sort. The caller then leaves the loop.
			 */
			void onEndOfInput() {

				if (!isFiltered) return;
				needsClrScrn = true;
				isFiltered = false;
				filteringStack.clear();
				currentPage = originalPage;
				need2Sort = false;

			}

			// ----- input handlers (tried in this order) ----------------------------

			/**
			 * @brief Handles the ESC key ('\x1b').
			 *
			 * Clears the scroll buffer. With an active filter: restores the
			 * unfiltered list via @c restoreUnfilteredList() and requests a
			 * redraw. Without a filter: resets pagination and returns to the
			 * previous menu.
			 *
			 * @param input Raw user input.
			 * @return @c Continue when a filter was cleared, @c Return when
			 *         leaving the screen, @c Proceed otherwise.
			 */
			LoopAction escape(const std::string& input) {

				if (input != "\x1b") return LoopAction::Proceed;
				clearScrollBuffer();
				if (isFiltered) {
					// Restore original unfiltered file list.
					restoreUnfilteredList();
					needsClrScrn = true;
					return LoopAction::Continue;
				}
				currentPage = 0;
				need2Sort = false;
				return LoopAction::Return;

			}

			/**
			 * @brief Clears all staged-selection state.
			 *
			 * Empties the staged path set, the display index vector, and the
			 * pending-process flag.
			 */
			void clearPendingSelection() {

				pendingPaths.clear();
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

				if (input == "P" && pendingPaths.empty()) {
					hasPendingProcess = false;
					needsClrScrn = false;
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
			 * @brief Swallows empty, comment-like, and suppressed input.
			 *
			 * Ignores empty lines, lines starting with ';', and lines
			 * containing ";;". Marks the screen as not needing a redraw and
			 * returns @c LoopAction::Continue for ignored input; otherwise
			 * returns @c LoopAction::Proceed.
			 *
			 * @param input Raw user input.
			 * @return @c Continue when the input was ignored, @c Proceed
			 *         otherwise.
			 */
			LoopAction ignoreInput(const std::string& input) {

				const bool ignored = input.empty() ||
				                     input[0] == ';' ||
				                     input.find(";;") != std::string::npos;
				if (!ignored) return LoopAction::Proceed;
				needsClrScrn = false;
				return LoopAction::Continue;

			}

			/**
			 * @brief Handles pagination and help commands.
			 *
			 * Computes the page count from the current @c files vector (zero
			 * when @c GlobalState::ITEMS_PER_PAGE is zero) and delegates to
			 * @c processPaginationHelpAndDisplay().
			 *
			 * @param input Raw user input.
			 * @return @c Continue when the command was handled, @c Proceed
			 *         otherwise.
			 */
			LoopAction pagination(const std::string& input) {

				std::size_t totalPages = (GlobalState::ITEMS_PER_PAGE != 0)
				    ? ((files.size() + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE)
				    : 0;

				const bool handled = processPaginationHelpAndDisplay(
				    input, totalPages, currentPage, isFiltered, needsClrScrn,
				    operation, need2Sort, nullptr, files);
				return handled ? LoopAction::Continue : LoopAction::Proceed;

			}

			/**
			 * @brief Executes the staged selection ("P").
			 *
			 * The staged selection holds source indices of the full list, so
			 * it runs against it: while a filter is active, a copy of the
			 * backing cache is passed to the converter and the active filter
			 * (and @c files) is left untouched. Resolves vanished paths before
			 * converting; verbose output is printed afterward when enabled.
			 *
			 * @param input Raw user input.
			 * @return @c Continue when "P" was handled (even if nothing was
			 *         staged), @c Proceed otherwise.
			 */
			LoopAction runStaged(const std::string& input) {

				if (input != "P") return LoopAction::Proceed;

				const bool useCache = isFiltered && cache;
				std::vector<std::string> fullList;
				if (useCache) fullList = *cache;
				std::vector<std::string>& target = useCache ? fullList : files;

				const std::string batch = pending::joinTokens(pending::compressRuns(
				    pending::resolveIndices(pendingPaths, target, true)));   // drops vanished files
				needsClrScrn = true;

				if (batch.empty()) {
					pendingIndices.clear();
					hasPendingProcess = false;
					return LoopAction::Continue;
				}

				convert(batch, target);
				keybindingGuard.restore();
				clearPendingSelection();
				if (verbose) printVerboseResults();
				return LoopAction::Continue;

			}

			/**
			 * @brief Handles filter-command input.
			 *
			 * Delegates to @c handleFilteringConvert2ISO() with a context that
			 * wires the filter state into this session, passing the original
			 * unfiltered image list (the backing RAM cache, or @c files when no
			 * cache exists) as the filter corpus. Filtering is single-level and
			 * non-incremental: each Enter re-filters from that source rather
			 * than narrowing the previous result. Restores keybindings on
			 * return.
			 *
			 * @param input Raw user input.
			 * @return @c Continue when the command was handled, @c Proceed
			 *         otherwise.
			 */
			LoopAction filtering(const std::string& input) {

				if (!isFilterCommand(input)) return LoopAction::Proceed;
				FilterEntryContext ctx {
					.isFiltered        = &isFiltered,
					.needsClrScrn      = &needsClrScrn,
					.filterHistory     = &filterHistory,
					.currentPage       = &currentPage,
					.need2Sort         = &need2Sort,
					.pendingIndices    = &pendingIndices,
					.hasPendingProcess = &hasPendingProcess,
					.state             = state,
					.repaintList       = [this] { repaintList(); }
				};
				handleFilteringConvert2ISO(input, files, filterSourceList(), operation, ctx);
				keybindingGuard.restore();
				return LoopAction::Continue;

			}

			/**
			 * @brief Stages an index selection that contains ';'.
			 *
			 * Input containing ';' is always treated as staging syntax: if it
			 * is not accepted the iteration simply ends. Bare numbers resolve
			 * against @c files (the current view); "n^" numbers resolve against
			 * @c sourceList() (the unfiltered list). Delegates to
			 * @c pending::stagePending().
			 *
			 * @param input Raw user input.
			 * @return @c Continue when the input was handled as staging syntax,
			 *         @c Proceed otherwise.
			 */
			LoopAction stageSelection(const std::string& input) {

				if (input.find(';') == std::string::npos) return LoopAction::Proceed;
				pending::stagePending(input, pendingPaths, hasPendingProcess, needsClrScrn,
				                      files, sourceList());
				return LoopAction::Continue;

			}

			/**
			 * @brief Executes a direct index selection.
			 *
			 * Runs the converter against @c files, requests a redraw, prints
			 * verbose results when enabled, restores keybindings, and finally
			 * discards the staged selection via @c clearPendingSelection()
			 * (a direct run invalidates anything staged earlier, even if the
			 * conversion failed or was cancelled).
			 *
			 * @param input Raw user input containing indices or ranges.
			 */
			void convertDirect(const std::string& input) {

				convert(input, files);
				needsClrScrn = true;
				if (verbose) printVerboseResults();
				keybindingGuard.restore();
				clearPendingSelection();

			}

			// ----- dispatcher ------------------------------------------------------

			/**
			 * @brief Tries each input handler in order and executes unhandled
			 *        input as a direct conversion.
			 *
			 * Handler order: ESC, staged commands, ignored input,
			 * pagination/help, "P", filtering, staging. If every handler
			 * returns @c Proceed, executes @c convertDirect().
			 *
			 * @param input Raw user input.
			 * @return The first non-@c Proceed action returned by a handler;
			 *         otherwise @c LoopAction::Continue after running the
			 *         conversion.
			 */
			LoopAction dispatch(const std::string& input) {

				using Handler = LoopAction (ImageSession::*)(const std::string&);
				static const Handler handlers[] = {
					&ImageSession::escape,
					&ImageSession::stagedCommands,
					&ImageSession::ignoreInput,
					&ImageSession::pagination,
					&ImageSession::runStaged,
					&ImageSession::filtering,
					&ImageSession::stageSelection,
				};

				for (Handler h : handlers) {
					const LoopAction action = (this->*h)(input);
					if (action != LoopAction::Proceed) return action;
				}

				convertDirect(input);
				return LoopAction::Continue;

			}
		};

	} // namespace image_select

} // namespace

// ---------------------------------------------------------------------------
// Global scope: public entry points
// ---------------------------------------------------------------------------

/**
 * @brief Runs the interactive image selection screen for ISO conversion.
 *
 * ImageSession uses the RAM cache for the requested image format. Filtering
 * always searches the full cache; Esc clears the filter or returns to the
 * menu. The screen does not support manual database refresh (R).
 *
 * Selections ending in ';' are staged as paths. Bare numbers select visible
 * rows; a '^' suffix selects source-list indices. P runs the staged paths
 * without clearing the filter. Direct conversion clears staged paths even
 * on failure or cancellation. Keybinding guards and resize callbacks keep
 * the terminal state consistent; see selectShared.cppm for cursor rules.
 */
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files,
                         bool& list, std::shared_ptr<RefreshState> state) {

	using select_common::LoopAction;

	// --- RAII: keybindings are automatically reset on any exit path ---
	LineEditorKeybindingGuard keybindingGuard;

	editorBindKey('\f', ignoreEditorKeybinding);
	editorBindKey('\t', ignoreEditorKeybinding);

	image_select::ImageSession s(fileType, files, list, state, keybindingGuard);

	while (true) {
		s.beginIteration();
		s.showListOrRewind();

		const LineEditorAndPromptTheme pt = getPromptTheme();
		s.bindPromptKeys();
		const std::string prompt = s.buildPrompt(pt);

		armPendingInputRestore();
		EditorLine rawInput = editLineWithResize(prompt, [&s] { s.repaintList(); });
		editorStartupHook = nullptr;

		if (!rawInput) {
			s.onEndOfInput();
			break;
		}

		const std::string inputString(*rawInput);

		const LoopAction action = s.dispatch(inputString);
		if (action == LoopAction::Break)  break;
		if (action == LoopAction::Return) return;
	}
	// Keybinding guard destructor automatically calls reset_custom_keybindingsForSelect().

}
