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
constexpr std::size_t kMaxRetainedCapacity = 5u << 20; // 5 MiB

/// Which list a frame is being rendered for. Each kind owns its own retained buffer.
enum class ListKind : std::size_t { Iso, Image, Mounted, Count };

/// One retained thread_local buffer per list type, so rendering one list type
/// never grows, shrinks, or reallocates the buffer used by another.
inline std::string& bufferFor(ListKind kind) {
    static thread_local std::array<std::string, static_cast<std::size_t>(ListKind::Count)> buffers;
    return buffers[static_cast<std::size_t>(kind)];
}

/// Number of decimal digits in value (value 0 -> 1 digit).
inline std::size_t digitCount(std::size_t value) noexcept {
    std::size_t d = 1;
    while (value >= 10) { value /= 10; ++d; }
    return d;
}

/// Formats value into a stack buffer and appends it to out (no heap allocation,
/// no shared state).
inline void appendNum(std::string& out, std::size_t value) {
    char buf[20]; // enough for a 64-bit unsigned integer
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

/// Like appendNum, but right-aligns the number to the given width using spaces.
inline void appendNumPadded(std::string& out, std::size_t value, std::size_t width) {
    char buf[20];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    const std::size_t len = static_cast<std::size_t>(res.ptr - buf);
    if (len < width) out.append(width - len, ' ');
    out.append(buf, len);
}

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
 *    also shows the item's original index ("n:m^").
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
 *    kMaxRetainedCapacity (5 MiB) is released after the render.
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
 * @param items              Non-owning view of the strings to display. No copies
 *                           are made; the underlying storage must outlive the call.
 * @param listType           List category: "ISO_FILES", "IMAGE_FILES" or "MOUNTED_ISOS".
 * @param listSubType        Extension / sub-format used to split directory and filename.
 * @param pendingIndices     Indices staged for processing (shown in the pending line).
 * @param hasPendingProcess  Whether a process action is staged.
 * @param currentPage        Requested zero-based page. It is clamped to the last
 *                           page for display; the caller's value is not modified.
 * @param state              Shared state providing printMutex and isImportRunning.
 * @param clearFirst         If true, clears screen and scrollback inside the same
 *                           synchronized frame, so clear and content appear
 *                           atomically (no blank flash). Default is declared in
 *                           the header.
 * @param showPendingIndices If true, appends the pending-indices line when a
 *                           process is staged. Default is declared in the header.
 */
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst, bool showPendingIndices) {

    // --- Flags & config ---
    const bool isIsoMode      = (listType == "ISO_FILES");
    const bool isImgMode      = (listType == "IMAGE_FILES");
    const bool isMountedMode  = (listType == "MOUNTED_ISOS");
    const bool isFileMode     = (isIsoMode || isImgMode);
    const bool showNamesOnly  = displayConfig::toggleNamesOnly;
    const bool showFullUmount = displayConfig::toggleFullListUmount;

    const PrintListTheme c = getListColors();

    // Columns available to an item row; 0 = unknown width, do not clip. The last
    // column stays free so a full-width row cannot trigger the terminal's auto-wrap.
    const size_t termCols   = terminalColumns();
    const size_t usableCols = termCols > 1 ? termCols - 1 : 0;

    const bool noFilterResults =
        (items.empty() && !GlobalState::globalIsoFileList.empty() && isIsoMode) ||
        (items.empty() && !isIsoMode);

    // --- Pagination ---
    const size_t totalItems = items.size();
    const bool disablePagination = (GlobalState::ITEMS_PER_PAGE == 0 || totalItems <= GlobalState::ITEMS_PER_PAGE);
    const size_t totalPages = disablePagination ? 1 : (totalItems + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;

    const size_t effectivePage = disablePagination ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
    const size_t startIndex = disablePagination ? 0 : (effectivePage * GlobalState::ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + GlobalState::ITEMS_PER_PAGE, totalItems);

    const size_t maxDigits = digitCount(endIndex);

    // --- Sync indicator (prepared BEFORE taking the print lock) ---
    // The atomic is read here only as a cheap pre-check to avoid filesystem syscalls
    // when no import is running. It is re-read under the lock below, which is the
    // authoritative check.
    const bool wantSync = isIsoMode && !noFilterResults && !GlobalState::globalIsoFileList.empty();
    const bool maybeSyncing = wantSync && state->isImportRunning.load(std::memory_order_acquire);

    std::string syncLine;
    if (maybeSyncing) {
        const bool historyOk = !isHistoryFileEmpty(GlobalState::historyFilePath)
                               && fs::is_regular_file(GlobalState::historyFilePath);
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
    }

    // --- Output buffer (per-list-type, capacity retained across calls) ---
    const ListKind kind = isIsoMode ? ListKind::Iso
                        : isImgMode ? ListKind::Image
                                    : ListKind::Mounted;
    std::string& output = bufferFor(kind);
    output.clear();

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

    // Single-buffer "No filter results" message (previously a separate locked write).
    if (noFilterResults) {
        output.append(c.num); // warning color for empty items/filter match
        output.append("No filter results");
        output.append(UI::Palette::Reset).append(UI::Palette::BoldReset);
        output += '\n';
    }

    // --- Header ---
    size_t syncInsertPos = std::string::npos;
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

        syncInsertPos = output.size(); // mark insertion point before BoldReset+\n\n
        output.append(UI::Palette::BoldReset).append("\n\n");
    } else {
        syncInsertPos = output.size(); // mark insertion point after leading \n
    }

    // --- Hoisted per-frame lookups ---
    const auto* origIdx = filteringStack.empty() ? nullptr : &filteringStack.back().originalIndices;
    const size_t origSize = origIdx ? origIdx->size() : 0;

    // --- Main item loop ---
    for (size_t i = startIndex; i < endIndex; ++i) {
        const std::size_t rowStart = output.size();   // start of this row, for width fitting
        const std::string_view seqColor = (i % 2 == 0) ? c.indexA : c.indexB;

        output.append(seqColor);
        appendNumPadded(output, i + 1, maxDigits);

        if (origIdx && i < origSize) {
            output.append(":").append(UI::Palette::BoldReset).append(c.square);
            appendNum(output, static_cast<std::size_t>((*origIdx)[i]) + 1);
            output.append(UI::Palette::BoldReset).append(c.square).append("^ ")
                  .append(UI::Palette::BoldReset);
        } else {
            output.append(". ").append(UI::Palette::BoldReset);
        }

        const std::string& item = items[i];

        if (isFileMode) {
            auto [dir, fname] = extractDirectoryAndFilename(item, listSubType);
            const std::string_view dirView{dir};
            const std::string_view nameView{fname};

            // Shorten the directory (from its left) before anything else, so the
            // filename stays complete whenever the terminal can hold it.
            std::string_view dirShown = dirView;
            bool dirCut  = false;
            bool dirDrop = showNamesOnly;
            if (!showNamesOnly && usableCols > 0) {
                const std::size_t prefixW = visibleWidthFrom(output, rowStart);
                const std::size_t nameW   = utf8Cols(nameView);
                if (prefixW + utf8Cols(dirView) + 1 + nameW > usableCols) {
                    const std::size_t fixed  = prefixW + 1 + nameW;               // prefix + "/" + name
                    const std::size_t budget = usableCols > fixed ? usableCols - fixed : 0;
                    if (budget >= 2) {                                           // "…" + at least 1 char
                        dirShown = tailColumns(dirView, budget - 1);
                        dirCut   = true;
                    } else {
                        dirDrop = true;
                    }
                }
            }

            if (!dirDrop) {
                output.append(c.dir);
                if (dirCut) output.append("\u2026");
                output.append(dirShown).append(UI::Palette::BoldReset).append("/");
            }
            output.append(isIsoMode ? c.iso : c.img).append(nameView);
        }
        else if (isMountedMode) {
            auto [dirPart, pathPart, hashPart] = parseMountPointComponents(item);
            if (showFullUmount) {
                output.append(c.mnt)
                      .append(dirPart).append(UI::Palette::Reset).append(UI::Palette::BoldReset)
                      .append(c.iso).append(pathPart)
                      .append(c.square).append(hashPart);
            } else {
                output.append(c.iso).append(pathPart);
            }
        }

        // Last line of defence: never let a row wrap.
        if (usableCols > 0) fitRowToWidth(output, rowStart, usableCols);

        output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
    }

    // --- Footer ---
    if (!disablePagination) {
        output.append("\n").append(c.head);
        if (effectivePage > 0) output.append("[PgUp] Prev | ");
        if (effectivePage < totalPages - 1) output.append("[PgDn] Next | ");
        output.append("[g#] ↵ GoTo").append(UI::Palette::BoldReset).append("\n");
    }

    // --- Pending processes ---
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

    if (kUseSynchronizedOutput) output.append(kSyncEnd);

    // --- Sync-safe print (minimal critical section) ---
    {
        std::lock_guard<std::mutex> lk(state->printMutex);

        // Authoritative read: guarantees we never show a stale "Syncing" indicator.
        const bool syncing = maybeSyncing
            && state->isImportRunning.load(std::memory_order_acquire);

        if (syncing) {
            // Write in three segments instead of output.insert() (no memmove / realloc).
            std::cout.write(output.data(), static_cast<std::streamsize>(syncInsertPos));
            std::cout.write(syncLine.data(), static_cast<std::streamsize>(syncLine.size()));
            std::cout.write(output.data() + syncInsertPos,
                            static_cast<std::streamsize>(output.size() - syncInsertPos));
        } else {
            std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
        }

        // Single flush per frame, under the lock so no other thread touches the stream.
        std::cout.flush();
    }

    // Don't let one huge render pin a large buffer for the life of the thread.
    // Only this list type's buffer is released; the other buffers are untouched.
    if (output.capacity() > kMaxRetainedCapacity) {
        std::string().swap(output);
    }
}
