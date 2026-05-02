// SPDX-License-Identifier: GPL-3.0-or-later

// Project Headers
#include "../caches.h"
#include "../display.h"

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
 * @brief Decomposes a file path into a directory and filename, applying shortening logic to the directory.
 * 
 * @details This function separates the filename from the directory path. Depending on the 'location' 
 * and global configuration, the directory path is either returned as-is or processed through a 
 * "shortening" algorithm that truncates path components at special characters or a maximum length.
 * 
 * To optimize performance and avoid redundant string manipulations, the results of shortened 
 * directories are stored in a global cache (GlobalCaches::transformationCache).
 * 
 * @param path The full input path to process.
 * @param location A string key used to check if full-path display is toggled for specific UI contexts.
 * @return A pair of string_views:
 *         - first: The processed (shortened or original) directory.
 *         - second: The original filename.
 * 
 * @note The directory string_view points to memory owned either by the input 'path' (if unshortened) 
 * or the 'GlobalCaches::transformationCache' (if shortened).
 */
std::pair<std::string_view, std::string_view> extractDirectoryAndFilename(std::string_view path, const std::string& location) {
    const auto lastSlashPos = path.find_last_of("/\\");
    
    // 1. Filename is always a slice of the input path
    const std::string_view filename = (lastSlashPos == std::string_view::npos) ? 
                                       path : path.substr(lastSlashPos + 1);
    
    if (lastSlashPos == std::string_view::npos) {
        return {"", filename};
    }
    
    const std::string_view originalDir = path.substr(0, lastSlashPos);
    
    // 2. Check for "Full Path" flags (no processing needed)
    bool showFull = (displayConfig::toggleFullListMount && location == "mount") ||
                    (displayConfig::toggleFullListCpMvRm && location == "cp_mv_rm") ||
                    (displayConfig::toggleFullListConvert2iso && location == "convert2iso") ||
                    (displayConfig::toggleFullListWrite2usb && location == "write2usb");

    if (showFull) {
        return {originalDir, filename};
    }
    
    // 3. Fast-Path: Transparent Cache Lookup
    // find() now accepts string_view directly thanks to StringViewHash
    if (auto it = GlobalCaches::transformationCache.find(originalDir); 
             it != GlobalCaches::transformationCache.end()) {
        return {std::string_view(it->second), filename};
    }
    
    // 4. Slow-Path: Process and Shorten (Executes once per unique directory)
    std::string processedDir;
    processedDir.reserve(originalDir.size());
    
    size_t start = 0;
    while (start < originalDir.size()) {
        auto end = originalDir.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = originalDir.size();
        
        std::string_view component = originalDir.substr(start, end - start);
        size_t truncatePos = std::min<size_t>(16, component.size());

        for (size_t i = 0; i < truncatePos; ++i) {
            char c = component[i];
            // Truncate at first special character
            if (c == ' ' || c == '-' || c == '_' || c == '.') {
                if (i == 0 && component.size() > 1) continue; // Allow hidden files
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
    
    // 5. Store result in cache and return a view of it
    auto [it, inserted] = GlobalCaches::transformationCache.emplace(
        std::string(originalDir), 
        std::move(processedDir)
    );
    
    return {std::string_view(it->second), filename};
}

/**
 * @brief Parses a complex mount point name into structural components.
 * @details Expected format: "directory_filename~hash". 
 * Used primarily for unmount displays to style different parts of the path.
 * * @param dir The directory string view to parse.
 * @return A tuple of {DirectoryPart_, FilenamePart, ~HashPart}.
 */
std::tuple<std::string_view, std::string_view, std::string_view> parseMountPointComponents(std::string_view dir) {
    // 1. Transparent lookup: 0 allocations to check the cache
    if (auto it = GlobalCaches::cachedParsesForUmount.find(dir); 
             it != GlobalCaches::cachedParsesForUmount.end()) {
        const auto& [d, f, h] = it->second;
        return {d, f, h}; // Implicitly converts std::string to std::string_view
    }
    
    // 2. Logic: Find positions using string_view (no copies)
    size_t underscorePos = dir.find('_');
    if (underscorePos == std::string_view::npos) {
        auto& stored = GlobalCaches::cachedParsesForUmount[std::string(dir)] = {std::string(dir), "", ""};
        return {std::get<0>(stored), "", ""};
    }
    
    size_t lastTildePos = dir.find_last_of('~');
    
    std::string directoryPart(dir.substr(0, underscorePos + 1));
    std::string filenamePart;
    std::string hashPart;

    if (lastTildePos == std::string_view::npos || lastTildePos <= underscorePos) {
        filenamePart = std::string(dir.substr(underscorePos + 1));
    } else {
        filenamePart = std::string(dir.substr(underscorePos + 1, lastTildePos - underscorePos - 1));
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
 * @param str The input string.
 * @return A cleaned version of the string.
 */
std::string trimWhitespace(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    
    std::string trimmed = str.substr(first, (last - first + 1));
    std::string result;
    result.reserve(trimmed.size());

    for (size_t i = 0; i < trimmed.length(); ++i) {
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
