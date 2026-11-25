// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdio.h>
#include <readline/history.h>
#include <readline/readline.h>

import std;
import isocmd.core.lists;
import isocmd.core.sharedRefreshState;
import isocmd.core.uiState;
import isocmd.filter.filter;
import isocmd.input.history;
import isocmd.input.readline;
import isocmd.select.select;
import isocmd.ui.display.display;
import isocmd.ui.display.printList;
import isocmd.ui.display.readlineResize;
import isocmd.ui.display.stringListView;
import isocmd.ui.display.themes;

// ─── Constants ───────────────────────────────────────────────────────────────

namespace AnsiEscape {

    // From the row below a prompt back onto the prompt row, cleared.
    constexpr const char* REWIND_TO_PROMPT_ROW = "\033[1A\r\033[K";

} // namespace AnsiEscape

// ─── Prompt-row Convention ───────────────────────────────────────────────────
//
// Every prompt in this file is a SINGLE line (no leading "\n"), so readline's
// own cursor bookkeeping never has to cope with a multi-line prompt.
//
// The filter prompt REPLACES the select prompt in place: it is drawn on the
// select prompt's row, under the blank separator row that is already there.
//
//  - Entry: the caller's prompt was submitted (Enter or a hotkey, which now
//    both echo a newline), so the cursor is one row below the prompt row.
//    runFilterLoop rewinds onto that row once, then calls readline.
//  - Every exit (success, Esc, Ctrl+D) leaves the cursor one row below the
//    filter prompt row, so the caller needs no fix-up and rewinds once.
//  - A failed commit rewinds onto the prompt row and prompts again.
//  - applyFilterCore does no cursor movement; runFilterLoop owns all of it.
//  - The preview renderer prints its blank separator row after repainting.
//  - The resize path delegates live-preview repainting to filterPreview.cpp.

// ─── History Helpers ─────────────────────────────────────────────────────────

/**
 * @brief Saves a search query to readline history.
 *
 * Sets @p filterHistory to true, optionally loads history first, appends the
 * query, persists it, and clears the in-memory history list.
 *
 * @param query The query string to save.
 * @param filterHistory Reference to the filter-history flag.
 * @param alreadyLoaded If true, skips loadHistory (caller loaded before prompt).
 */
static void saveQueryToHistory(
    const std::string& query,
    bool& filterHistory,
    bool alreadyLoaded = false)
{
    filterHistory = true;

    if (!alreadyLoaded)
        loadHistory(filterHistory);

    add_history(query.c_str());
    saveHistory(filterHistory);
    clear_history();
}

// ─── Resize Handling ─────────────────────────────────────────────────────────

/**
 * @brief Repaints the display in response to a terminal resize.
 *
 * Delegates live-preview repainting to the preview implementation. If live
 * preview is unavailable, falls back to the normal list renderer or the
 * caller-provided repaint callback.
 *
 * Fallback order:
 *   1. Live preview repaint (repaintLivePreviewOnResizeIfConfigured).
 *   2. Full printList() render when @c ctx has a listType and the pending
 *      selection wiring (pendingIndices / hasPendingProcess / state) is set;
 *      renders @p previewSource with showPendingIndices=true and clearFirst.
 *   3. @c ctx.repaintList callback, if provided. The Readline line buffer and
 *      cursor are saved/restored around the callback so the user's in-progress
 *      input survives the repaint.
 *
 * @param ctx Current filter context.
 * @param previewSource Source list displayed by the current operation.
 */
static void repaintOnResize(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource)
{
    if (repaintLivePreviewOnResizeIfConfigured())
        return;

    if (ctx.pendingIndices &&
        ctx.hasPendingProcess &&
        ctx.state &&
        !ctx.listType.empty()) {
        size_t page = ctx.currentPage;

        printList(
            StringListView(previewSource),
            ctx.listType,
            ctx.listSubType,
            PrintListContext{
                .pendingIndices = *ctx.pendingIndices,
                .hasPendingProcess = *ctx.hasPendingProcess,
                .currentPage = page,
                .state = ctx.state,
                .clearFirst = true,
                .showPendingIndices = true,
                .showSourceIndices = false
            });

    } else if (ctx.repaintList) {
        const std::string savedLine(
            rl_line_buffer ? rl_line_buffer : "");

        const int savedPoint = rl_point;

        ctx.repaintList();

        rl_replace_line(savedLine.c_str(), 1);
        rl_point = std::min(savedPoint, rl_end);
    }
}

// ─── Cancellation ────────────────────────────────────────────────────────────

/**
 * @brief Handles prompt cancellation via EOF (Ctrl+D) or Escape.
 *
 * Emits a newline when @p raw is null (Ctrl+D), marks the screen for clearing
 * when a live-preview frame was painted during the session, and clears
 * Readline history.
 *
 * @note The newline is emitted only for EOF (null @p raw), not for Escape;
 *       Escape already leaves the cursor one row below the prompt.
 *
 * @param raw Raw readline result.
 * @param ctx Current filter context.
 */
static void handleFilterCancel(
    const RlLine& raw,
    FilterContext& ctx)
{
    if (!raw)
        std::cout << "\n";

    if (livePreviewWasRepainted())
        ctx.needsClrScrn = true;

    clear_history();
}

// ─── Interactive Filter Driver ────────────────────────────────────────────────

/**
 * @brief Runs the interactive filter prompt and commits a query on Enter.
 *
 * Manages the Readline preview session, history, resize callback, cancellation,
 * and filter-commit lifecycle.
 *
 * Loop contract: on each iteration, resetLivePreviewIteration() primes a fresh
 * preview session, readlineWithResize() reads one line, and
 * deactivateLivePreview() ends the session. A null/empty readline result or a
 * leading Escape (0x1B) cancels via handleFilterCancel(). Otherwise the query
 * is committed by applyFilterCore(); on success the query is saved to history
 * and @p onSuccess runs; on failure the prompt row is rewound and the loop
 * repeats. Readline history is loaded once, before the loop, so the
 * alreadyLoaded=true save path is used.
 *
 * @note @p onEmptyInput is currently resolved and cast to void but not
 *       otherwise used inside this function; empty-input handling is owned by
 *       the caller via applyFilterCore/callbacks.
 *
 * @param promptText Single-line prompt displayed by Readline.
 * @param ctx Mutable filter context.
 * @param onSuccess Callback invoked after a successful filter commit.
 * @param onEmptyInput Optional callback associated with empty-input handling.
 */
static void runFilterLoop(
    const std::string& promptText,
    FilterContext& ctx,
    const std::function<void()>& onSuccess,
    const std::function<void()>& onEmptyInput = nullptr)
{
    auto tryFilter = [&](const std::string& query) -> bool {
        return applyFilterCore(query, ctx);
    };

    auto defaultEmptyInput = [&]() {
        ctx.needsClrScrn = false;
    };

    const auto& handleEmpty =
        onEmptyInput ? onEmptyInput : defaultEmptyInput;

    (void)handleEmpty;

    std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;

    ctx.filterHistory = true;
    loadHistory(ctx.filterHistory);

    // Installs the preview Readline hook and restores it on scope exit.
    LivePreviewReadlineGuard livePreviewGuard;

    const std::vector<std::string>& previewSource =
        ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

    configureLivePreview(ctx, previewSource);

    const auto resizeRepaint = [&]() {
        repaintOnResize(ctx, previewSource);
    };

    while (true) {
        resetLivePreviewIteration();

        RlLine raw = readlineWithResize(promptText, resizeRepaint);

        deactivateLivePreview();

        if (!raw || raw.get()[0] == 27) {
            handleFilterCancel(raw, ctx);
            break;
        }

        std::string query(raw.get());

        if (tryFilter(query)) {
            saveQueryToHistory(query, ctx.filterHistory, true);
            onSuccess();
            break;
        }

        std::cout << AnsiEscape::REWIND_TO_PROMPT_ROW;
    }
}

// ─── Shared Filter Workflow ──────────────────────────────────────────────────

/**
 * @brief Core implementation shared by all filter entry points.
 *
 * Configures Readline, constructs the filter context, and runs the interactive
 * filter workflow for the requested operation.
 *
 * Only the literal @c "/" input is treated as a filter request; any other
 * input returns false without side effects. On the filter path, sets
 * @c g_suppressPendingRefresh for the duration (via a scope guard that clears
 * it on exit), restores PageUp/PageDown to history navigation, resets custom
 * select keybindings, and binds Escape to exit_handler before running the
 * loop. The filter prompt is themed via getFilterTheme("", false).
 *
 * @param inputString Original command input.
 * @param cfg Configuration describing the filter operation.
 *
 * @return True when the input was handled as a filter request.
 */
static bool runSharedFilterFlow(
    const std::string& inputString,
    const FilterCallConfig& cfg)
{
    if (inputString != "/")
        return false;

    GlobalState::g_suppressPendingRefresh.store(true);

    std::shared_ptr<void> suppressGuard(
        nullptr,
        [](void*) {
            GlobalState::g_suppressPendingRefresh.store(false);
        });

    rl_bind_keyseq(
        "\x1b[5~",
        rl_named_function("previous-history"));

    rl_bind_keyseq(
        "\x1b[6~",
        rl_named_function("next-history"));

    reset_custom_keybindingsForSelect();

    rl_bind_keyseq("\x1b", exit_handler);

    const ReadlineAndPromptTheme ft = getFilterTheme("", false);

    std::string prompt(ft.filter);
    prompt.append("Filter: ").append(ft.reset);

    FilterContext ctx{
        *cfg.files,
        *cfg.isFiltered,
        *cfg.needsClrScrn,
        *cfg.filterHistory,
        *cfg.currentPage
    };

    if (cfg.sourceOverride) {
        ctx.sourceOverride = cfg.sourceOverride;
        ctx.isUnmount = cfg.isUnmount;
        ctx.toggleFullListUmount = cfg.toggleFullList;
    }

    ctx.listType = cfg.listType;
    ctx.listSubType = cfg.listSubType;
    ctx.pendingIndices = cfg.pendingIndices;
    ctx.hasPendingProcess = cfg.hasPendingProcess;
    ctx.state = cfg.state;
    ctx.repaintList = cfg.repaintList;

    auto onEmptyInput = [&]() {
        *cfg.needsClrScrn = *cfg.isFiltered;
    };

    auto onSort = [&]() {
        if (cfg.need2Sort)
            *cfg.need2Sort = true;
    };

    runFilterLoop(prompt, ctx, onSort, onEmptyInput);

    return true;
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * @brief Filter entry point for ISO file operations (mount, unmount, etc.).
 *
 * Selects the appropriate source list and configures the shared filter
 * workflow with the ISO operation's rendering and state information.
 *
 * Base source selection: the original list is ALWAYS used as the search
 * source — mounted ISO dirs for unmount, or the global ISO list for all other
 * operations — so each committed filter starts fresh instead of narrowing the
 * previous result. The already-filtered list is only the destination
 * (@c filteredFiles). When @c ctx.state is present, rendering is wired as
 * "MOUNTED_ISOS" (unmount) or "ISO_FILES" (other) with the state's
 * pending-selection fields.
 *
 * @param inputString Original command input.
 * @param filteredFiles Destination filtered list.
 * @param operation Operation name.
 * @param operationColor Operation display color.
 * @param isoDirs Mounted ISO directories.
 * @param isUnmount Whether this is an unmount operation.
 * @param ctx Optional caller-owned filter state and callbacks.
 *
 * @return True when the input was handled as a filter request.
 */
bool handleFilteringForISO(
    const std::string& inputString,
    std::vector<std::string>& filteredFiles,
    const std::string& operation,
    const std::string& operationColor,
    const std::vector<std::string>& isoDirs,
    bool isUnmount,
    FilterEntryContext ctx)
{
    // Always search the original list, never the previous filter's result.
    const std::vector<std::string>& baseSource =
        isUnmount ? isoDirs : GlobalState::globalIsoFileList;

    FilterCallConfig cfg{
        .files = &filteredFiles,
        .sourceOverride = &baseSource,
        .operation = operation,
        .operationColor = operationColor,
        .isFiltered = ctx.isFiltered,
        .needsClrScrn = ctx.needsClrScrn,
        .filterHistory = ctx.filterHistory,
        .currentPage = ctx.currentPage,
        .isUnmount = isUnmount,
        .toggleFullList = displayConfig::toggleFullListUmount
    };

    if (ctx.state) {
        cfg.listType = isUnmount ? "MOUNTED_ISOS" : "ISO_FILES";
        cfg.listSubType = isUnmount ? "" : ctx.state->listSubtype;
        cfg.pendingIndices = &ctx.state->pendingIndices;
        cfg.hasPendingProcess = &ctx.state->hasPendingProcess;
        cfg.state = ctx.state;
    }

    cfg.repaintList = std::move(ctx.repaintList);

    return runSharedFilterFlow(inputString, cfg);
}

/**
 * @brief Filter entry point for convert-to-ISO operations.
 *
 * Configures the shared filter workflow with the image-conversion list
 * rendering context and optional caller-owned state.
 *
 * Filtering is single-level: every prompt filters from @p sourceFiles (the
 * original unfiltered image list), never from @p files (which may already be
 * the current filtered view). A second filter therefore replaces the previous
 * result instead of narrowing it.
 *
 * When @c ctx.state is present, rendering is wired as "IMAGE_FILES" with
 * sub-type "convert2iso". The operation color is fixed to
 * UI::Palette::Orange. The return value of runSharedFilterFlow is discarded
 * (this entry point is void).
 *
 * @param inputString Original command input.
 * @param files Image files to filter (the current display list).
 * @param sourceFiles Original unfiltered image list used as the filter
 *        corpus on every prompt.
 * @param operation Operation name.
 * @param ctx Optional caller-owned filter state and callbacks.
 */
void handleFilteringConvert2ISO(
    const std::string& inputString,
    std::vector<std::string>& files,
    const std::vector<std::string>& sourceFiles,
    const std::string& operation,
    FilterEntryContext ctx)
{
    FilterCallConfig cfg{
        .files = &files,
        .sourceOverride = &sourceFiles,
        .operation = operation,
        .operationColor = UI::Palette::Orange,
        .isFiltered = ctx.isFiltered,
        .needsClrScrn = ctx.needsClrScrn,
        .filterHistory = ctx.filterHistory,
        .need2Sort = ctx.need2Sort,
        .currentPage = ctx.currentPage
    };

    if (ctx.state) {
        cfg.listType = "IMAGE_FILES";
        cfg.listSubType = "convert2iso";
        cfg.pendingIndices = ctx.pendingIndices;
        cfg.hasPendingProcess = ctx.hasPendingProcess;
        cfg.state = ctx.state;
    }

    cfg.repaintList = std::move(ctx.repaintList);

    runSharedFilterFlow(inputString, cfg);
}
