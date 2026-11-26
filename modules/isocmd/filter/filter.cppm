// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.filter.filter;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// Forward declaration of shared state (needed for live main-list rendering)
struct RefreshState;

/**
 * DATA STRUCTURES
 */

/**
 * @brief One search term, optionally negated with a leading unescaped '!'.
 *
 * Matching is case-sensitive if the term contains an uppercase letter;
 * otherwise the lowercase term is compared with lowercase candidate text.
 * Negation requires the term to be absent. Stored text excludes the '!'.
 */
struct QueryToken {
	std::string original;            ///< Term text as typed (escapes resolved, '!' prefix removed).
	std::string lower;               ///< Lowercased @c original; set only when !isCaseSensitive.
	bool isCaseSensitive = false;    ///< True if @c original contains an uppercase letter.
	bool negated = false;            ///< True if the term must NOT occur in the entry.
};

/// The current committed filter and its source-list indices.
struct FilteringState {
	std::vector<std::size_t> originalIndices;
	std::string query;
	bool isFiltered = false;
	bool namesOnly = false;
};

/// All mutable state needed to perform a single filter operation.
struct FilterContext {
	std::vector<std::string>& files;
	bool& isFiltered;
	bool& needsClrScrn;
	bool& filterHistory;
	std::size_t& currentPage;
	const std::vector<std::string>* sourceOverride = nullptr;
	bool isUnmount = false;
	bool toggleFullListUmount = false;
	std::function<void()> repaintList = nullptr;

	// Live main-list rendering: when listType is non-empty and pendingIndices,
	// hasPendingProcess, and state are all set, the filter prompt repaints the
	// real printList output on every keystroke, narrowing/widening it as the
	// query changes. Leaving listType empty disables this and keeps the
	// Enter-only prompt.
	std::string listType{};       // e.g. "ISO_FILES", "IMAGE_FILES", "MOUNTED_ISOS"
	std::string listSubType{};
	std::vector<std::string>* pendingIndices = nullptr;
	bool* hasPendingProcess = nullptr;
	std::shared_ptr<RefreshState> state = nullptr;
};

/// Translates a local filtered-list index into its global list index.
std::size_t resolveGlobalIndex(std::size_t localIdx);

/// Extracts the basename-based unmount key from a mounted ISO path.
std::string extractUnmountKey(const std::string& path);

/// Applies and commits a filter query to the supplied filter context.
bool applyFilterCore(const std::string& searchString, FilterContext& ctx);

/// Configuration for runSharedFilterFlow, describing one filter operation.
struct FilterCallConfig {
	std::vector<std::string>* files = nullptr;
	const std::vector<std::string>* sourceOverride = nullptr;
	std::string operation;
	std::string_view operationColor;
	bool* isFiltered = nullptr;
	bool* needsClrScrn = nullptr;
	bool* filterHistory = nullptr;
	bool* need2Sort = nullptr;
	std::size_t* currentPage = nullptr;
	bool isUnmount = false;
	bool toggleFullList = false;
	std::function<void()> repaintList = nullptr;

	// Optional live rendering: populate all four to repaint the real list
	// while the user types (see FilterContext above).
	std::string listType{};
	std::string listSubType{};
	std::vector<std::string>* pendingIndices = nullptr;
	bool* hasPendingProcess = nullptr;
	std::shared_ptr<RefreshState> state = nullptr;
};

/**
 * @brief Shared mutable filter state, optional operation hooks, and live preview context
 *        for filter entry points.
 * @details Groups caller-owned state pointers and optional callbacks passed into
 *          @c handleFilteringForISO and @c handleFilteringConvert2ISO so call sites
 *          do not repeat long argument lists.
 */
struct FilterEntryContext {
	bool* isFiltered = nullptr;                        /**< Points to caller's flag indicating whether the list is filtered. */
	bool* needsClrScrn = nullptr;                      /**< Points to caller's flag requesting a full screen redraw. */
	bool* filterHistory = nullptr;                     /**< Points to caller's flag indicating filter history should be saved. */
	std::size_t* currentPage = nullptr;                     /**< Points to caller's page index; may be reset after filtering. */
	bool* need2Sort = nullptr;                         /**< Optional pointer to flag requesting resort after filtering. */
	std::vector<std::string>* pendingIndices = nullptr;/**< Optional pointer to caller's pending-selection indices. */
	bool* hasPendingProcess = nullptr;                 /**< Optional pointer to flag indicating a staged process action. */
	std::shared_ptr<RefreshState> state = nullptr;     /**< Optional shared refresh state enabling live main-list preview. */
	std::function<void()> repaintList = nullptr;       /**< Optional callback to repaint normal list on resize when not previewing. */
};

/**
 * GLOBAL STATE
 */

/// Stores the current committed filter (zero or one entry).
extern std::vector<FilteringState> filteringStack;

/**
 * FILTERING LOGIC & SYNC
 */

/**
 * @brief Returns the indices of @p files matching @p query, using
 * SIMD-accelerated substring search.
 *
 * @param precomputedLower Optional array parallel to @p files holding each
 *        entry already lowercased. Callers that run the same list through many
 *        queries in a row (e.g. live filter preview, once per keystroke) can
 *        pass this to lowercase each entry once instead of per call. When
 *        nullptr, entries are lowercased on every call.
 */
std::vector<std::size_t> filterFilesIndices(const std::vector<std::string>& files, const std::string& query,
                                        const std::vector<std::string>* precomputedLower = nullptr);

/**
 * One OR-branch of a query: every positive term in the group must match
 * (AND semantics). A query is a list of such groups; an entry matches if ANY
 * group's positive terms are all satisfied (OR semantics). Negated terms
 * (see @c QueryToken::negated) are parsed into their group but applied
 * globally, to the whole query.
 */
using QueryGroup = std::vector<QueryToken>;

/// True if @p query ends in an unpaired backslash (a dangling escape).
bool hasDanglingBackslash(const std::string& query);

/// Parses a query into OR-ed groups of AND-ed terms (grammar documented in filterQuery.cpp).
std::vector<QueryGroup> buildQueryGroups(const std::string& query,
                                         bool* trailingEmptyGroup = nullptr);

/// True if any term in any group is negated ('!').
bool groupsHaveNegation(const std::vector<QueryGroup>& groups);

/// Like filterFilesIndices, but tests only the previously established
/// @p candidateIndices (incremental narrowing for the live preview).
std::vector<std::size_t> filterFilesIndicesSubset(
    const std::vector<std::string>& files,
    const std::vector<std::size_t>& candidateIndices,
    const std::string& query,
    const std::vector<std::string>* precomputedLower);

/**
 * @brief Refreshes an active filter after a list reload, preserving results.
 *
 * Entries already shown are kept if they still exist; newly discovered ISOs
 * are tested using the committed query and its saved text mode.
 * Clears the filter if nothing remains.
 */
void refreshFilterKeepingResults(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered);

/// Rebuilds the source indices for the current filtered results.
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles);

/**
 * LIVE FILTER PREVIEW
 */

/**
 * @brief Configures live-preview state for an interactive filter session.
 *
 * @param ctx Filter context supplying list-rendering configuration.
 * @param previewSource Source list displayed by the preview.
 */
void configureLivePreview(
    FilterContext& ctx,
    const std::vector<std::string>& previewSource);

/**
 * @brief Repaints the live preview after a terminal resize when enabled.
 *
 * @return True if the live preview handled the repaint; otherwise false.
 */
bool repaintLivePreviewOnResizeIfConfigured();

/// Resets incremental matching and scrolling state before a prompt read.
void resetLivePreviewIteration();

/// Marks the current live-preview session as inactive.
void deactivateLivePreview();

/**
 * @brief Reports whether the live preview has repainted at least once.
 *
 * @return True if a preview frame has been rendered during the current
 *         preview session; otherwise false.
 */
bool livePreviewWasRepainted();

/**
 * @brief Manages Readline hooks and keybindings for a live-preview session.
 *
 * Installs the preview redisplay hook and Page Up/Page Down bindings on
 * construction, then restores the original Readline configuration and
 * resets preview state on destruction.
 */
class LivePreviewReadlineGuard {
public:
	LivePreviewReadlineGuard();
	~LivePreviewReadlineGuard();

	LivePreviewReadlineGuard(const LivePreviewReadlineGuard&) = delete;
	LivePreviewReadlineGuard& operator=(const LivePreviewReadlineGuard&) = delete;
};

} // extern "C++"
