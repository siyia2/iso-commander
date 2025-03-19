// SPDX-License-Identifier: LGPL-3.0-or-later

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

// For memory mapping original paths
std::unordered_map<std::string, std::string> originalPathsCache;


// Function to extract directory and filename from a given path
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location) {
    // Find last slash efficiently
    auto lastSlashPos = path.find_last_of("/\\");
    if (lastSlashPos == std::string_view::npos) {
        return {"", std::string(path)};
    }
    
    // Extract filename part once for reuse
    std::string filename = std::string(path.substr(lastSlashPos + 1));
    std::string fullPath = std::string(path);
    
    // Get original directory path
    std::string originalDir;
    
    // Check if path is already in originalPathsCache
    auto originalPathIt = originalPathsCache.find(fullPath);
    if (originalPathIt != originalPathsCache.end()) {
        originalDir = originalPathIt->second;
    } else {
        // Store original directory path
        originalDir = std::string(path.substr(0, lastSlashPos));
        originalPathsCache[fullPath] = originalDir;
    }
    
    // Early return for full list mode - use original directory
    if (displayConfig::toggleFullListMount && location == "mount") {
        return {originalDir, filename};
    } else if (displayConfig::toggleFullListCpMvRm && location == "cp_mv_rm") {
        return {originalDir, filename};
    } else if (displayConfig::toggleFullListConversions && location == "conversions") {
        return {originalDir, filename};
    } else if (displayConfig::toggleFullListWrite && location == "write") {
        return {originalDir, filename};
    }
    
    // Check transformation cache
    auto cacheIt = transformationCache.find(fullPath);
    if (cacheIt != transformationCache.end()) {
        return {cacheIt->second, filename};
    }
    
    // Optimize directory shortening
    std::string processedDir;
    processedDir.reserve(path.length() / 2);  // More conservative pre-allocation
    size_t start = 0;
    while (start < lastSlashPos) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = lastSlashPos;
        // More efficient component truncation
        size_t componentLength = end - start;
        size_t truncatePos = std::min({
            componentLength, 
            path.find(' ', start) - start,
            path.find('-', start) - start,
            path.find('_', start) - start,
            path.find('.', start) - start,
            size_t(16)
        });
        processedDir.append(path.substr(start, truncatePos));
        // Don't add a slash after the last component
        if (end < lastSlashPos) {
            processedDir.push_back('/');
        }
        start = end + 1;
    }
    
    // Cache the transformed result
    transformationCache[fullPath] = processedDir;
    
    return {processedDir, filename};
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
