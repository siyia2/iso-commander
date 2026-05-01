// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef STRINGMANIPULATION_H
#define STRINGMANIPULATION_H

// C++ Standard Library Headers
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <utility> // For std::pair

// --- Path & Component Parsing ---

/**
 * Splits a full path into its directory and filename components.
 * @param path The full input path.
 * @param location Contextual hint or fallback location.
 * @return A pair where .first is the Directory and .second is the Filename.
 */
std::pair<std::string, std::string> extractDirectoryAndFilename(
    std::string_view path, 
    const std::string& location
);

/**
 * Parses a mount point string into its constituent parts.
 * @return A tuple containing (Device, MountPoint, FilesystemType).
 */
std::tuple<std::string, std::string, std::string> parseMountPointComponents(
    std::string_view dir
);


// --- Content Cleaning & Transformation ---

/**
 * Removes leading and trailing whitespace from a string.
 */
std::string trimWhitespace(const std::string& str);

/**
 * Converts all characters in a string to lowercase directly.
 */
void toLowerInPlace(std::string& str);

#endif // STRINGMANIPULATION_H
