// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


// Conver strings to lowercase efficiently
void toLowerInPlace(std::string& str) {
    for (char& c : str) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
}


// For memory mapping string transformations
std::unordered_map<std::string, std::string> transformationCache;


// Function to extract directory and filename from a given path
std::pair<std::string, std::string>
extractDirectoryAndFilename(std::string_view path, const std::string& location) {
    // Find last slash or backslash
    auto lastSlashPos = path.find_last_of("/\\");
    
    // Extract a view of the filename (no allocation)
    std::string_view filenameView = path.substr(lastSlashPos + 1);
    
    if ((displayConfig::toggleFullListMount       && location == "mount")       ||
        (displayConfig::toggleFullListCpMvRm      && location == "cp_mv_rm")    ||
        (displayConfig::toggleFullListConversions && location == "conversions") ||
        (displayConfig::toggleFullListWrite       && location == "write")) {
        return { std::string(path.substr(0, lastSlashPos)),
                 std::string(filenameView) };
    }

    // Now create fullPath only when needed
    std::string fullPath(path); // Key for cache
    
    // Check cache after early returns
    if (auto it = transformationCache.find(fullPath); it != transformationCache.end()) {
        return { it->second, std::string(filenameView) };
    }

    // Build truncated directory
    std::string processedDir;
    size_t start = 0;
    while (start < lastSlashPos) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos || end > lastSlashPos) {
            end = lastSlashPos;
        }

        const size_t compLen = end - start;
        // Limit search to first 16 characters of the component
        const size_t truncationLimit = std::min(start + 16, end);
        size_t firstSpecial = std::string_view::npos;
        
        // Search for special characters within the first 16 chars
        for (size_t i = start; i < truncationLimit; ++i) {
            if (const char c = path[i]; c == ' ' || c == '-' || c == '_' || c == '.') {
                firstSpecial = i;
                break;
            }
        }

        size_t truncLen = (firstSpecial != std::string_view::npos) 
                          ? (firstSpecial - start) 
                          : std::min(compLen, size_t(16));
        
        processedDir.append(path.substr(start, truncLen));
        if (end < lastSlashPos) {
            processedDir.push_back('/');
        }
        start = end + 1;
    }

    // Cache and return
    auto [insertIt, _] = transformationCache.emplace(std::move(fullPath), processedDir);
    return { insertIt->second, std::string(filenameView) };
}


// Memory map for umount string division
std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>> cachedParsesForUmount;


// Function to divide any mountpoint into three strings and cache the results
std::tuple<std::string, std::string, std::string> parseMountPointComponents(std::string_view dir) {
    // Check cache with a string key converted from the string_view
    std::string dir_str(dir);
    auto cacheIt = cachedParsesForUmount.find(dir_str);
    if (cacheIt != cachedParsesForUmount.end()) {
        return cacheIt->second;
    }
    
    size_t underscorePos = dir.find('_');
    if (underscorePos == std::string_view::npos) {
        // No underscore found, return the whole string as directory part
        auto result = std::make_tuple(dir_str, std::string(), std::string());
        cachedParsesForUmount[dir_str] = result;
        return result;
    }
    
    // Include the underscore in the directory part
    std::string directoryPart(dir.substr(0, underscorePos + 1));
    
    size_t lastTildePos = dir.find_last_of('~');
    if (lastTildePos == std::string_view::npos || lastTildePos <= underscorePos) {
        // No tilde after underscore, format is "directory_filename"
        std::string filenamePart(dir.substr(underscorePos + 1));
        auto result = std::make_tuple(directoryPart, filenamePart, std::string());
        cachedParsesForUmount[dir_str] = result;
        return result;
    }
    
    // Format is "directory_filename~hash"
    std::string filenamePart(dir.substr(underscorePos + 1, lastTildePos - underscorePos - 1));
    std::string hashPart(dir.substr(lastTildePos));
    auto result = std::make_tuple(directoryPart, filenamePart, hashPart);
    cachedParsesForUmount[dir_str] = result;
    return result;
}


// Trim function to remove leading and trailing whitespaces and spaces between semicolons
std::string trimWhitespace(const std::string& str) {
    // Step 1: Trim leading and trailing spaces
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    
    if (first == std::string::npos || last == std::string::npos)
        return "";
    
    std::string trimmed = str.substr(first, (last - first + 1));
    
    // Step 2: Remove spaces around semicolons
    std::string result;
    for (size_t i = 0; i < trimmed.length(); ++i) {
        if (trimmed[i] == ' ') {
            // Skip spaces if they are before or after a semicolon
            bool isSpaceBeforeSemicolon = (i + 1 < trimmed.length() && trimmed[i + 1] == ';');
            bool isSpaceAfterSemicolon = (i > 0 && trimmed[i - 1] == ';');
            
            if (!isSpaceBeforeSemicolon && !isSpaceAfterSemicolon) {
                result += ' ';
            }
        } else if (trimmed[i] == ';') {
            // Add the semicolon and skip any spaces immediately before or after
            result += ';';
        } else {
            // Add non-space, non-semicolon characters
            result += trimmed[i];
        }
    }
    
    return result;
}
