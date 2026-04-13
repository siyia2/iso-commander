// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
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
 * @brief Extracts and optionally shortens directory components and a filename from a path.
 * @details If "Full List" mode is disabled for the given location, directory components 
 * are truncated at separators or a max length of 16 characters to save screen space.
 * Results are cached in transformationCache to avoid re-processing.
 * * @param path The full filesystem path.
 * @param location The context (e.g., "mount", "write") used to check display settings.
 * @return A pair containing the (potentially processed) directory and the raw filename.
 */
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location) {
    auto lastSlashPos = path.find_last_of("/\\");
    
    // Extract filename (everything after the last slash)
    std::string filename = (lastSlashPos == std::string_view::npos) ? 
                           std::string(path) : 
                           std::string(path.substr(lastSlashPos + 1));
    
    if (lastSlashPos == std::string_view::npos) {
        return {"", std::move(filename)};
    }
    
    std::string_view originalDir = path.substr(0, lastSlashPos);
    
    // Check if we should return the full, un-shortened path
    bool showFull = (displayConfig::toggleFullListMount && location == "mount") ||
                    (displayConfig::toggleFullListCpMvRm && location == "cp_mv_rm") ||
                    (displayConfig::toggleFullListConvert2iso && location == "convert2iso") ||
                    (displayConfig::toggleFullListWrite2usb && location == "write2usb");

    if (showFull) {
        return {std::string(originalDir), std::move(filename)};
    }
    
    // Cache lookup for processed directory paths
    std::string fullPathKey(path);
    if (auto it = transformationCache.find(fullPathKey); it != transformationCache.end()) {
        return {it->second, std::move(filename)};
    }
    
    std::string processedDir;
    processedDir.reserve(originalDir.size());
    
    size_t start = 0;
    while (start < lastSlashPos) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos || end > lastSlashPos) end = lastSlashPos;
        
        std::string_view component = path.substr(start, end - start);
        size_t truncatePos = std::min<size_t>(16, component.size());

        // Logic: Shorten component at the first special character (space, dot, dash, etc.)
        for (size_t i = 0; i < truncatePos; ++i) {
            char c = component[i];
            if (c == ' ' || c == '-' || c == '_' || c == '.') {
                // If it's a hidden file (starts with .), keep the dot and the first char
                if (i == 0 && component.size() > 1) continue; 
                truncatePos = i;
                break;
            }
        }

        processedDir.append(component.substr(0, truncatePos));
        
        if (end < lastSlashPos) {
            processedDir.push_back('/');
        }
        start = end + 1;
    }
    
    transformationCache[fullPathKey] = processedDir;
    return {processedDir, std::move(filename)};
}

/**
 * @brief Parses a complex mount point name into structural components.
 * @details Expected format: "directory_filename~hash". 
 * Used primarily for unmount displays to style different parts of the path.
 * * @param dir The directory string view to parse.
 * @return A tuple of {DirectoryPart_, FilenamePart, ~HashPart}.
 */
std::tuple<std::string, std::string, std::string> parseMountPointComponents(std::string_view dir) {
    std::string dir_str(dir);
    if (auto it = cachedParsesForUmount.find(dir_str); it != cachedParsesForUmount.end()) {
        return it->second;
    }
    
    size_t underscorePos = dir.find('_');
    if (underscorePos == std::string_view::npos) {
        return cachedParsesForUmount[dir_str] = {dir_str, "", ""};
    }
    
    std::string directoryPart(dir.substr(0, underscorePos + 1));
    size_t lastTildePos = dir.find_last_of('~');
    
    if (lastTildePos == std::string_view::npos || lastTildePos <= underscorePos) {
        return cachedParsesForUmount[dir_str] = {directoryPart, std::string(dir.substr(underscorePos + 1)), ""};
    }
    
    std::string filenamePart(dir.substr(underscorePos + 1, lastTildePos - underscorePos - 1));
    std::string hashPart(dir.substr(lastTildePos));
    
    return cachedParsesForUmount[dir_str] = {directoryPart, filenamePart, hashPart};
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
