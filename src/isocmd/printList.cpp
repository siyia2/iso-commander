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
#include "../listResizeWatcher.h"
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

/// Clear screen + scrollback, home cursor, reset attributes. Emitted inside the sync frame
/// so the clear and the new content are presented atomically.
constexpr std::string_view kClearSeq = "\033[2J\033[3J\033[H\033[0m";

/// Rough per-row overhead (index digits, color escapes, separators) used to size the buffer.
constexpr std::size_t kPerRowOverhead = 96;

/// Retained capacity (per list-type buffer) above which we release memory after a render.
constexpr std::size_t kMaxRetainedCapacity = 5u << 20; // 5 MiB

/// Which list a frame is being rendered for. Each kind owns its own retained buffer.
enum class ListKind : std::size_t { Iso, Image, Mounted, Count };

/// One retained thread_local buffer per list type, so rendering one list type never
/// grows, shrinks, or reallocates the buffer used by another.
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

// ------------------------------------------------------------------------- //
// Terminal width helpers                                                     //
// ------------------------------------------------------------------------- //

/**
 * @brief Terminal size in columns and rows; {0, 0} if it has never been known.
 *
 * Queries @c TIOCGWINSZ on stdout. On success the size is remembered; if a later
 * query fails or reports a zero dimension (detached or transiently resized pty), the
 * last known good size is returned instead, so a transient failure never changes the
 * layout and is never mistaken for a resize. Until a first success (e.g. output is
 * redirected, not a tty) the result is {0, 0}, which callers treat as "no limit":
 * rows are then printed unclipped, as before.
 *
 * The remembered size is one packed atomic (cols << 32 | rows), so a reader never
 * sees a torn pair. It is read by the main thread, by the Readline event hook and
 * by the ListResizeWatcher thread.
 */
struct TermSize { std::size_t cols; std::size_t rows; };

inline TermSize terminalSize() {
    static std::atomic<std::uint64_t> lastGood{0};

    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        lastGood.store((std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row},
                       std::memory_order_relaxed);
        return { ws.ws_col, ws.ws_row };
    }
    const std::uint64_t v = lastGood.load(std::memory_order_relaxed);
    return { static_cast<std::size_t>(v >> 32), static_cast<std::size_t>(v & 0xFFFFFFFFu) };
}

/// Terminal width in columns, or 0 if unknown (see terminalSize()).
inline std::size_t terminalColumns() { return terminalSize().cols; }

/// Visible columns of s[from..]: UTF-8 code points count 1, ANSI CSI escapes count 0.
inline std::size_t visibleWidthFrom(const std::string& s, std::size_t from) {
    std::size_t w = 0;
    for (std::size_t i = from; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            if (i < s.size()) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++w;
        ++i;
    }
    return w;
}

/// Visible columns of a plain UTF-8 string (code points, no escapes expected).
inline std::size_t utf8Cols(std::string_view s) noexcept {
    std::size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++w;
    return w;
}

/// The last @p cols code points of s (all of s if it is shorter). Never splits a
/// multi-byte sequence.
inline std::string_view tailColumns(std::string_view s, std::size_t cols) noexcept {
    if (cols == 0) return {};
    std::size_t seen = 0;
    for (std::size_t i = s.size(); i > 0; ) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++seen == cols)
            return s.substr(i);
    }
    return s;
}

/**
 * @brief Cuts the row that starts at out[from] to at most @p maxCols visible columns.
 *
 * Last line of defence against wrapping. If the row is wider than @p maxCols it is cut
 * in place so that it ends with "…" and occupies exactly @p maxCols columns. ANSI CSI
 * escapes before the cut are kept (callers append their own reset afterwards); a
 * multi-byte character is never split. A row that already fits is left untouched.
 * @p maxCols below 2 leaves the row alone (no room for even one character and "…").
 */
inline void fitRowToWidth(std::string& out, std::size_t from, std::size_t maxCols) {
    if (maxCols < 2) return;

    std::size_t w = 0;
    std::size_t cutAt = std::string::npos;   // offset just before the maxCols-th visible char

    for (std::size_t i = from; i < out.size(); ) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 0x1B && i + 1 < out.size() && out[i + 1] == '[') {
            i += 2;
            while (i < out.size() && !(out[i] >= 0x40 && out[i] <= 0x7E)) ++i;
            if (i < out.size()) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) {
            if (w == maxCols - 1) cutAt = i;
            ++w;
        }
        ++i;
    }

    if (w > maxCols && cutAt != std::string::npos) {
        out.resize(cutAt);
        out.append("\u2026");
    }
}

} // namespace

/**
 * @brief Renders formatted lists (ISO, Image, or Mounts) to the terminal.
 *
 * Performance Notes:
 *  - The frame is built in a thread_local std::string (one per list type: ISO, Image,
 *    Mounts) that keeps its capacity across redraws, so steady-state rendering performs
 *    no heap allocation for the buffer, and switching between list types never
 *    reallocates.
 *  - Numbers are formatted via std::to_chars straight into the buffer.
 *  - Everything that can be computed without the print lock (filesystem checks, sync
 *    indicator text) is computed before locking. The critical section only re-reads
 *    the isImportRunning flag, writes the frame (in segments, no mid-buffer insert),
 *    and flushes once.
 *  - The frame is wrapped in synchronized-output escape sequences to avoid tearing.
 *
 * @par Terminal width
 * Item rows never wrap. The width is queried once per frame (see terminalColumns();
 * a transient query failure keeps the last known good width, and an unknown width,
 * e.g. redirected output, means no clipping). The last column is kept free so a row
 * can never trigger auto-wrap. A row that is too wide is shortened in two steps:
 * - File lists (ISO / image), when the directory is shown: the directory is cut from
 *   its left side and prefixed with "…", so the part next to the filename survives
 *   and the filename stays complete. If even that leaves no room, the directory is
 *   dropped for that row.
 * - Any row still too wide (an extremely long filename, a narrow terminal, or a
 *   mounted-ISO row) is cut at the right with "…" (ANSI-escape and UTF-8 aware).
 * The pagination header, footer, sync indicator and pending-indices line are not
 * clipped. A resize only changes what the next call produces; to repaint while a
 * prompt is waiting, the prompt owner starts a @ref ListResizeWatcher around its
 * @c readline() call (see the end of this file).
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
 * @param clearFirst         If true, clears the screen and scrollback inside the same
 *                           synchronized-output frame, so clear and content present
 *                           atomically (no blank flash). Default is declared in the header.
 */
void printList(const StringListView& items, const std::string& listType, const std::string& listSubType,
               std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
               size_t& currentPage, std::shared_ptr<RefreshState> state, bool clearFirst, bool showPendingIndices) {

    // --- Flags & Config ---
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

    // --- Output Buffering (per-list-type buffer, capacity retained across calls) ---
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

    // --- Pending Processes ---
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

        // Single flush per frame, done under the lock so no other thread touches the stream.
        std::cout.flush();
    }

    // Don't let one huge render pin a large buffer for the life of the thread.
    // Only this list type's buffer is released; the other buffers are untouched.
    if (output.capacity() > kMaxRetainedCapacity) {
        std::string().swap(output);
    }
}

// ------------------------------------------------------------------------- //
// ListResizeWatcher                                                          //
// ------------------------------------------------------------------------- //

/**
 * @brief State shared by the watcher thread and the Readline event hook.
 *
 * The watcher thread only sets @ref pending. Everything that touches the terminal or
 * Readline (@ref tick) runs on the main thread, from the hook, where Readline's
 * globals (rl_line_buffer, rl_point, ...) can be used without a data race.
 */
struct ListResizeWatcher::Impl {
    explicit Impl(Repaint r) : repaint(std::move(r)), lastCols(terminalSize().cols) {}

    Repaint            repaint;
    std::size_t        lastCols;            // watcher thread only after construction
    std::atomic<bool>  pending{false};
    rl_hook_func_t*    prevHook = nullptr;  // hook that was installed before ours (chained)

    std::mutex              m;
    std::condition_variable cv;
    bool                    stopRequested = false;
    std::thread             thread;

    /// Polls the width every 100 ms and flags a change. A failed query keeps the last
    /// good size (see terminalSize()), so it can never look like a resize.
    void loop() {
        constexpr auto kPoll = std::chrono::milliseconds(100);
        std::unique_lock<std::mutex> lk(m);
        while (!stopRequested) {
            cv.wait_for(lk, kPoll, [&] { return stopRequested; });
            if (stopRequested) break;

            const std::size_t cols = terminalSize().cols;
            if (cols == 0) continue;                       // size never known
            if (lastCols == 0) { lastCols = cols; continue; }   // first reading = baseline
            if (cols != lastCols) {
                lastCols = cols;
                pending.store(true, std::memory_order_release);
            }
        }
    }

    /// Main thread: if a resize is pending, repaint the list and restore the prompt.
    void tick() {
        if (!pending.exchange(false, std::memory_order_acquire)) return;

        const TermSize sz = terminalSize();
        if (sz.cols == 0 || sz.rows == 0) return;

        // Keep Readline's own wrapping in step with the layout painted below.
        rl_set_screen_size(static_cast<int>(sz.rows), static_cast<int>(sz.cols));

        // The callback re-runs the normal list display step (it must not throw through
        // Readline's C frames).
        try { repaint(); } catch (...) {}

        // Normal layout: the list ends, one blank line, then the prompt line. The
        // callback leaves the cursor where the list display step does; this newline is
        // the blank line. Readline then redraws the prompt's last line plus whatever
        // the user had typed, cursor position included.
        std::cout << '\n' << std::flush;
        rl_on_new_line();
        rl_redisplay();
    }
};

namespace {

/// The watcher the event hook serves. Main thread only (set in the constructor, cleared
/// in stop(), read from the hook, all on the thread that runs readline()).
ListResizeWatcher::Impl* g_activeListWatcher = nullptr;

/// `rl_event_hook`: repaint if a resize is pending, then chain to the previous hook
/// (e.g. the pending-refresh hook) so it keeps working while a watcher is active.
int listResizeHook() {
    ListResizeWatcher::Impl* w = g_activeListWatcher;
    if (!w) return 0;
    w->tick();
    return w->prevHook ? w->prevHook() : 0;
}

} // namespace

ListResizeWatcher::ListResizeWatcher(Repaint repaint)
    : impl_(std::make_unique<Impl>(std::move(repaint))) {
    impl_->prevHook = rl_event_hook;
    if (impl_->prevHook == listResizeHook) impl_->prevHook = nullptr;   // never chain to ourselves
    g_activeListWatcher = impl_.get();
    rl_event_hook = listResizeHook;
    impl_->thread = std::thread([p = impl_.get()] { p->loop(); });
}

ListResizeWatcher::~ListResizeWatcher() { stop(); }

void ListResizeWatcher::stop() {
    if (!impl_ || !impl_->thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->stopRequested = true;
    }
    impl_->cv.notify_all();
    impl_->thread.join();

    if (g_activeListWatcher == impl_.get()) {
        g_activeListWatcher = nullptr;
        // Only undo our own installation; leave it alone if someone replaced the hook since.
        if (rl_event_hook == listResizeHook) rl_event_hook = impl_->prevHook;
    }
}
