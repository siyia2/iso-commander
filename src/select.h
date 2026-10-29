// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SELECT_H
#define SELECT_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Project Headers
#include "./state.h"

// Forward declaration of shared state
struct RefreshState;

// ======================================================================
// RAII Guards for Select Functions
// ======================================================================
//
// These guards manage terminal state, readline keybindings, atomic flags,
// and cursor position across the interactive TUI selection functions
// (selectForIsoFiles, selectForImageFiles). They eliminate manual cleanup
// calls and guarantee proper teardown on all exit paths, including early
// returns, loop breaks, and stack unwinding from exceptions.
// ======================================================================

/**
 * @brief Installs the select-mode readline keybindings on construction and
 *        resets them on destruction.
 *
 * The destructor also clears @c RetainAndRestoreReadlineBuffer::g_rl_pending_text,
 * so a pending prompt-text restore from a previous session cannot leak into
 * the caller. Call @ref restore after any sub-mode that replaces the selection
 * keybindings (e.g. the filter flow via @c runSharedFilterFlow) so the select
 * prompt keeps its own bindings.
 */
class ReadlineKeybindingGuard {
public:
    ReadlineKeybindingGuard() {
        setup_custom_keybindingsForSelect();
    }

    ~ReadlineKeybindingGuard() {
        reset_custom_keybindingsForSelect();
        RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
    }

    /// Re-establish custom keybindings after a sub-mode overrode them.
    void restore() {
        setup_custom_keybindingsForSelect();
    }

    // Non-copyable, non-movable
    ReadlineKeybindingGuard(const ReadlineKeybindingGuard&) = delete;
    ReadlineKeybindingGuard& operator=(const ReadlineKeybindingGuard&) = delete;
};

/**
 * @brief Sets an @c std::atomic<bool> to @p activeValue on construction and
 *        restores the previous value on destruction (unless released).
 *
 * The previous value is captured at construction time, so nested guards and
 * pre-existing values round-trip correctly; the flag is not simply flipped.
 * Call @ref release to leave the flag at @p activeValue.
 */
class AtomicFlagGuard {
public:
    explicit AtomicFlagGuard(std::atomic<bool>& flag, bool activeValue = true)
        : flag_(flag),
          restoreValue_(flag.load(std::memory_order_acquire)),
          released_(false) {
        flag_.store(activeValue, std::memory_order_release);
    }

    ~AtomicFlagGuard() {
        if (!released_) {
            flag_.store(restoreValue_, std::memory_order_release);
        }
    }

    /// Leave the flag at @p activeValue on destruction instead of restoring.
    void release() { released_ = true; }

    AtomicFlagGuard(const AtomicFlagGuard&) = delete;
    AtomicFlagGuard& operator=(const AtomicFlagGuard&) = delete;

private:
    std::atomic<bool>& flag_;
    bool restoreValue_;
    bool released_;
};

/**
 * @brief Restores @c isAtISOList to its pre-scope value on destruction
 *        (unless the operation is an unmount) and always clears the global
 *        filtering indicator.
 *
 * The unmount exemption is per-instance and re-evaluated at destruction
 * time via @p isUnmountCheck, so a long-lived guard can observe a change in
 * whether the current operation is an unmount.
 *
 * The filtering indicator (@c GlobalState::g_filteringIndicator) is cleared
 * unconditionally, regardless of whether the flag above was restored, so a
 * failed filter cannot leave the "locked during sync" banner stuck on
 * screen after the guard goes out of scope.
 */
class IsoListStateGuard {
public:
    IsoListStateGuard(std::atomic<bool>& isAtISOList,
                       const std::function<bool()>& isUnmountCheck)
        : isAtISOList_(isAtISOList),
          wasAtList_(isAtISOList.load(std::memory_order_acquire)),
          isUnmountCheck_(isUnmountCheck) {}

    ~IsoListStateGuard() {
        if (!isUnmountCheck_()) {
            isAtISOList_.store(wasAtList_, std::memory_order_release);
        }

        GlobalState::g_filteringIndicator.store(false, std::memory_order_release);
    }

    IsoListStateGuard(const IsoListStateGuard&) = delete;
    IsoListStateGuard& operator=(const IsoListStateGuard&) = delete;

private:
    std::atomic<bool>& isAtISOList_;
    bool wasAtList_;
    std::function<bool()> isUnmountCheck_;
};

// --- Data Loading & List Display ---

/// Loads and displays the ISO list from the database / cache.
bool loadAndDisplayIso(
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    const std::string &listSubType,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/// Loads and displays currently mounted ISOs.
bool loadAndDisplayMountedISOs(
    std::vector<std::string> &isoDirs,
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    size_t &originalPage,
    std::shared_ptr<RefreshState> state
);

/// Loads and displays non-ISO image files (MDF, NRG, CHD, DAA/GBI, BIN/IMG)
/// for conversion.
void loadAndDisplayImageFiles(
    std::vector<std::string> &files,
    const std::string &fileType,
    bool &need2Sort,
    bool &isFiltered,
    bool &list,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    size_t &currentPage,
    std::shared_ptr<RefreshState> state
);


// --- Input Processing & Action Handlers ---

/// Processes a user selection for Mount or Unmount.
void processInputForMountOrUmount(
    const std::string &input,
    const std::vector<std::string> &files,
    bool &umountMvRmBreak,
    bool &verbose,
    bool isUnmount
);

/// Processes a user selection for Copy, Move, or Remove.
void processInputForCpMvRm(
    const std::string &input,
    const std::vector<std::string> &isoFiles,
    const std::string &process,
    bool &umountMvRmBreak,
    bool &filterHistory,
    bool &verbose
);

/// Processes a user selection for converting images to ISO.
void processInputForConversions(
    const std::string &input,
    std::vector<std::string> &fileList,
    const bool &modeMdf,
    const bool &modeNrg,
    const bool &modeChd,
    const bool &modeDaa,
    bool &verbose
);

/// Handles the write-to-USB flow for a user selection.
void writeToUsb(
    const std::string &input,
    const std::vector<std::string> &isoFiles
);


// --- Filtering & Pagination ---

/// Handles pagination keys, help ('?'), and display toggles ('*', '~').
bool processPaginationHelpAndDisplay(
    const std::string& inputString,
    size_t& totalPages,
    size_t& currentPage,
    bool& isFiltered,
    bool& needsClrScrn,
    const std::string& operation,
    bool& need2Sort,
    std::atomic<bool>* isAtISOList,
    std::vector<std::string>& filteredFiles
);

/// Filter entry point for the ISO list (mount / unmount / cp / mv / rm /
/// write2usb operations).
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles,
    bool& isFiltered, bool& needsClrScrn, bool& filterHistory,
    const std::string& operation, const std::string& operationColor,
    const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage,
    std::shared_ptr<RefreshState> state,
    std::function<void()> repaintList = nullptr);

/// Filter entry point for the convert-to-ISO image list.
void handleFilteringConvert2ISO(const std::string& inputString, std::vector<std::string>& files,
    const std::string& operation, bool& isFiltered, bool& needsClrScrn,
    bool& filterHistory, bool& need2Sort, size_t& currentPage,
    std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
    std::shared_ptr<RefreshState> state,
    std::function<void()> repaintList = nullptr);


// --- Utility & Cleanup ---

/// Displays the results of a selection session.
void handleSelectIsoFilesResults(
    const std::string &operation,
    bool &verbose,
    bool &isFiltered,
    bool &umountMvRmBreak
);

#endif // SELECT_H
