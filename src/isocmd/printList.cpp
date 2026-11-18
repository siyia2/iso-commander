// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// C / System Headers
#include <sys/ioctl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../display.h"
#include "../filtering.h"
#include "../databaseOps.h"
#include "../rendering.h"
#include "../main.h"
#include "../sharedRefreshState.h"
#include "../state.h"
#include "../stringListView.h"
#include "../stringManipulation.h"
#include "../themes.h"

namespace fs = std::filesystem;

/// Clear screen + scrollback, home cursor, reset attributes. Emitted inside the
/// sync frame so the clear and the new content are presented atomically.
constexpr std::string_view kClearSeq = "\033[2J\033[3J\033[H\033[0m";

/// Rough per-row overhead (index digits, color escapes, separators) used to size
/// the buffer.
constexpr std::size_t kPerRowOverhead = 96;

/// Retained capacity (per list-type buffer) above which we release memory after a
/// render.
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
        char buf[20]; // enough for a 64-bit unsigned integer
        const auto res = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, static_cast<std::size_t>(res.ptr - buf));
    }

    /**
     * @brief Formats an integer value with right-alignment padding into the output string.
     *
     * @param out   Reference to the destination string buffer.
     * @param value The numeric value to append.
     * @param width The target formatting column width.
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
     * Performs an atomic pre-check to bypass unnecessary filesystem syscalls when imports are inactive.
     * Validates history file availability and formats the dim synchronization status string, including
     * filtering lock notices if applicable.
     *
     * @param isIsoMode         True if the active view mode is ISO_FILES.
     * @param noFilterResults   True if the active filter yielded empty results.
     * @param state             Shared refresh state containing import flags and synchronization mutexes.
     * @param disablePagination True if pagination is bypassed for the active view.
     * @return std::string      The formatted sync message string, or an empty string if no sync is required.
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
     * Formats the current page, total pages, and visible item range statistics (e.g., "Page 1/3 (Items (1-50)/150)")
     * using theme colors and appends it to the output string buffer.
     *
     * @param output            Reference to the retained output string buffer.
     * @param disablePagination Flag indicating whether pagination is bypassed.
     * @param effectivePage     Zero-based index of the current active page.
     * @param totalPages        Total number of available pages.
     * @param startIndex        1-based index of the first item on the current page.
     * @param endIndex          1-based index of the last item on the current page.
     * @param totalItems        Total number of items across all pages.
     * @param c                 Theme color configuration structure (@c PrintListTheme).
     * @return size_t           The exact byte offset position in the buffer where the sync line can be spliced.
     */
    size_t appendHeader(std::string& output, bool disablePagination, size_t effectivePage, size_t totalPages,
                        size_t startIndex, size_t endIndex, size_t totalItems, const PrintListTheme& c)
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

            size_t syncInsertPos = output.size();
            output.append(UI::Palette::BoldReset).append("\n\n");
            return syncInsertPos;
        } else {
            size_t syncInsertPos = output.size();
            return syncInsertPos;
        }
    }

    /**
     * @brief Bundles filtering index mappings, collection sizes, and dynamic padding constraints.
     */
    struct PrintFilterContext {
        const std::vector<unsigned long>* origIdx; /**< Pointer to original index mapping vector, or nullptr. */
        size_t origSize;                           /**< Total size of the original index vector. */
        size_t maxOrigDigits;                      /**< Maximum digit length across active original indices for alignment. */
    };

    /**
     * @brief Bundles list mode flags, path layout toggles, sizing constraints, and color themes.
     */
    struct RowConfig {
        bool isFileMode;         /**< True if the item list represents file entries. */
        bool isIsoMode;          /**< True if the item list represents ISO file entries. */
        bool isMountedMode;      /**< True if the item list represents mounted ISO entries. */
        bool showNamesOnly;      /**< Configuration flag to hide parent directory paths. */
        bool showFullUmount;     /**< Configuration flag to display full unmount path details. */
        size_t usableCols;       /**< Maximum width in columns available for row rendering. */
        const PrintListTheme& colors; /**< Theme palette (non-owning; must outlive the row config). */
    };

    /**
     * @brief Renders and clips a single item row into the output buffer with uniform alignment.
     *
     * Handles row numbering, original index badge rendering (`n^`) when filters are active with dynamic padding,
     * directory/filename splitting for file modes, path parsing for mounted ISO modes, and enforces terminal width
     * bounds to prevent line auto-wrapping. Directory paths are clipped from the left with an ellipsis (`…`) if needed.
     *
     * @param output      Reference to the retained output string buffer.
     * @param items       Non-owning view collection of string items to display.
     * @param i           Current zero-based index of the item being rendered.
     * @param maxDigits   Maximum digit count used for right-aligning row sequence numbers.
     * @param filterCtx   Filtering context structure containing index mappings and padding constraints.
     * @param listSubType Extension or sub-format string used for path splitting.
     * @param cfg         Row configuration flags, view constraints, and color definitions (@c RowConfig).
     */
    void appendItemRow(std::string& output, const StringListView& items, size_t i, size_t maxDigits,
                       const PrintFilterContext& filterCtx, const std::string& listSubType, const RowConfig& cfg)
    {
        const std::size_t rowStart = output.size();
        const std::string_view seqColor = (i % 2 == 0) ? cfg.colors.indexA : cfg.colors.indexB;

        output.append(seqColor);
        appendNumPadded(output, i + 1, maxDigits);

        if (filterCtx.origIdx && i < filterCtx.origSize) {
            const std::size_t origVal = static_cast<std::size_t>((*filterCtx.origIdx)[i]) + 1;
            output.append(":").append(UI::Palette::BoldReset).append(cfg.colors.square);
            appendNum(output, origVal);
            output.append("^");
            const std::size_t pad = filterCtx.maxOrigDigits - digitCount(origVal);
            if (pad) output.append(pad, ' ');
            output.append(" ").append(UI::Palette::BoldReset);
        } else {
            output.append(". ").append(UI::Palette::BoldReset);
        }

        const std::string& item = items[i];

        if (cfg.isFileMode) {
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
        else if (cfg.isMountedMode) {
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

        if (cfg.usableCols > 0) fitRowToWidth(output, rowStart, cfg.usableCols);

        output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
    }

    /**
     * @brief Appends pagination footers and staged pending process queues to the output frame buffer.
     *
     * @param output             Reference to the retained output string buffer.
     * @param disablePagination  Flag indicating whether pagination is bypassed.
     * @param effectivePage      Zero-based index of the current active page.
     * @param totalPages         Total number of available pages.
     * @param showPendingIndices Flag indicating whether pending indices should be displayed.
     * @param hasPendingProcess  Flag indicating whether a process action is currently staged.
     * @param pendingIndices     Vector containing string identifiers of staged indices.
     * @param isImgMode          Flag indicating if the list represents image files (used for color styling).
     * @param c                  Theme color configuration structure (@c PrintListTheme).
     */
    void appendFooterAndPending(std::string& output, bool disablePagination, size_t effectivePage,
                                size_t totalPages, bool showPendingIndices, bool hasPendingProcess,
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
            for (size_t i = 0; i < pendingIndices.size(); ++i) {
                output.append(pendingIndices[i]);
                if (i < pendingIndices.size() - 1) output.push_back(' ');
            }
            output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
        }
    }

    /**
     * @brief Configuration and display flags derived from the list type and global settings.
     */
    struct ListConfig {
        bool isIsoMode;         /**< True if the list represents ISO files. */
        bool isImgMode;         /**< True if the list represents image files. */
        bool isMountedMode;     /**< True if the list represents mounted ISOs. */
        bool isFileMode;        /**< True if the list is either ISO or image files. */
        bool showNamesOnly;     /**< Configuration toggle to show filenames only without paths. */
        bool showFullUmount;    /**< Configuration toggle for full unmount listing options. */
        PrintListTheme colors;  /**< Color palette associated with the list. */
        size_t usableCols;      /**< Number of usable columns in the terminal (width - 1). */
        bool noFilterResults;   /**< True if an active filter yields zero matching items. */
    };

    /**
     * @brief Evaluates the current list type, configuration settings, and terminal dimensions.
     *
     * @param items    Non-owning view of the items to display.
     * @param listType Category string specifying the list kind ("ISO_FILES", "IMAGE_FILES", "MOUNTED_ISOS").
     * @return ListConfig Populated configuration structure.
     */
    ListConfig getListConfig(const StringListView& items, const std::string& listType) {
        const bool isIsoMode      = (listType == "ISO_FILES");
        const bool isImgMode      = (listType == "IMAGE_FILES");
        const bool isMountedMode  = (listType == "MOUNTED_ISOS");
        const bool isFileMode     = (isIsoMode || isImgMode);
        const bool showNamesOnly  = displayConfig::toggleNamesOnly;
        const bool showFullUmount = displayConfig::toggleFullListUmount;
        const size_t termCols   = terminalColumns();
        const size_t usableCols = termCols > 1 ? termCols - 1 : 0;

        const bool noFilterResults =
            (items.empty() && !GlobalState::globalIsoFileList.empty() && isIsoMode) ||
            (items.empty() && !isIsoMode);

        return {isIsoMode, isImgMode, isMountedMode, isFileMode, showNamesOnly, showFullUmount, getListColors(), usableCols, noFilterResults};
    }

    /**
     * @brief Computed pagination boundaries and slice indices for the current frame.
     */
    struct PaginationInfo {
        bool disablePagination; /**< True if pagination is turned off or all items fit on one page. */
        size_t totalPages;      /**< Total number of available pages. */
        size_t effectivePage;   /**< Clamped zero-based active page index. */
        size_t startIndex;      /**< Starting index of items for the current page. */
        size_t endIndex;        /**< Ending index (exclusive) of items for the current page. */
        size_t maxDigits;       /**< Number of digits required to format row item numbers. */
    };

    /**
     * @brief Calculates pagination boundaries based on item count and global page constraints.
     *
     * @param items       Non-owning view of all list items.
     * @param currentPage Requested zero-based page index.
     * @return PaginationInfo Computed pagination metrics.
     */
    PaginationInfo computePagination(const StringListView& items, size_t currentPage) {
        const size_t totalItems = items.size();
        const bool disablePagination = (GlobalState::ITEMS_PER_PAGE == 0 || totalItems <= GlobalState::ITEMS_PER_PAGE);
        const size_t totalPages = disablePagination ? 1 : (totalItems + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;

        const size_t effectivePage = disablePagination ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
        const size_t startIndex = disablePagination ? 0 : (effectivePage * GlobalState::ITEMS_PER_PAGE);
        const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + GlobalState::ITEMS_PER_PAGE, totalItems);
        const size_t maxDigits = digitCount(endIndex);

        return {disablePagination, totalPages, effectivePage, startIndex, endIndex, maxDigits};
    }

    /**
     * @brief Reserves buffer capacity and appends initial frame formatting (synchronized output, clears, and filter notices).
     *
     * @param output          Retained string buffer for the current list kind.
     * @param startIndex      Start index of items on the page.
     * @param endIndex        End index of items on the page.
     * @param items           Non-owning view of the list items.
     * @param clearFirst      If true, appends screen clear sequence.
     * @param noFilterResults If true, appends a "No filter results" notice.
     * @param c               Theme color palette.
     */
    void reserveAndBuildOutput(std::string& output, size_t startIndex, size_t endIndex,
                               const StringListView& items, bool clearFirst, bool noFilterResults,
                               const PrintListTheme& c) {
        {
            std::size_t estimate = 1024;
            for (size_t i = startIndex; i < endIndex; ++i) {
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
     *
     * @param startIndex Start index of items on the page.
     * @param endIndex   End index of items on the page.
     * @return PrintFilterContext Populated print filter context structure.
     */
    PrintFilterContext computePrintFilterContext(size_t startIndex, size_t endIndex) {
        const auto* origIdx = filteringStack.empty() ? nullptr : &filteringStack.back().originalIndices;
        const size_t origSize = origIdx ? origIdx->size() : 0;

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
     * @brief Atomically writes the built frame buffer to standard output under the print mutex lock.
     *
     * @param state         Shared application state containing the synchronization mutex and background import flag.
     * @param output        The fully assembled frame string buffer.
     * @param syncLine      Pre-formatted sync/import indicator string.
     * @param syncInsertPos Buffer byte offset where the sync line should be spliced if syncing is active.
     * @param maybeSyncing  Quick preliminary check flag indicating whether an import could be running.
     */
    void flushOutput(const std::shared_ptr<RefreshState>& state, const std::string& output,
                     const std::string& syncLine, size_t syncInsertPos, bool maybeSyncing) {
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
 * The whole frame (optional clear, header, rows, footer, pending-indices line)
 * is built in a retained buffer and written to stdout with a single flush.
 *
 * @par Layout
 *  - "No filter results" is shown when the list is empty because of an active
 *    filter (or, for non-ISO lists, simply empty).
 *  - When the list spans more than one page, a header ("Page n/N (Items
 *    (a-b)/total)") and a footer with PgUp / PgDn / g# hints are shown.
 *    Pagination is disabled if ITEMS_PER_PAGE is 0 or all items fit on one page.
 *  - Rows are numbered and right-aligned. While a filter is active, each row
 *    also shows the item's original index ("n:m^"). The original-index field
 *    is padded (after the '^') so that filenames line up across all rows on
 *    the page.
 *  - ISO mode only: while a background import is running, a dim
 *    "[↻ Syncing: NewISO → Restructure]" indicator (or a "no history" notice)
 *    is inserted after the header, plus a "Filtering locked" note if the
 *    filtering indicator is set.
 *  - If @p showPendingIndices is set and a process is staged, a
 *    "Pending Indices [P]: ..." line is appended.
 *
 * @par Terminal width
 * Rows never wrap. The width is queried once per frame via terminalColumns()
 * (a transient failure keeps the last known width; an unknown width, e.g.
 * redirected output, disables clipping), and the last column is always left
 * free so a full-width row cannot trigger auto-wrap. An over-wide row is
 * shortened in two steps:
 *  1. File lists with the directory shown: the directory is cut from its left
 *     side and prefixed with "…" so the filename stays intact. If there is no
 *     room even for that, the directory is dropped for that row.
 *  2. Any row still too wide (very long filename, narrow terminal, or a
 *     mounted-ISO row) is cut at the right with "…" (ANSI- and UTF-8-aware).
 * The header, footer, sync indicator and pending-indices line are not clipped.
 * A resize only affects the next call; to repaint while a prompt is waiting,
 * the prompt owner runs a @ref ListResizeWatcher around its @c readline() call.
 *
 * @par Performance
 *  - The frame is built in a thread_local std::string per list type. Capacity
 *    is retained across redraws (no steady-state allocation, and switching list
 *    types never reallocates another type's buffer). A buffer that grows past
 *    kMaxRetainedCapacity (4 MiB) is released after the render.
 *  - Numbers are formatted with std::to_chars directly into the buffer.
 *  - Filesystem checks and the sync-indicator text are computed before taking
 *    the print lock. Under the lock, the code re-reads isImportRunning, writes
 *    the frame (in segments, so the indicator is spliced in without an insert),
 *    and flushes once.
 *  - If kUseSynchronizedOutput is set, the frame is wrapped in
 *    synchronized-output escapes to avoid tearing.
 *
 * @par Thread safety
 * The sync-indicator check and the terminal write happen atomically under
 * RefreshState::printMutex, so a background import finishing cannot leave a
 * stale "Syncing" indicator on screen.
 *
 * @param items               Non-owning view of the strings to display. No copies
 *                            are made; the underlying storage must outlive the call.
 * @param listType            List category: "ISO_FILES", "IMAGE_FILES" or "MOUNTED_ISOS".
 * @param listSubType         Extension / sub-format used to split directory and filename.
 * @param pendingIndices      Indices staged for processing (shown in the pending line).
 * @param hasPendingProcess   Whether a process action is staged.
 * @param currentPage         Requested zero-based page. It is clamped to the last
 *                            page for display; the caller's value is not modified.
 * @param state               Shared state providing printMutex and isImportRunning.
 * @param clearFirst          If true, clears screen and scrollback inside the same
 *                            synchronized frame, so clear and content appear
 *                            atomically (no blank flash). Default is declared in
 *                            the header.
 * @param showPendingIndices  If true, appends the pending-indices line when a
 *                            process is staged. Default is declared in the header.
 */
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst, bool showPendingIndices) {

    const ListConfig cfg = getListConfig(items, listType);
    const PaginationInfo page = computePagination(items, currentPage);

    std::string syncLine = prepareSyncLine(cfg.isIsoMode, cfg.noFilterResults, state, page.disablePagination);
    const bool maybeSyncing = !syncLine.empty();

    const ListKind kind = cfg.isIsoMode ? ListKind::Iso
                        : cfg.isImgMode ? ListKind::Image
                                    : ListKind::Mounted;
    std::string& output = bufferFor(kind);
    output.clear();

    reserveAndBuildOutput(output, page.startIndex, page.endIndex, items, clearFirst, cfg.noFilterResults, cfg.colors);

    size_t syncInsertPos = appendHeader(output, page.disablePagination, page.effectivePage, page.totalPages,
                                        page.startIndex, page.endIndex, items.size(), cfg.colors);

    const PrintFilterContext filterCtx = computePrintFilterContext(page.startIndex, page.endIndex);
    const RowConfig rowCfg{cfg.isFileMode, cfg.isIsoMode, cfg.isMountedMode, cfg.showNamesOnly, cfg.showFullUmount, cfg.usableCols, cfg.colors};

    for (size_t i = page.startIndex; i < page.endIndex; ++i) {
        appendItemRow(output, items, i, page.maxDigits, filterCtx, listSubType, rowCfg);
    }

    appendFooterAndPending(output, page.disablePagination, page.effectivePage, page.totalPages,
                           showPendingIndices, hasPendingProcess, pendingIndices, cfg.isImgMode, cfg.colors);

    if (kUseSynchronizedOutput) output.append(kSyncEnd);

    flushOutput(state, output, syncLine, syncInsertPos, maybeSyncing);

    if (output.capacity() > kMaxRetainedCapacity) {
        std::string().swap(output);
    }
}
