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
 * @brief RAII guard that resets custom readline keybindings on scope exit.
 */
class ReadlineKeybindingGuard {
public:
    ReadlineKeybindingGuard() {
        setup_custom_keybindingsForSelect();
    }

    ~ReadlineKeybindingGuard() {
        reset_custom_keybindingsForSelect();
    }

    /**
     * Re-establish custom keybindings after they were overridden by
     * a sub-mode (e.g., filter flow). Call this after returning from
     * runSharedFilterFlow or any function that replaces the selection
     * keybindings.
     */
    void restore() {
        setup_custom_keybindingsForSelect();
    }

    // Non-copyable, non-movable
    ReadlineKeybindingGuard(const ReadlineKeybindingGuard&) = delete;
    ReadlineKeybindingGuard& operator=(const ReadlineKeybindingGuard&) = delete;
};

/**
 * @brief RAII guard for atomic boolean flag that restores on destruction.
 *
 * Sets the flag to @p activeValue on construction and restores it to
 * !activeValue on destruction (unless release() is called).
 */
class AtomicFlagGuard {
public:
    explicit AtomicFlagGuard(std::atomic<bool>& flag, bool activeValue = true)
        : flag_(flag), restoreValue_(!activeValue), released_(false) {
        flag_.store(activeValue, std::memory_order_release);
    }

    ~AtomicFlagGuard() {
        if (!released_) {
            flag_.store(restoreValue_, std::memory_order_release);
        }
    }

    /** Release ownership — the flag will not be restored on destruction. */
    void release() { released_ = true; }

    AtomicFlagGuard(const AtomicFlagGuard&) = delete;
    AtomicFlagGuard& operator=(const AtomicFlagGuard&) = delete;

private:
    std::atomic<bool>& flag_;
    bool restoreValue_;
    bool released_;
};

/**
 * @brief RAII guard for terminal cursor position.
 *
 * Optionally restores cursor to a saved position on destruction.
 */
class TerminalCursorGuard {
public:
    /** Saves current cursor position. */
    TerminalCursorGuard() {
        std::cout << "\033[s";  // Save cursor position
    }

    /** Restores cursor position. */
    ~TerminalCursorGuard() {
        std::cout << "\033[u";  // Restore cursor position
    }

    TerminalCursorGuard(const TerminalCursorGuard&) = delete;
    TerminalCursorGuard& operator=(const TerminalCursorGuard&) = delete;
};

/**
 * @brief RAII guard for controlling the "at ISO list" state.
 *
 * Ensures isAtISOList is restored to the desired value even on exception
 * or early return paths.
 */
class IsoListStateGuard {
public:
    IsoListStateGuard(std::atomic<bool>& isAtISOList,
                      const std::function<bool()>& isUnmountCheck)
        : isAtISOList_(isAtISOList),
          wasAtList_(isAtISOList.load(std::memory_order_acquire)),
          isUnmountCheck_(isUnmountCheck) {}

    ~IsoListStateGuard() {
        // Restore to appropriate state based on operation type
        if (!isUnmountCheck_()) {
            isAtISOList_.store(wasAtList_, std::memory_order_release);
        }
    }

    IsoListStateGuard(const IsoListStateGuard&) = delete;
    IsoListStateGuard& operator=(const IsoListStateGuard&) = delete;

private:
    std::atomic<bool>& isAtISOList_;
    bool wasAtList_;
    std::function<bool()> isUnmountCheck_;
};

// --- Data Loading & List Display ---

/**
 * Loads and displays the standard ISO list from the database/cache.
 */
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

/**
 * Specialized loader for currently mounted ISOs.
 */
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

/**
 * Loads and displays non-ISO image files (MDF, NRG, etc.) for conversion.
 */
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

/**
 * Processes user selection for Mount or Unmount operations.
 */
void processInputForMountOrUmount(
    const std::string &input,
    const std::vector<std::string> &files,
    bool &umountMvRmBreak,
    bool &verbose,
    bool isUnmount
);

/**
 * Processes user selection for Copy, Move, or Remove operations.
 */
void processInputForCpMvRm(
    const std::string &input,
    const std::vector<std::string> &isoFiles,
    const std::string &process,
    bool &umountMvRmBreak,
    bool &filterHistory,
    bool &verbose
);

/**
 * Processes user selection for converting images to ISO.
 */
void processInputForConversions(
    const std::string &input,
    std::vector<std::string> &fileList,
    const bool &modeMdf,
    const bool &modeNrg,
    const bool &modeChd,
    const bool &modeDaa,
    bool &verbose
);

/**
 * Handles specialized logic for writing an ISO to a USB device.
 */
void writeToUsb(
    const std::string &input,
    const std::vector<std::string> &isoFiles
);


// --- Filtering & Pagination ---

/**
 * Manages pagination state and help text display.
 */
bool processPaginationHelpAndDisplay(
    const std::string& command,
    size_t& totalPages,
    size_t& currentPage,
    bool& isFiltered,
    bool& needsClrScrn,
    const std::string& operation,
    bool& need2Sort,
    std::atomic<bool>* isAtISOList
);

/**
 * Generic string filtering for the ISO list.
 */
bool handleFilteringForISO(
    const std::string &inputString,
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    bool &needsClrScrn,
    bool &filterHistory,
    const std::string &operation,
    const std::string &operationColor,
    const std::vector<std::string> &isoDirs,
    bool isUnmount,
    size_t &currentPage
);

/**
 * Filtering specifically for the conversion list.
 */
void handleFilteringConvert2ISO(
    const std::string &mainInputString,
    std::vector<std::string> &files,
    const std::string &operation,
    bool &isFiltered,
    bool &needsClrScrn,
    bool &filterHistory,
    bool &need2Sort,
    size_t &currentPage
);


// --- Utility & Cleanup ---

/**
 * Finalizes and displays the results of a selection session.
 */
void handleSelectIsoFilesResults(
    const std::string &operation,
    bool &verbose,
    bool &isFiltered,
    bool &umountMvRmBreak
);

#endif // SELECT_H
