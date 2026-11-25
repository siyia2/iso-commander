// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>

import std;
import isocmd.app.main;
import isocmd.core.lists;
import isocmd.core.paths;
import isocmd.core.sharedRefreshState;
import isocmd.core.uiState;
import isocmd.filter.filter;
import isocmd.ui.display.display;
import isocmd.ui.display.printList;
import isocmd.ui.display.stringListView;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;
import isocmd.util.stringManipulation;

namespace fs = std::filesystem;

/// Clear screen + scrollback, home cursor, reset attributes. Emitted inside the
/// sync frame so the clear and the new content are presented atomically.
constexpr std::string_view kClearSeq = "\033[2J\033[3J\033[H\033[0m";

/// Rough per-row overhead (index digits, color escapes, separators) used to size
/// the buffer.
constexpr std::size_t kPerRowOverhead = 96;

/// Retained capacity (per list-type buffer) above which we release memory after a
/// render. The release happens only *after* the frame has been flushed, so a
/// large page never incurs a reallocation mid-write.
constexpr std::size_t kMaxRetainedCapacity = 4u << 20; // 4 MiB

namespace {

    /// Which list a frame is being rendered for. Each kind owns its own retained buffer.
    enum class ListKind : std::size_t { Iso, Image, Mounted, Count };

    /**
     * @brief Retrieves a thread-local retained string buffer specific to the given list kind.
     *
     * Each list kind owns its independent buffer, ensuring rendering one list type
     * never grows, shrinks, or reallocates the buffer used by another.
     *
     * @note Selection is by kind only. printList() maps ISO_FILES -> Iso,
     *       IMAGE_FILES -> Image, and *every other* listType (notably
     *       MOUNTED_ISOS) to Mounted. An unexpected listType therefore shares
     *       the Mounted buffer rather than getting one of its own.
     *
     * @param kind The category of the list (@c ListKind).
     * @return std::string& Reference to the retained thread-local buffer.
     */
    std::string& bufferFor(ListKind kind) {
        static thread_local std::array<std::string, static_cast<std::size_t>(ListKind::Count)> buffers;
        return buffers[static_cast<std::size_t>(kind)];
    }

    /**
     * @brief Computes the number of decimal digits required to represent a value.
     *
     * @param value Unsigned numeric value.
     * @return std::size_t The digit count (value 0 evaluates to 1 digit).
     */
    std::size_t digitCount(std::size_t value) noexcept {
        std::size_t d = 1;
        while (value >= 10) { value /= 10; ++d; }
        return d;
    }

    /**
     * @brief Formats an integer value into a stack buffer and appends it to the output string.
     *
     * Performs fast conversion without heap allocation or shared state tracking.
     *
     * @param out   Reference to the destination string buffer.
     * @param value The numeric value to append.
     */
    void appendNum(std::string& out, std::size_t value) {
        char buf[20];
        const auto res = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, static_cast<std::size_t>(res.ptr - buf));
    }

    /**
     * @brief Formats an integer value with right-alignment padding into the output string.
     *
     * @note @p width is a *minimum* only. A value whose digit count exceeds
     *       @p width is appended in full with no truncation.
     *
     * @param out   Reference to the destination string buffer.
     * @param value The numeric value to append.
     * @param width The minimum formatting column width.
     */
    void appendNumPadded(std::string& out, std::size_t value, std::size_t width) {
        char buf[20];
        const auto res = std::to_chars(buf, buf + sizeof(buf), value);
        const std::size_t len = static_cast<std::size_t>(res.ptr - buf);
        if (len < width) out.append(width - len, ' ');
        out.append(buf, len);
    }

    /**
     * @brief Prepares the background synchronization indicator text if a database import is active.
     *
     * Builds the "Syncing: NewISO -> Restructure" banner (plus an optional
     * "Filtering locked during sync" notice) that flushOutput() splices into
     * the frame at the header boundary. When no import is running, or the
     * list is not an ISO list, or the view has no filter results, returns an
     * empty string (the caller treats empty as "not syncing").
     *
     * In paginated mode the banner is prefixed with blank lines and, for the
     * filtering notice, a cursor-up (`\033[1A`) + erase-line (`\033[K`) pair so
     * the notice replaces the blank line above it rather than pushing the
     * header down. In non-paginated mode plain blank-line separators are used
     * instead.
     *
     * @param isIsoMode          True when rendering the ISO list.
     * @param noFilterResults    True when the view is empty due to filtering.
     * @param state              Shared refresh state; isImportRunning is sampled.
     * @param disablePagination  Selects the cursor-up vs. plain-separator layout.
     * @return std::string The styled sync banner, or an empty string if not syncing.
     */
    std::string prepareSyncLine(bool isIsoMode, bool noFilterResults, const std::shared_ptr<RefreshState>& state, bool disablePagination) {
        const bool wantSync = isIsoMode && !noFilterResults && !GlobalState::globalIsoFileList.empty();
        const bool maybeSyncing = wantSync && state->isImportRunning.load(std::memory_order_acquire);

        if (!maybeSyncing) return "";

        const bool historyOk = !isHistoryFileEmpty(GlobalState::historyFilePath)
                               && fs::is_regular_file(GlobalState::historyFilePath);
        std::string syncLine;
        syncLine.reserve(192);
        syncLine.append(UI::Palette::Dim);
        if (historyOk) {
            syncLine.append(disablePagination
                ? "[↻ Syncing: NewISO → Restructure]\n\n"
                : "\n\n[↻ Syncing: NewISO → Restructure]");
            if (GlobalState::g_filteringIndicator) {
                syncLine.append(disablePagination
                    ? "\033[1A\033[K[ℹ  Filtering locked during sync]\n\n"
                    : "\n[ℹ  Filtering locked during sync]");
            }
        } else {
            syncLine.append(disablePagination
                ? "[No FolderPath history — nothing to sync]\n\n"
                : "\n\n[No FolderPath history — nothing to sync]");
        }
        syncLine.append(UI::Palette::BoldReset);
        return syncLine;
    }

    /**
     * @brief Appends pagination header information to the output frame buffer.
     *
     * In paginated mode emits the "Page X/Y (Items A-B/N)" line followed by a
     * reset and a blank line. In non-paginated mode emits nothing.
     *
     * @return std::size_t Byte offset within @p output at which the sync banner
     *         (see prepareSyncLine) should later be spliced in. This is the
     *         position immediately after the header text (paginated) or
     *         immediately after the leading content (non-paginated); the
     *         caller must pass it to flushOutput().
     */
    size_t appendHeader(std::string& output, bool disablePagination, std::size_t effectivePage, std::size_t totalPages,
                        std::size_t startIndex, std::size_t endIndex, std::size_t totalItems, const PrintListTheme& c)
    {
        if (!disablePagination) {
            output.append(c.head).append("Page ");
            output.append(c.accent);
            appendNum(output, effectivePage + 1);
            output.append(c.head).append("/").append(c.num);
            appendNum(output, totalPages);
            output.append(c.head).append(" (Items (").append(c.accent);
            appendNum(output, startIndex + 1);
            output.append("-");
            appendNum(output, endIndex);
            output.append(c.head).append(")/").append(c.num);
            appendNum(output, totalItems);
            output.append(c.head).append(")");

            std::size_t syncInsertPos = output.size();
            output.append(UI::Palette::BoldReset).append("\n\n");
            return syncInsertPos;
        } else {
            std::size_t syncInsertPos = output.size();
            return syncInsertPos;
        }
    }

    /**
     * @brief Bundles filtering index mappings, collection sizes, and dynamic padding constraints.
     *
     * @c origIdx          Back-filter mapping: view index -> original (pre-filter)
     *                     index. Null when no filter is active.
     * @c origSize         Number of entries in @c origIdx (0 when null).
     * @c maxOrigDigits    Maximum digit count, over the current page slice, of the
     *                     *1-based* original index (i.e. digitCount(orig + 1)).
     *                     Used to right-align the "orig^" badge column.
     */
    struct PrintFilterContext {
        const std::vector<unsigned long>* origIdx;
        std::size_t origSize;
        std::size_t maxOrigDigits;
    };

    /**
     * @brief Bundles list mode flags, path layout toggles, sizing constraints, and color themes.
     *
     * @c isFileMode        True for ISO_FILES / IMAGE_FILES (use appendFileModeBody).
     * @c isIsoMode         True for ISO_FILES (affects filename coloring).
     * @c isMountedMode     True for MOUNTED_ISOS (use appendMountedModeBody).
     * @c showNamesOnly     Hide directory components in file mode.
     * @c showFullUmount    In mounted mode, include dir + hash in addition to the path.
     * @c usableCols        Terminal width minus one. When 0, row clipping is disabled.
     * @c colors            Theme colors for this render.
     */
    struct RowConfig {
        bool isFileMode;
        bool isIsoMode;
        bool isMountedMode;
        bool showNamesOnly;
        bool showFullUmount;
        std::size_t usableCols;
        const PrintListTheme& colors;
    };

    /**
     * @brief Appends the alternating sequence number and optional original-index badge to the row.
     *
     * @param rowIndex          Absolute (global) index into the list view, i.e. the
     *                          startIndex..endIndex loop counter. Used to look up
     *                          filterCtx.origIdx[rowIndex].
     * @param displayIndex      Absolute position in the displayed list, used for
     *                          numbering and alternating index colors.
     * @param maxDigits         Width of the sequence-number field.
     * @param showSourceIndices When true (and a filter is active and rowIndex is in
     *                          range), emit "orig^" badge padded to maxOrigDigits.
     */
    void appendRowIndexPrefix(
        std::string& output,
        std::size_t rowIndex,
        std::size_t displayIndex,
        std::size_t maxDigits,
        const PrintFilterContext& filterCtx,
        const RowConfig& cfg,
        bool showSourceIndices)
    {
        const std::string_view seqColor =
            (displayIndex % 2 == 0) ? cfg.colors.indexA : cfg.colors.indexB;

        output.append(seqColor);
        appendNumPadded(output, displayIndex + 1, maxDigits);

        // The filter mapping is indexed by the global row index, not by a
        // view-local offset.
        if (showSourceIndices && filterCtx.origIdx && rowIndex < filterCtx.origSize) {
            const std::size_t origVal =
                static_cast<std::size_t>((*filterCtx.origIdx)[rowIndex]) + 1;

            output.append(":").append(UI::Palette::BoldReset).append(cfg.colors.square);
            appendNum(output, origVal);
            output.append("^");

            const std::size_t pad = filterCtx.maxOrigDigits - digitCount(origVal);
            if (pad) output.append(pad, ' ');

            output.append(" ").append(UI::Palette::BoldReset);
        } else {
            output.append(". ").append(UI::Palette::BoldReset);
        }
    }

    /**
     * @brief Appends a file-mode entry with left-ellipsized directory clipping and filename coloring.
     *
     * @param rowStart  Byte offset in @p output where this row began. Used to
     *                  measure the already-emitted prefix (index + badge) so the
     *                  directory/filename budget can be computed against
     *                  cfg.usableCols.
     */
    void appendFileModeBody(std::string& output, std::size_t rowStart, const std::string& item,
                            const std::string& listSubType, const RowConfig& cfg)
    {
        auto [dir, fname] = extractDirectoryAndFilename(item, listSubType);
        const std::string_view dirView{dir};
        const std::string_view nameView{fname};

        std::string_view dirShown = dirView;
        bool dirCut = false;
        bool dirDrop = cfg.showNamesOnly;

        if (!cfg.showNamesOnly && cfg.usableCols > 0) {
            const std::size_t prefixW = visibleWidthFrom(output, rowStart);
            const std::size_t nameW = utf8Cols(nameView);
            if (prefixW + utf8Cols(dirView) + 1 + nameW > cfg.usableCols) {
                const std::size_t fixed = prefixW + 1 + nameW;
                const std::size_t budget = cfg.usableCols > fixed ? cfg.usableCols - fixed : 0;
                if (budget >= 2) {
                    dirShown = tailColumns(dirView, budget - 1);
                    dirCut = true;
                } else {
                    dirDrop = true;
                }
            }
        }

        if (!dirDrop) {
            output.append(cfg.colors.dir);
            if (dirCut) output.append("\u2026");
            output.append(dirShown).append(UI::Palette::BoldReset).append("/");
        }

        output.append(cfg.isIsoMode ? cfg.colors.iso : cfg.colors.img).append(nameView);
    }

    /**
     * @brief Appends a mounted-ISO entry, optionally including mount point and hash components.
     */
    void appendMountedModeBody(std::string& output, const std::string& item, const RowConfig& cfg)
    {
        auto [dirPart, pathPart, hashPart] = parseMountPointComponents(item);
        if (cfg.showFullUmount) {
            output.append(cfg.colors.mnt)
                  .append(dirPart).append(UI::Palette::Reset).append(UI::Palette::BoldReset)
                  .append(cfg.colors.iso).append(pathPart)
                  .append(cfg.colors.square).append(hashPart);
        } else {
            output.append(cfg.colors.iso).append(pathPart);
        }
    }

    /**
     * @brief Renders and clips a single item row into the output buffer with uniform alignment.
     *
     * @param showSourceIndices Forwarded to appendRowIndexPrefix; when false the
     *                          "orig^" badge is never emitted.
     */
    void appendItemRow(
        std::string& output,
        const StringListView& items,
        std::size_t rowIndex,
        std::size_t displayIndexOffset,
        std::size_t maxDigits,
        const PrintFilterContext& filterCtx,
        const std::string& listSubType,
        const RowConfig& cfg,
        bool showSourceIndices)
    {
        const std::size_t rowStart = output.size();
        const std::size_t displayIndex = rowIndex + displayIndexOffset;

        appendRowIndexPrefix(
            output,
            rowIndex,
            displayIndex,
            maxDigits,
            filterCtx,
            cfg,
            showSourceIndices);

        const std::string& item = items[rowIndex];

        if (cfg.isFileMode) {
            appendFileModeBody(output, rowStart, item, listSubType, cfg);
        } else if (cfg.isMountedMode) {
            appendMountedModeBody(output, item, cfg);
        }

        if (cfg.usableCols > 0) {
            fitRowToWidth(output, rowStart, cfg.usableCols);
        }

        output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
    }

    /**
     * @brief Appends pagination footers and staged pending process queues to the output frame buffer.
     *
     * The pagination footer ("[PgUp] Prev | [PgDn] Next | [g#] GoTo") is
     * suppressed entirely when @p disablePagination is true. The pending-index
     * block is emitted only when @p showPendingIndices, @p hasPendingProcess,
     * and the queue is non-empty; its indices are colored with c.img in image
     * mode and c.iso otherwise. When filtering is active, pending indices are
     * marked with '^'; ranges are enclosed in parentheses before the marker
     * (e.g. (1-4)^), while individual indices use the standard notation (e.g. 7^).
     */
    void appendFooterAndPending(std::string& output, bool disablePagination, std::size_t effectivePage,
                                std::size_t totalPages, bool showPendingIndices, bool hasPendingProcess,
                                const std::vector<std::string>& pendingIndices, bool isImgMode, const PrintListTheme& c)
    {
        if (!disablePagination) {
            output.append("\n").append(c.head);
            if (effectivePage > 0) output.append("[PgUp] Prev | ");
            if (effectivePage < totalPages - 1) output.append("[PgDn] Next | ");
            output.append("[g#] ↵ GoTo").append(UI::Palette::BoldReset).append("\n");
        }

        if (showPendingIndices && hasPendingProcess && !pendingIndices.empty()) {
            output.append("\n");
            output.append(c.bracketBg).append("Pending Indices [")
                  .append(c.procText).append("P")
                  .append(UI::Palette::BoldReset).append(c.bracketBg).append("]: ");

            output.append(!isImgMode ? c.iso : c.img);

            const bool isFiltered = !filteringStack.empty();

            for (std::size_t i = 0; i < pendingIndices.size(); ++i) {
                const std::string& index = pendingIndices[i];

                if (isFiltered) {
                    if (index.find('-') != std::string::npos) {
                        output.push_back('(');
                        output.append(index);
                        output.append(")^");
                    } else {
                        output.append(index).push_back('^');
                    }
                } else {
                    output.append(index);
                }

                if (i < pendingIndices.size() - 1) output.push_back(' ');
            }

            output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
        }
    }

    /**
     * @brief Configuration and display flags derived from the list type and global settings.
     *
     * Field meanings mirror RowConfig (isFileMode/isIsoMode/isMountedMode/
     * showNamesOnly/showFullUmount/usableCols/colors) with two additions:
     * @c isImgMode        True for IMAGE_FILES.
     * @c noFilterResults  True when the view is empty because a filter excluded
     *                     every item. ISO mode requires the global ISO list to be
     *                     non-empty; all other modes treat any empty view as
     *                     "no results".
     */
    struct ListConfig {
        bool isIsoMode;
        bool isImgMode;
        bool isMountedMode;
        bool isFileMode;
        bool showNamesOnly;
        bool showFullUmount;
        PrintListTheme colors;
        std::size_t usableCols;
        bool noFilterResults;
    };

    /**
     * @brief Evaluates the current list type, configuration settings, and terminal dimensions.
     *
     * noFilterResults is computed as:
     *   - ISO mode:     view empty AND the global ISO list is non-empty, or
     *   - non-ISO mode: view empty.
     */
    ListConfig getListConfig(const StringListView& items, const std::string& listType) {
        const bool isIsoMode      = (listType == "ISO_FILES");
        const bool isImgMode      = (listType == "IMAGE_FILES");
        const bool isMountedMode  = (listType == "MOUNTED_ISOS");
        const bool isFileMode     = (isIsoMode || isImgMode);
        const bool showNamesOnly  = displayConfig::toggleNamesOnly;
        const bool showFullUmount = displayConfig::toggleFullListUmount;
        const std::size_t termCols   = terminalColumns();
        const std::size_t usableCols = termCols > 1 ? termCols - 1 : 0;

        const bool noFilterResults =
            (items.empty() && !GlobalState::globalIsoFileList.empty() && isIsoMode) ||
            (items.empty() && !isIsoMode);

        return {isIsoMode, isImgMode, isMountedMode, isFileMode, showNamesOnly, showFullUmount, getListColors(), usableCols, noFilterResults};
    }

    /**
     * @brief Computed pagination boundaries and slice indices for the current frame.
     *
     * @c disablePagination True when ITEMS_PER_PAGE == 0 or the item count fits
     *                      in a single page.
     * @c totalPages        Effective page count (1 when pagination is disabled).
     * @c effectivePage     currentPage clamped to [0, totalPages - 1].
     * @c startIndex        First view index of the rendered slice.
     * @c endIndex          One-past-last view index of the rendered slice.
     * @c maxDigits         Digit count of endIndex; used to size the index column.
     */
    struct PaginationInfo {
        bool disablePagination;
        std::size_t totalPages;
        std::size_t effectivePage;
        std::size_t startIndex;
        std::size_t endIndex;
        std::size_t maxDigits;
    };

    /**
     * @brief Calculates pagination boundaries based on item count and global page constraints.
     */
    PaginationInfo computePagination(const StringListView& items, std::size_t currentPage) {
        const std::size_t totalItems = items.size();
        const bool disablePagination = (GlobalState::ITEMS_PER_PAGE == 0 || totalItems <= GlobalState::ITEMS_PER_PAGE);
        const std::size_t totalPages = disablePagination ? 1 : (totalItems + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;

        const std::size_t effectivePage = disablePagination ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
        const std::size_t startIndex = disablePagination ? 0 : (effectivePage * GlobalState::ITEMS_PER_PAGE);
        const std::size_t endIndex = disablePagination ? totalItems : std::min(startIndex + GlobalState::ITEMS_PER_PAGE, totalItems);
        const std::size_t maxDigits = digitCount(endIndex);

        return {disablePagination, totalPages, effectivePage, startIndex, endIndex, maxDigits};
    }

    /**
     * @brief Reserves buffer capacity and appends initial frame formatting.
     *
     * Emits, in order: the synchronized-output begin marker (when enabled), the
     * clear sequence (when @p clearFirst), a leading blank line, and — when
     * @p noFilterResults — the "No filter results" line. Capacity is reserved
     * up front from the per-row sizes of the *rendered slice* plus
     * kPerRowOverhead per row.
     */
    void reserveAndBuildOutput(std::string& output, std::size_t startIndex, std::size_t endIndex,
                               const StringListView& items, bool clearFirst, bool noFilterResults,
                               const PrintListTheme& c) {
        {
            std::size_t estimate = 1024;
            for (std::size_t i = startIndex; i < endIndex; ++i) {
                estimate += items[i].size() + kPerRowOverhead;
            }
            if (output.capacity() < estimate) output.reserve(estimate);
        }

        if (kUseSynchronizedOutput) output.append(kSyncBegin);
        if (clearFirst) output.append(kClearSeq);
        output += '\n';

        if (noFilterResults) {
            output.append(c.num);
            output.append("No filter results");
            output.append(UI::Palette::Reset).append(UI::Palette::BoldReset);
            output += '\n';
        }
    }

    /**
     * @brief Computes original index mappings and padding digit bounds when a filter is active.
     */
    PrintFilterContext computePrintFilterContext(std::size_t startIndex, std::size_t endIndex) {
        const auto* origIdx = filteringStack.empty() ? nullptr : &filteringStack.back().originalIndices;
        const std::size_t origSize = origIdx ? origIdx->size() : 0;

        std::size_t maxOrigDigits = 0;
        if (origIdx) {
            const std::size_t lim = std::min(endIndex, origSize);
            for (std::size_t k = startIndex; k < lim; ++k) {
                maxOrigDigits = std::max(maxOrigDigits,
                    digitCount(static_cast<std::size_t>((*origIdx)[k]) + 1));
            }
        }

        return {origIdx, origSize, maxOrigDigits};
    }

    /**
     * @brief Writes the built frame buffer to standard output under the print mutex lock.
     *
     * When syncing, the frame is written in three segments (before syncInsertPos,
     * the sync banner, after syncInsertPos) so the banner appears between the
     * header and the rows. Exclusion is guaranteed only against other callers
     * that also take @c state->printMutex; it is not a single atomic write to
     * the underlying file descriptor.
     */
    void flushOutput(const std::shared_ptr<RefreshState>& state, const std::string& output,
                     const std::string& syncLine, std::size_t syncInsertPos, bool maybeSyncing) {
        std::lock_guard<std::mutex> lk(state->printMutex);

        const bool syncing = maybeSyncing
            && state->isImportRunning.load(std::memory_order_acquire);

        if (syncing) {
            std::cout.write(output.data(), static_cast<std::streamsize>(syncInsertPos));
            std::cout.write(syncLine.data(), static_cast<std::streamsize>(syncLine.size()));
            std::cout.write(output.data() + syncInsertPos,
                            static_cast<std::streamsize>(output.size() - syncInsertPos));
        } else {
            std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
        }

        std::cout.flush();
    }

} // namespace

/**
 * @brief Renders one page of an ISO, image, or mounted-ISO list to the terminal.
 *
 * Side effects:
 *   - Uses and mutates the thread-local retained buffer for the detected
 *     list kind (see bufferFor).
 *   - After flushing, releases that buffer (swap with a fresh string) if its
 *     capacity exceeds kMaxRetainedCapacity.
 *   - Splices the "Syncing" banner into the frame at the header boundary when
 *     an import is in progress (see prepareSyncLine / flushOutput).
 *
 * @param items        List entries to render.
 * @param listType     Type of list being rendered.
 * @param listSubType  Subtype used to configure row formatting.
 * @param ctx          Rendering state and display options. Notable fields:
 *                     clearFirst (emit clear-screen), displayIndexOffset
 *                     (numbering offset), showSourceIndices (emit "orig^"
 *                     badge), showPendingIndices / hasPendingProcess /
 *                     pendingIndices (pending queue block), currentPage,
 *                     and state (shared refresh state / print mutex).
 */
void printList(const StringListView& items,
               const std::string& listType,
               const std::string& listSubType,
               const PrintListContext& ctx) {

    const ListConfig cfg = getListConfig(items, listType);
    const PaginationInfo page = computePagination(items, ctx.currentPage);

    const std::size_t displayMaxDigits =
        digitCount(ctx.displayIndexOffset + page.endIndex);

    std::string syncLine = prepareSyncLine(
        cfg.isIsoMode, cfg.noFilterResults, ctx.state, page.disablePagination);

    const bool maybeSyncing = !syncLine.empty();

    const ListKind kind = cfg.isIsoMode ? ListKind::Iso
                        : cfg.isImgMode ? ListKind::Image
                                        : ListKind::Mounted;

    std::string& output = bufferFor(kind);
    output.clear();

    reserveAndBuildOutput(
        output, page.startIndex, page.endIndex, items,
        ctx.clearFirst, cfg.noFilterResults, cfg.colors);

    std::size_t syncInsertPos = appendHeader(
        output, page.disablePagination, page.effectivePage, page.totalPages,
        page.startIndex, page.endIndex, items.size(), cfg.colors);

    const PrintFilterContext filterCtx =
        computePrintFilterContext(page.startIndex, page.endIndex);

    const RowConfig rowCfg{
        cfg.isFileMode,
        cfg.isIsoMode,
        cfg.isMountedMode,
        cfg.showNamesOnly,
        cfg.showFullUmount,
        cfg.usableCols,
        cfg.colors
    };

    for (std::size_t i = page.startIndex; i < page.endIndex; ++i) {
        appendItemRow(
            output,
            items,
            i,
            ctx.displayIndexOffset,
            displayMaxDigits,
            filterCtx,
            listSubType,
            rowCfg,
            ctx.showSourceIndices);
    }

    appendFooterAndPending(
        output,
        page.disablePagination,
        page.effectivePage,
        page.totalPages,
        ctx.showPendingIndices,
        ctx.hasPendingProcess,
        ctx.pendingIndices,
        cfg.isImgMode,
        cfg.colors);

    if (kUseSynchronizedOutput) {
        output.append(kSyncEnd);
    }

    flushOutput(ctx.state, output, syncLine, syncInsertPos, maybeSyncing);

    if (output.capacity() > kMaxRetainedCapacity) {
        std::string().swap(output);
    }
}
