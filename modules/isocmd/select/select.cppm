// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.select.select;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- select/select ----

// Forward declarations
struct RefreshState;
struct FilterEntryContext;

// --- Data Loading & List Display ---

/// While an ISO select screen is active, projects its staged selection (held as
/// paths) onto pendingIndices as source indices, merged into ranges, against the
/// list as it is right now. Returns false when no select screen is active, so
/// the caller manages its own pending state. Called by the ISO / mounted-ISO
/// loaders right before they print.
bool syncIsoPendingDisplay(std::vector<std::string>& pendingIndices,
                           bool& hasPendingProcess,
                           bool importRunning);

/// Loads and displays the ISO list from the database / cache.
bool loadAndDisplayIso(
    std::vector<std::string> &filteredFiles,
    bool &isFiltered,
    const std::string &listSubType,
    bool &umountMvRmBreak,
    std::vector<std::string> &pendingIndices,
    bool &hasPendingProcess,
    std::size_t &currentPage,
    std::size_t &originalPage,
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
    std::size_t &currentPage,
    std::size_t &originalPage,
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
    std::size_t &currentPage,
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
    std::size_t& totalPages,
    std::size_t& currentPage,
    bool& isFiltered,
    bool& needsClrScrn,
    const std::string& operation,
    bool& need2Sort,
    std::atomic<bool>* isAtISOList,
    std::vector<std::string>& filteredFiles
);

/**
 * @brief Filter entry point for ISO file operations (mount, unmount, etc.).
 * @details Resolves the correct source list based on the current filter and
 * unmount state, then forwards to @c runSharedFilterFlow. Source list
 * priority:
 * -# @p filteredFiles — if a filter is already active (@c ctx.isFiltered && @c *ctx.isFiltered)
 * -# @p isoDirs       — if in unmount mode with no active filter
 * -# @c GlobalState::globalIsoFileList — otherwise
 *
 * When @p ctx.state is provided it also enables the live main-list preview: the
 * list type is @c "MOUNTED_ISOS" in unmount mode and @c "ISO_FILES" otherwise,
 * and @c pendingIndices / @c hasPendingProcess / the list subtype are taken
 * from the shared @c RefreshState.
 *
 * @param inputString    Raw user input; must be exactly @c "/" to trigger filtering.
 * @param filteredFiles  The currently displayed (possibly already filtered) file list.
 * @param operation      Name of the ISO operation shown in the prompt (e.g. "Mount").
 * @param operationColor Raw ANSI escape code used to colorise @p operation in the prompt.
 * @param isoDirs        Mounted ISO paths used as the source list in unmount mode.
 * @param isUnmount      True when the caller is performing an unmount operation.
 * @param ctx            Shared filter/display state pointers, refresh state, and repaint callback.
 * @return @c true if @p inputString was handled as a filter command, @c false otherwise.
 */
bool handleFilteringForISO(const std::string& inputString,
                           std::vector<std::string>& filteredFiles,
                           const std::string& operation,
                           const std::string& operationColor,
                           const std::vector<std::string>& isoDirs,
                           bool isUnmount,
                           FilterEntryContext ctx);

/**
 * @brief Filter entry point for convert-to-ISO operations.
 *
 * Filtering is single-level and non-incremental: every prompt filters from
 * @p sourceFiles (the original unfiltered image list), never from @p files
 * (which may already be the current filtered view). A second filter
 * therefore replaces the previous result instead of narrowing it.
 *
 * @param inputString Original command input.
 * @param files       Image files to filter (the current display list).
 * @param sourceFiles Original unfiltered image list used as the filter
 *                    corpus on every prompt.
 * @param operation   Operation name.
 * @param ctx         Optional caller-owned filter state and callbacks.
 */
void handleFilteringConvert2ISO(const std::string& inputString,
                                std::vector<std::string>& files,
                                const std::vector<std::string>& sourceFiles,
                                const std::string& operation,
                                FilterEntryContext ctx);

// --- Utility & Cleanup ---

/// Displays the results of a selection session.
void handleSelectIsoFilesResults(
    const std::string &operation,
    bool &verbose,
    bool &isFiltered,
    bool &umountMvRmBreak
);

} // extern "C++"
