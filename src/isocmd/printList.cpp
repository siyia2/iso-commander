// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Project Headers
#include "../display.h"
#include "../filtering.h"
#include "../databaseOps.h"
#include "../main.h"
#include "../sharedRefreshState.h"
#include "../state.h"
#include "../stringListView.h"
#include "../stringManipulation.h"
#include "../themes.h"

namespace fs = std::filesystem;

/**
 * @file list_renderer.cpp
 * @brief Optimized terminal list rendering with pagination, color themes, and stack-based formatting.
 */

namespace {

/// Wrap each frame in "synchronized output" (DEC mode 2026) to eliminate tearing.
/// Terminals that don't support it silently ignore the sequences.
constexpr bool             kUseSynchronizedOutput = true;
constexpr std::string_view kSyncBegin             = "\033[?2026h";
constexpr std::string_view kSyncEnd               = "\033[?2026l";

/// Rough per-row overhead (index digits, color escapes, separators) used to size the buffer.
constexpr std::size_t kPerRowOverhead = 96;

/// Retained thread_local buffer capacity above which we release memory after a render.
constexpr std::size_t kMaxRetainedCapacity = 1u << 20; // 1 MiB

/// Number of decimal digits in value (value 0 -> 1 digit).
inline std::size_t digitCount(std::size_t value) noexcept {
    std::size_t d = 1;
    while (value >= 10) { value /= 10; ++d; }
    return d;
}

/// Formats value into a stack buffer and appends it to out (no heap allocation, no shared state).
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

} // namespace

/**
 * @brief Renders formatted lists (ISO, Image, or Mounts) to the terminal.
 *
 * Performance Notes:
 *  - The frame is built in a thread_local std::string that keeps its capacity across
 *    redraws, so steady-state rendering performs no heap allocation for the buffer.
 *  - Numbers are formatted via std::to_chars straight into the buffer.
 *  - Everything that can be computed without the print lock (filesystem checks, sync
 *    indicator text) is computed before locking. The critical section only re-reads
 *    the isImportRunning flag, writes the frame (in segments, no mid-buffer insert),
 *    and flushes once.
 *  - The frame is wrapped in synchronized-output escape sequences to avoid tearing.
 *
 * Thread Safety: The sync indicator read and terminal write are performed
 * atomically under printMutex, preventing stale indicator display during
 * concurrent background ISO imports.
 *
 * @param items              Non-owning view of the strings to display (no copies are made;
 *                           the underlying vectors must outlive this call).
 * @param listType           Category of the list (e.g., "ISO_FILES").
 * @param listSubType        Extension or sub-format details.
 * @param pendingIndices     Current user selection indices awaiting processing.
 * @param hasPendingProcess  Flag indicating if a process action is staged.
 * @param currentPage        Mutable reference to the current pagination index.
 * @param state              Shared state providing printMutex and isImportRunning
 *                           flag; guards the "[↻ Syncing: NewISO → Restructure]"
 *                           indicator against races with background import completion.
 */
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state) {

    // --- Flags & Config ---
    const bool isIsoMode      = (listType == "ISO_FILES");
    const bool isImgMode      = (listType == "IMAGE_FILES");
    const bool isMountedMode  = (listType == "MOUNTED_ISOS");
    const bool isFileMode     = (isIsoMode || isImgMode);
    const bool showNamesOnly  = displayConfig::toggleNamesOnly;
    const bool showFullUmount = displayConfig::toggleFullListUmount;

    const PrintListTheme c = getListColors();

    const bool noFilterResults =
        (items.empty() && !GlobalState::globalIsoFileList.empty() && isIsoMode) ||
        (items.empty() && !isIsoMode);

    // --- Pagination Logic ---
    const size_t totalItems = items.size();
    const bool disablePagination = (GlobalState::ITEMS_PER_PAGE == 0 || totalItems <= GlobalState::ITEMS_PER_PAGE);
    const size_t totalPages = disablePagination ? 1 : (totalItems + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;

    const size_t effectivePage = disablePagination ? 0 : (currentPage >= totalPages ? totalPages - 1 : currentPage);
    const size_t startIndex = disablePagination ? 0 : (effectivePage * GlobalState::ITEMS_PER_PAGE);
    const size_t endIndex = disablePagination ? totalItems : std::min(startIndex + GlobalState::ITEMS_PER_PAGE, totalItems);

    const size_t maxDigits = digitCount(endIndex);

    // --- Sync indicator preparation (done BEFORE taking the print lock) ---
    // The atomic is read here only as a cheap pre-check to avoid filesystem syscalls when no
    // import is running. It is re-read under the lock below, which is the authoritative check.
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

    // --- Output Buffering (capacity retained across calls) ---
    static thread_local std::string output;
    output.clear();

    {
        std::size_t estimate = 1024;
        for (size_t i = startIndex; i < endIndex; ++i) {
            estimate += items[i].size() + kPerRowOverhead;
        }
        if (output.capacity() < estimate) output.reserve(estimate);
    }

    if (kUseSynchronizedOutput) output.append(kSyncBegin);
    output += '\n';

    // Single-buffer "No filter results" message (previously a separate locked write).
    if (noFilterResults) {
        output.append(c.num); // Warning color for empty items/filter match
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

    // --- Main Item Loop ---
    for (size_t i = startIndex; i < endIndex; ++i) {
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
            if (!showNamesOnly) {
                output.append(c.dir).append(dir).append(UI::Palette::BoldReset).append("/");
            }
            output.append(isIsoMode ? c.iso : c.img).append(fname);
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
        output.append(UI::Palette::Reset).append(UI::Palette::BoldReset).append("\n");
    }

    // --- Footer ---
    if (!disablePagination) {
        output.append("\n").append(c.head);
        if (effectivePage > 0) output.append("[PgUp] Prev | ");
        if (effectivePage < totalPages - 1) output.append("[PgDn] Next | ");
        output.append("[g#] ↵ GoTo").append(UI::Palette::BoldReset).append("\n");
    }

    // --- Pending Processes ---
    if (hasPendingProcess && !pendingIndices.empty()) {
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

        // Single flush per frame, done under the lock so no other thread touches the stream.
        std::cout.flush();
    }

    // Don't let one huge render pin a large buffer for the life of the thread.
    if (output.capacity() > kMaxRetainedCapacity) {
        std::string().swap(output);
    }
}
