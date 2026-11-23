// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>

// Project Headers
#include "isocmd/ui/display.h"
#include "isocmd/core/globalMutexes.h"
#include "isocmd/core/cancellation.h"
#include "isocmd/util/stringManipulation.h"
#include "isocmd/ui/themes.h"
#include "isocmd/ops/umount.h"
#include "isocmd/ui/verbose.h"

namespace fs = std::filesystem;

/**
 * @brief Checks if a directory is empty.
 * @details Used primarily to determine if a mount point can be safely removed
 * if a umount2 call fails but the directory contains no data.
 * * @param path The absolute path to the directory.
 * @return True if the directory exists and contains no entries, false otherwise.
 */
static bool isDirectoryEmpty(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }

    return std::filesystem::directory_iterator(path) ==
           std::filesystem::directory_iterator();
}

/**
 * @brief Formats a mount point path for console output.
 * @details Breaks the path into segments using parseMountPointComponents.
 * Converts non-owning string_views into a styled, owning std::string based
 * on ANSI palette settings and user display configuration.
 *
 * @param isoDir The directory path to format.
 * @param fmt Reference to the VerboseMessageFormatter.
 * @param messageKey The key for the specific message template.
 * @return A fully constructed, styled string ready for display.
 */
static std::string formatDirText(const std::string& isoDir) {
    auto dirParts = parseMountPointComponents(isoDir);

    std::string formattedDir{std::get<1>(dirParts)};
    std::string_view squareColor = UI::Palette::DimGray;

    if (displayConfig::toggleFullListUmount) {
        formattedDir = std::string(std::get<0>(dirParts))
                       + formattedDir
                       + std::string(squareColor)
                       + std::string(std::get<2>(dirParts))
                       + std::string(UI::Palette::BoldReset);
    }

    return formattedDir;
}

static std::string formatDirForDisplay(const std::string& isoDir, VerboseMessageFormatter& fmt, const char* messageKey) {
    return fmt.format(messageKey, formatDirText(isoDir));
}

/**
 * @brief Performs unmount operations on a list of ISO mount points.
 *
 * Allocates a single libmount context (@c mnt_new_context) and reuses it
 * across all entries via @c mnt_reset_context, avoiding per-iteration
 * allocation overhead.  The context is managed by a RAII CtxGuard that
 * guarantees @c mnt_free_context on any exit path.
 *
 * Each mount point is unmounted with lazy detach (@c MNT_DETACH) and
 * automatic loop device cleanup (@c /dev/loopX).  Empty mount point
 * directories are removed after a successful unmount.
 *
 * Results are written directly to the global verboseSets:
 *   - verboseSets.operationCompleted  Success messages.
 *   - verboseSets.operationFailed     Failure messages (root_error, cancel, error).
 *
 * @param isoDirs        Vector of mount point directories to unmount.
 * @param completedTasks Atomic counter incremented for each successful unmount.
 * @param failedTasks    Atomic counter incremented for each failure
 *                       (including cancellation and context allocation failure).
 * @param silentMode     Suppresses all message generation; only counters are updated.
 *
 * @warning Requires root (geteuid() == 0). Without it every entry gets "root_error".
 *          If the initial context allocation fails, all entries are marked failed
 *          immediately.
 */
void unmountISO(
    const std::vector<std::string>& isoDirs,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    const bool hasRoot = (geteuid() == 0);
    // Results are kept as compact records (directory + message key) and turned into
    // lines only when they are printed; see forEachUmountLine().
    std::vector<UmountRecord> tempRecords;
    constexpr size_t kBatchSize = 50;
    if (!silentMode) tempRecords.reserve(kBatchSize);
    auto flushTemporaryBuffers = [&]() {
        if (silentMode || tempRecords.empty()) return;
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
        verboseSets.umountRecords.insert(verboseSets.umountRecords.end(),
                                         std::make_move_iterator(tempRecords.begin()),
                                         std::make_move_iterator(tempRecords.end()));
        tempRecords.clear();
    };
    auto record = [&](const std::string& isoDir, const char* messageKey, bool completed) {
        if (!silentMode) tempRecords.push_back({isoDir, messageKey, completed});
    };
    auto maybeFlush = [&]() {
        if (!silentMode && tempRecords.size() >= kBatchSize) flushTemporaryBuffers();
    };

    if (!hasRoot) {
        if (!silentMode) {
            for (const auto& isoDir : isoDirs) record(isoDir, "root_error", false);
            flushTemporaryBuffers();
        }
        failedTasks->fetch_add(isoDirs.size(), std::memory_order_relaxed);
        return;
    }
    for (const auto& isoDir : isoDirs) {
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) {
            record(isoDir, "cancel", false);
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            maybeFlush();
            continue;
        }

        // Allocate isolated libmount context
        libmnt_context* ctx = mnt_new_context();
        if (!ctx) {
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            record(isoDir, "error", false);
            maybeFlush();
            continue;
        }

        // Configure target, lazy unmount (MNT_DETACH), and loop cleanup
        mnt_context_set_target(ctx, isoDir.c_str());
        mnt_context_enable_lazy(ctx, 1);    // Matches MNT_DETACH
        mnt_context_enable_loopdel(ctx, 1); // Cleans up /dev/loopX

        // Execute the unmount operation
        const int result = mnt_context_umount(ctx);
        mnt_free_context(ctx);

        // Handle results and directory removal verbose output
        if (result == 0 || isDirectoryEmpty(isoDir)) {
            rmdir(isoDir.c_str());
            completedTasks->fetch_add(1, std::memory_order_relaxed);
            record(isoDir, "success", true);
        } else {
            failedTasks->fetch_add(1, std::memory_order_relaxed);
            record(isoDir, "error", false);
        }
        maybeFlush();
    }
    flushTemporaryBuffers();
}

// ---------------------------------------------------------------------------
// Unmount results: records are turned into lines only when printed.
// ---------------------------------------------------------------------------
namespace {

/// Case-insensitive three-way compare, the same ordering verbosePrint() uses.
int compareIgnoreCase(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i]));
        const int y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

struct UmountLineRef {
    std::string key;   // displayed directory text + the bytes that follow it in the line
    size_t      index; // position in verboseSets.umountRecords
};

} // namespace

size_t countUmountRecords(bool completed) {
    return static_cast<size_t>(std::count_if(
        verboseSets.umountRecords.begin(), verboseSets.umountRecords.end(),
        [completed](const UmountRecord& r) { return r.completed == completed; }));
}

void forEachUmountLine(bool completed, const std::function<void(std::string_view)>& fn) {
    const std::vector<UmountRecord>& records = verboseSets.umountRecords;
    VerboseMessageFormatter formatter;

    // In a printed line the directory text is followed by a fixed colour code and a
    // quote (different for success and failure lines). Comparing directory + that
    // suffix orders lines like comparing the full lines would, up to the final tag.
    std::string suffix(completed ? std::string_view(formatter.tc.path)
                                 : std::string_view(formatter.tc.warning));
    suffix.push_back('\'');

    std::vector<UmountLineRef> refs;
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].completed == completed)
            refs.push_back({formatDirText(records[i].mountPoint) + suffix, i});
    if (refs.empty()) return;

    auto line = [&](size_t i) {
        return formatDirForDisplay(records[i].mountPoint, formatter, records[i].messageKey);
    };

    // Ties (same directory text, e.g. different failure tags) are broken by the full
    // line, so repeated lines end up next to each other and can be dropped, as the
    // old string sets did.
    std::sort(refs.begin(), refs.end(), [&](const UmountLineRef& a, const UmountLineRef& b) {
        if (int c = compareIgnoreCase(a.key, b.key)) return c < 0;
        if (a.index == b.index) return false;
        const std::string la = line(a.index);
        const std::string lb = line(b.index);
        if (int c = compareIgnoreCase(la, lb)) return c < 0;
        return la < lb;
    });

    std::string previous;
    bool havePrevious = false;
    for (const UmountLineRef& ref : refs) {
        std::string current = line(ref.index);
        if (havePrevious && current == previous) continue;
        fn(current);
        previous = std::move(current);
        havePrevious = true;
    }
}
