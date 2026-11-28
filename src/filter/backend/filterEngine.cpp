// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.filter.filter;
import isocmd.ui.display.display;

std::vector<FilteringState> filteringStack;

/**
 * @brief Maps an index in the filtered view to its source-list index.
 *
 * Each committed filter searches the full source list and replaces the
 * previous result. The current filter stores source indices directly, so
 * only one lookup is needed. With no valid mapping, return localIdx unchanged.
 */
std::size_t resolveGlobalIndex(std::size_t localIdx) {

	if (!filteringStack.empty()) {
		const auto& top = filteringStack.back().originalIndices;
		if (localIdx < top.size())
		    return top[localIdx];
	}
	return localIdx;

}

/**
 * @brief Borrows the full path, basename, or basename without its final ~hash.
 * @note The returned view must not outlive the source path.
 */
std::string_view filterTargetView(std::string_view path, bool namesOnly, bool unmountKey) {

	if (!namesOnly && !unmountKey)
	    return path;

	const auto slash = path.find_last_of('/');
	if (slash != std::string_view::npos)
	    path.remove_prefix(slash + 1);

	if (unmountKey) {
		const auto tilde = path.find_last_of('~');
		if (tilde != std::string_view::npos)
		    path = path.substr(0, tilde);
	}

	return path;

}

/// Returns the basename-based unmount key as an owned string.
std::string extractUnmountKey(const std::string& path) {

	return std::string(filterTargetView(path, false, true));

}

/**
 * @brief Collects matching source indices without copying candidate strings.
 */
static std::vector<std::size_t> collectFilterMatches(
    const std::vector<std::string>& sourceList,
    const std::string& searchString,
    bool useNameOnly,
    bool useUnmountKey)
{

	if (!useNameOnly && !useUnmountKey)
	    return filterFilesIndices(sourceList, searchString);

	std::vector<std::string_view> derived;
	derived.reserve(sourceList.size());
	for (const auto& path : sourceList)
	    derived.push_back(filterTargetView(path, useNameOnly, useUnmountKey));

	return filterViewIndices(derived, searchString);

}

/**
 * @brief Commits a non-trivial filtered result set to UI and stack state.
 *
 * Each commit REPLACES the active filter rather than stacking on top of it,
 * so the stack is cleared before the new level is pushed. Resets pagination,
 * requests a screen clear, replaces @c ctx.files, maps local indices back to
 * global database indices, and pushes a new @c FilteringState entry onto
 * @c filteringStack.
 *
 * @param searchString  Original filter query stored for state recovery.
 * @param[in,out] ctx   Filter context updated in place.
 * @param useNameOnly   Recorded in the pushed @c FilteringState.
 * @param filteredFiles Matched entries moved into @c ctx.files.
 * @param matchedIndices Local indices to resolve into global indices.
 */
static void commitFilteredResults(
    const std::string& searchString,
    FilterContext& ctx,
    bool useNameOnly,
    std::vector<std::string>&& filteredFiles,
    const std::vector<std::size_t>& matchedIndices)
{

	ctx.currentPage  = 0;
	ctx.needsClrScrn = true;
	ctx.files        = std::move(filteredFiles);

	// Every commit searches the original source, so previous levels are no
	// longer part of the current match. Drop them to keep the stack a single
	// description of the active filter.
	filteringStack.clear();

	FilteringState newState;
	newState.originalIndices.reserve(matchedIndices.size());
	newState.query      = searchString;
	newState.namesOnly  = useNameOnly;
	newState.isFiltered = true;

	for (std::size_t idx : matchedIndices)
	    newState.originalIndices.push_back(resolveGlobalIndex(idx));

	filteringStack.push_back(std::move(newState));
	ctx.isFiltered = true;

}

/**
 * @brief Executes the core filter commit: transforms source paths into
 * searchable strings (filename only, unmount keys, or raw paths), runs the
 * query, and records the result in @c filteringStack so local indices map
 * back to global database indices. Resets pagination and marks the screen for
 * refresh.
 *
 * This is the COMMIT path, run when the user presses Enter. It is separate
 * from the live preview, which never touches @c filteringStack or @c ctx.files.
 * It does no terminal output or cursor movement; the caller owns that.
 *
 * The search source is @c ctx.sourceOverride when set (the original,
 * unfiltered list), falling back to @c ctx.files otherwise. Entry points are
 * expected to set @c sourceOverride so each commit re-searches the original
 * list instead of narrowing the previous result.
 *
 * @param searchString The query to filter by (saved for state recovery).
 * @param ctx FilterContext providing source lists, unmount flags, and UI state.
 * @return True if matches were found and the filter stack was updated; false
 *         if the query is empty or no matches exist. Also returns true,
 *         without pushing a new level, when the query matches every entry.
 */
bool applyFilterCore(const std::string& searchString, FilterContext& ctx) {

	if (searchString.empty())
	    return false;

	const std::vector<std::string>& sourceList =
	    ctx.sourceOverride ? *ctx.sourceOverride : ctx.files;

	const bool useNameOnly   = displayConfig::toggleNamesOnly && !ctx.isUnmount;
	const bool useUnmountKey = ctx.isUnmount && !ctx.toggleFullListUmount;

	const auto tempIndices = collectFilterMatches(
	    sourceList, searchString, useNameOnly, useUnmountKey);

	if (tempIndices.empty())
	    return false;

	if (tempIndices.size() == sourceList.size())
	    return true; // Matches everything: nothing to filter.

	std::vector<std::string> tempFiltered;
	tempFiltered.reserve(tempIndices.size());
	for (std::size_t idx : tempIndices)
	    tempFiltered.push_back(sourceList[idx]);

	commitFilteredResults(
	    searchString,
	    ctx,
	    useNameOnly,
	    std::move(tempFiltered),
	    tempIndices);

	return true;

}
