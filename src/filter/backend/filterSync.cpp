// SPDX-License-Identifier: GPL-3.0-or-later

// Preserves filter results and source indices across database refreshes.

import std;

import isocmd.core.lists;
import isocmd.filter.filter;

// ─── Filter stack sync ───────────────────────────────────────────────────

/**
 * @brief Extracts the searchable text representation of a path for a given mode.
 *
 * When @p namesOnly is @c false, the full path is returned. When @c true,
 * only the substring after the final '/' is returned (or the full string if
 * no '/' is present).
 *
 * @param path      Full file path to extract text from.
 * @param namesOnly @c true to match against the basename only; @c false for the full path.
 * @return The path or basename string used for filter matching.
 */
static std::string extractFilterTargetText(const std::string& path, bool namesOnly)
{

	if (!namesOnly) {
		return path;
	}

	const std::size_t slashPos = path.find_last_of('/');
	return (slashPos != std::string::npos) ? path.substr(slashPos + 1) : path;

}

/**
 * @brief Builds a lookup table mapping each path in @p globalList to its index.
 *
 * @param globalList Sorted list of global paths.
 * @return Hash map from path string to its index in @p globalList.
 */
static std::unordered_map<std::string, std::size_t> buildPathIndexMap(
    const std::vector<std::string>& globalList)
{

	std::unordered_map<std::string, std::size_t> indexOf;
	indexOf.reserve(globalList.size());

	for (std::size_t i = 0; i < globalList.size(); ++i) {
		indexOf.emplace(globalList[i], i);
	}

	return indexOf;

}

/**
 * @brief Collects new global indices that were not present before reload and
 *        satisfies every filter level in @p filteringStack.
 *
 * Evaluates one batched search per filter level using the @c namesOnly setting
 * recorded on each @c FilteringState.
 *
 * @param newGlobal      Freshly loaded, sorted global list.
 * @param oldPaths       Set of paths present prior to the reload.
 * @param filteringStack Active filter stack applied sequentially to new candidates.
 * @return Indices in @p newGlobal of newly added paths matching all filter levels.
 */
static std::vector<std::size_t> evaluateNewCandidates(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    const std::vector<FilteringState>& filteringStack)
{

	std::vector<std::size_t> candidates;
	for (std::size_t i = 0; i < newGlobal.size(); ++i) {
		if (oldPaths.find(newGlobal[i]) == oldPaths.end()) {
			candidates.push_back(i);
		}
	}

	for (const auto& lvl : filteringStack) {
		if (candidates.empty()) {
			break;
		}

		std::vector<std::string> texts;
		texts.reserve(candidates.size());
		for (std::size_t gi : candidates) {
			texts.push_back(extractFilterTargetText(newGlobal[gi], lvl.namesOnly));
		}

		const std::vector<std::size_t> hit = filterFilesIndices(texts, lvl.query);

		std::vector<std::size_t> next;
		next.reserve(hit.size());
		for (std::size_t h : hit) {
			next.push_back(candidates[h]);
		}
		candidates = std::move(next);
	}

	return candidates;

}

/**
 * @brief Updates filtered results after the ISO database is reloaded.
 *
 * Keep existing displayed paths if they still exist, without testing them
 * again. Test only newly discovered paths using the query and filename-only
 * setting saved at commit time. Rebuild source indices for the new list;
 * clear the filter if no results remain.
 *
 * @param oldPaths Paths present before the reload.
 * @param filteredFiles In/out: the displayed matches.
 * @param isFiltered In/out: cleared if no results remain.
 */
void refreshFilterKeepingResults(
    const std::vector<std::string>& newGlobal,
    const std::unordered_set<std::string>& oldPaths,
    std::vector<FilteringState>& filteringStack,
    std::vector<std::string>& filteredFiles,
    bool& isFiltered)
{

	if (!isFiltered || filteringStack.empty()) {
		return;
	}

	const std::unordered_map<std::string, std::size_t> indexOf = buildPathIndexMap(newGlobal);

	std::vector<std::size_t> result;
	result.reserve(filteredFiles.size());

	// 1) Existing results that still exist: kept untouched.
	for (const auto& p : filteredFiles) {
		const auto it = indexOf.find(p);
		if (it != indexOf.end()) {
			result.push_back(it->second);
		}
	}

	// 2) Newly discovered ISOs: must pass every level, each level using the
	//    text mode it was committed with. One batched search per level.
	std::vector<std::size_t> candidates = evaluateNewCandidates(newGlobal, oldPaths, filteringStack);
	result.insert(result.end(), candidates.begin(), candidates.end());

	// Global list is sorted, so ascending index == display order.
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());

	if (result.empty()) {
		filteringStack.clear();
		filteredFiles.clear();
		isFiltered = false;
		return;
	}

	filteredFiles.clear();
	filteredFiles.reserve(result.size());
	for (std::size_t idx : result) {
		filteredFiles.push_back(newGlobal[idx]);
	}

	filteringStack.back().originalIndices = std::move(result);

}

/**
 * @brief Refreshes source indices for the currently filtered ISO files.
 *
 * Maps each path in @p filteredFiles to its position in that list, then scans
 * @p globalIsoFileList once to find each path's current index there. The
 * resulting indices replace @c originalIndices of the last filtering state,
 * in the same order as @p filteredFiles. No filter queries are reapplied.
 *
 * The stack is left unmodified if @p filteringStack or @p filteredFiles is
 * empty, or if any filtered path is not found in @p globalIsoFileList (which
 * includes an empty global list).
 *
 * Keeps source indices synchronized after the global ISO list is reordered,
 * such as when toggling filenames-only display mode, while preserving the
 * current filtered results.
 *
 * @note Paths are assumed to be unique in both @p filteredFiles and
 *       @p globalIsoFileList. Only the first occurrence of a path in the
 *       global list is used.
 *
 * @param filteringStack    The stack of active filtering states; only the
 *                          last state's source indices are updated.
 * @param filteredFiles     The currently filtered ISO file paths. Must
 *                          outlive the call, since the lookup map holds
 *                          views into it.
 */
void refreshFilteredSourceIndices(
    const std::vector<std::string>& globalIsoFileList,
    std::vector<FilteringState>& filteringStack,
    const std::vector<std::string>& filteredFiles)
{

	if (filteringStack.empty() || filteredFiles.empty()) return;

	// Only the F filtered paths are stored; views avoid copying strings.
	std::unordered_map<std::string_view, std::size_t> wanted;
	wanted.reserve(filteredFiles.size());
	for (std::size_t k = 0; k < filteredFiles.size(); ++k)
	    wanted.emplace(filteredFiles[k], k);

	std::vector<std::size_t> currentIndices(filteredFiles.size());
	std::size_t found = 0;

	for (std::size_t i = 0; i < globalIsoFileList.size() && found < wanted.size(); ++i) {
		const auto it = wanted.find(globalIsoFileList[i]);
		if (it != wanted.end()) {
			currentIndices[it->second] = i;
			++found;
		}
	}

	if (found != wanted.size()) return;   // incomplete mapping, leave stack alone

	filteringStack.back().originalIndices = std::move(currentIndices);

}
