// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import isocmd.core.caches;
import isocmd.ui.display.display;

namespace GlobalCaches {
	thread_local std::unordered_map<
	    std::string,
	    std::string,
	    StringViewHash,
	    std::equal_to<>
	> transformationCache;
	thread_local std::unordered_map<
	    std::string,
	    std::tuple<std::string, std::string, std::string>,
	    StringViewHash,
	    std::equal_to<>
	> cachedParsesForUmount;
}

/**
 * @brief Converts a string to lowercase in-place.
 * @param str The string to modify.
 */
void toLowerInPlace(std::string& str) {

	for (char& c : str) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

}

/**
 * @brief Checks whether the full path should be displayed based on the UI location and global configuration flags.
 *
 * @param location A string key representing the specific UI context.
 * @return true if the full path should be displayed without shortening, false otherwise.
 */
static bool shouldShowFullPath(const std::string& location) {

	return (displayConfig::toggleFullListMount && location == "mount") ||
	       (displayConfig::toggleFullListCpMvRm && location == "cp_mv_rm") ||
	       (displayConfig::toggleFullListConvert2iso && location == "convert2iso") ||
	       (displayConfig::toggleFullListWrite2usb && location == "write2usb");

}

/**
 * @brief Shortens a directory path by truncating individual path components.
 *
 * @details Iterates through each component of the directory path and truncates it
 *          at a maximum length of 16 characters or at the first special character
 *          (' ', '-', '_', '.'), while preserving hidden files/directories.
 *
 * @param originalDir The original directory path string_view to shorten.
 * @return std::string The processed and shortened directory string.
 */
static std::string shortenDirectory(std::string_view originalDir) {

	std::string processedDir;
	processedDir.reserve(originalDir.size());

	std::size_t start = 0;
	while (start < originalDir.size()) {
		auto end = originalDir.find_first_of('/', start);
		if (end == std::string_view::npos) {
			end = originalDir.size();
		}

		std::string_view component = originalDir.substr(start, end - start);
		std::size_t truncatePos = std::min<std::size_t>(16, component.size());

		for (std::size_t i = 0; i < truncatePos; ++i) {
			char c = component[i];
			// Truncate at first special character
			if (c == ' ' || c == '-' || c == '_' || c == '.') {
				if (i == 0 && component.size() > 1) {
					continue; // Allow hidden files/directories starting with '.'
				}
				truncatePos = i;
				break;
			}
		}

		processedDir.append(component.substr(0, truncatePos));

		if (end < originalDir.size()) {
			processedDir.push_back('/');
		}
		start = end + 1;
	}

	return processedDir;

}

/**
 * @brief Decomposes a file path into a directory and filename, applying shortening logic to the directory.
 *
 * @details This function separates the filename from the directory path. Depending on the 'location'
 *          and global configuration, the directory path is either returned as-is or processed through a
 *          "shortening" algorithm that truncates path components at special characters or a maximum length.
 *
 *          To optimize performance and avoid redundant string manipulations, the results of shortened
 *          directories are stored in a global cache (GlobalCaches::transformationCache).
 *
 * @param path The full input path to process.
 * @param location A string key used to check if full-path display is toggled for specific UI contexts.
 * @return A pair of string_views:
 *         - first: The processed (shortened or original) directory.
 *         - second: The original filename.
 *
 * @note The directory string_view points to memory owned either by the input 'path' (if unshortened)
 *       or the 'GlobalCaches::transformationCache' (if shortened).
 */
std::pair<std::string_view, std::string_view> extractDirectoryAndFilename(std::string_view path, const std::string& location) {

	const auto lastSlashPos = path.find_last_of('/');

	// 1. Filename is always a slice of the input path
	const std::string_view filename = (lastSlashPos == std::string_view::npos) ?
	                                   path : path.substr(lastSlashPos + 1);

	if (lastSlashPos == std::string_view::npos) {
		return {"", filename};
	}

	const std::string_view originalDir = path.substr(0, lastSlashPos);

	// 2. Check for "Full Path" flags (no processing needed)
	if (shouldShowFullPath(location)) {
		return {originalDir, filename};
	}

	// 3. Fast-Path: Transparent Cache Lookup
	if (auto it = GlobalCaches::transformationCache.find(originalDir);
		it != GlobalCaches::transformationCache.end()) {
		return {std::string_view(it->second), filename};
	}

	// 4. Slow-Path: Process and Shorten (Executes once per unique directory)
	std::string processedDir = shortenDirectory(originalDir);

	// 5. Store result in cache and return a view of it
	auto [it, inserted] = GlobalCaches::transformationCache.emplace(
	    std::string(originalDir),
	    std::move(processedDir)
	);

	return {std::string_view(it->second), filename};

}

/**
 * @brief Parses a complex mount point name into structural components.
 * @details Expected format: "directoryISOs/filename~hash".
 * Used primarily for unmount displays to style different parts of the path.
 *
 * @param dir The directory string view to parse.
 * @return A tuple of {DirectoryISOs/Part, FilenamePart, ~HashPart}.
 */
std::tuple<std::string_view, std::string_view, std::string_view> parseMountPointComponents(std::string_view dir) {

	static constexpr std::string_view kDelim = "ISOs/";

	// 1. Transparent lookup: 0 allocations to check the cache
	if (auto it = GlobalCaches::cachedParsesForUmount.find(dir);
			 it != GlobalCaches::cachedParsesForUmount.end()) {
		const auto& [d, f, h] = it->second;
		return {d, f, h}; // Implicitly converts std::string to std::string_view
	}

	// 2. Logic: Find positions using string_view (no copies)
	std::size_t delimPos = dir.find(kDelim);
	if (delimPos == std::string_view::npos) {
		auto& stored = GlobalCaches::cachedParsesForUmount[std::string(dir)] = {std::string(dir), "", ""};
		return {std::get<0>(stored), "", ""};
	}

	std::size_t lastTildePos = dir.find_last_of('~');
	std::size_t afterDelim = delimPos + kDelim.size();

	std::string directoryPart(dir.substr(0, afterDelim));
	std::string filenamePart;
	std::string hashPart;

	if (lastTildePos == std::string_view::npos || lastTildePos <= afterDelim - 1) {
		filenamePart = std::string(dir.substr(afterDelim));
	} else {
		filenamePart = std::string(dir.substr(afterDelim, lastTildePos - afterDelim));
		hashPart = std::string(dir.substr(lastTildePos));
	}

	// 3. Store in cache once
	auto& stored = GlobalCaches::cachedParsesForUmount[std::string(dir)] =
	               {std::move(directoryPart), std::move(filenamePart), std::move(hashPart)};

	// 4. Return views of the strings now owned by the cache
	return {std::get<0>(stored), std::get<1>(stored), std::get<2>(stored)};

}

/**
 * @brief Cleans up a string by trimming edges and collapsing spaces around semicolons.
 * @return A cleaned version of the string.
 */
std::string trimWhitespace(const std::string& str) {

	std::size_t first = str.find_first_not_of(" \t\n\r\f\v");
	if (first == std::string::npos) return "";
	std::size_t last = str.find_last_not_of(" \t\n\r\f\v");

	std::string trimmed = str.substr(first, (last - first + 1));
	std::string result;
	result.reserve(trimmed.size());

	for (std::size_t i = 0; i < trimmed.length(); ++i) {
		if (trimmed[i] == ' ') {
			// Check neighbors to see if we are adjacent to a semicolon
			bool adjSemicolon = (i + 1 < trimmed.length() && trimmed[i + 1] == ';') ||
			                    (i > 0 && trimmed[i - 1] == ';');
			if (!adjSemicolon) {
				result += ' ';
			}
		} else {
			result += trimmed[i];
		}
	}

	return result;

}
