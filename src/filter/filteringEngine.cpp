// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Project Headers
#include "isocmd/filter/filter.h"
#include "isocmd/ui/display.h"

/**
 * @brief Translates a filter match's local index (relative to the current
 * display list) into its corresponding globalIsoFileList index.
 *
 * filteringStack[lvl].originalIndices always stores fully resolved global
 * indices at every level (applyFilterCore builds each level from the previous
 * one's already-global indices). On an ISO list refresh only the top level is
 * rewritten (see refreshFilterKeepingResults), and only the top level is ever
 * read, so resolving a local index requires one lookup against the top of the
 * stack; it must not walk back through earlier levels, whose stored indices
 * may be stale.
 *
 * @param localIdx Index into the list currently being searched/displayed.
 * @return The corresponding index into globalIsoFileList.
 */
size_t resolveGlobalIndex(size_t localIdx) {
    if (!filteringStack.empty()) {
        const auto& top = filteringStack.back().originalIndices;
        if (localIdx < top.size())
            return top[localIdx];
    }
    return localIdx;
}

/**
 * @brief Derives the short "unmount key" label used to match/display an
 * already-mounted ISO (basename with any trailing "~hash" suffix stripped).
 * Shared by @c applyFilterCore and the live filter preview so both stay in
 * sync on exactly what text is being matched against.
 *
 * @param path Full or partial path to an ISO's mount entry.
 * @return The entry's basename with any trailing "~hash" suffix removed.
 */
std::string extractUnmountKey(const std::string& path) {
    size_t lastSlash = path.find_last_of('/');
    std::string name = (lastSlash != std::string::npos)
        ? path.substr(lastSlash + 1)
        : path;

    size_t lastTilde = name.find_last_of('~');
    return (lastTilde != std::string::npos)
        ? name.substr(0, lastTilde)
        : name;
}

/**
 * @brief Converts a single source path into its searchable representation.
 *
 * When neither unmount-key nor filename-only mode is active, returns the
 * original path unchanged.
 *
 * @param path          Source path or identifier from the active list.
 * @param useNameOnly   True to match only the basename after the last '/'.
 * @param useUnmountKey True to match the derived unmount key.
 * @return Searchable string used during query evaluation.
 */
static std::string formatSearchCandidate(
    const std::string& path,
    bool useNameOnly,
    bool useUnmountKey)
{
    if (useUnmountKey)
        return extractUnmountKey(path);

    if (!useNameOnly)
        return path;

    const size_t lastSlash = path.find_last_of('/');
    return (lastSlash != std::string::npos)
        ? path.substr(lastSlash + 1)
        : path;
}

/**
 * @brief Evaluates @p searchString against @p sourceList and collects matches.
 *
 * Projects entries into searchable strings when required, runs
 * @c filterFilesIndices, and populates @p outFiltered and @p outIndices with
 * the corresponding source entries and their local indices.
 *
 * @param sourceList       Source entries to filter against.
 * @param searchString     Filter query passed to @c filterFilesIndices.
 * @param useNameOnly      True to filter by filename component only.
 * @param useUnmountKey    True to filter by unmount key.
 * @param[out] outFiltered Matched source strings in result order.
 * @param[out] outIndices  Local indices into @p sourceList for each match.
 * @return True if at least one match was found; false otherwise.
 */
static bool collectFilterMatches(
    const std::vector<std::string>& sourceList,
    const std::string& searchString,
    bool useNameOnly,
    bool useUnmountKey,
    std::vector<std::string>& outFiltered,
    std::vector<size_t>& outIndices)
{
    std::vector<size_t> matchedIndices;

    if (useNameOnly || useUnmountKey) {
        std::vector<std::string> derived;
        derived.reserve(sourceList.size());

        for (const auto& path : sourceList)
            derived.push_back(formatSearchCandidate(path, useNameOnly, useUnmountKey));

        matchedIndices = filterFilesIndices(derived, searchString);
    } else {
        matchedIndices = filterFilesIndices(sourceList, searchString);
    }

    if (matchedIndices.empty())
        return false;

    outFiltered.reserve(matchedIndices.size());
    outIndices.reserve(matchedIndices.size());

    for (size_t idx : matchedIndices) {
        outFiltered.push_back(sourceList[idx]);
        outIndices.push_back(idx);
    }

    return true;
}

/**
 * @brief Commits a non-trivial filtered result set to UI and stack state.
 *
 * Resets pagination, requests a screen clear, replaces @c ctx.files, maps
 * local indices back to global database indices, and pushes a new
 * @c FilteringState entry onto @c filteringStack.
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
    const std::vector<size_t>& matchedIndices)
{
    ctx.currentPage  = 0;
    ctx.needsClrScrn = true;
    ctx.files        = std::move(filteredFiles);

    FilteringState newState;
    newState.originalIndices.reserve(matchedIndices.size());
    newState.query      = searchString;
    newState.namesOnly  = useNameOnly;
    newState.isFiltered = true;

    for (size_t idx : matchedIndices)
        newState.originalIndices.push_back(resolveGlobalIndex(idx));

    filteringStack.push_back(std::move(newState));
    ctx.isFiltered = true;
}

/**
 * @brief Executes the core filter commit: transforms source paths into
 * searchable strings (filename only, unmount keys, or raw paths), runs the
 * query, and chains the result through @c filteringStack so local indices map
 * back to global database indices. Resets pagination and marks the screen for
 * refresh.
 *
 * This is the COMMIT path, run when the user presses Enter. It is separate
 * from the live preview, which never touches @c filteringStack or @c ctx.files.
 * It does no terminal output or cursor movement; the caller owns that.
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

    std::vector<std::string> tempFiltered;
    std::vector<size_t> tempIndices;

    if (!collectFilterMatches(
            sourceList,
            searchString,
            useNameOnly,
            useUnmountKey,
            tempFiltered,
            tempIndices)) {
        return false;
    }

    if (tempFiltered.empty())
        return false;

    if (tempFiltered.size() == sourceList.size())
        return true; // Matches everything: nothing to filter.

    commitFilteredResults(
        searchString,
        ctx,
        useNameOnly,
        std::move(tempFiltered),
        tempIndices);

    return true;
}
