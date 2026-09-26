// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SORT_H
#define SORT_H

// C++ Standard Library Headers
#include <string>
#include <vector>

/**
 * Performs an in-place, case-insensitive lexicographical sort on a vector of strings.
 * Useful for ensuring file lists appear in alphabetical order regardless of casing.
 */
void sortFilesCaseInsensitive(std::vector<std::string>& files);

/**
 * Adjusts the global or local sorting state specifically for "filename-only" display modes.
 * This typically ensures that path prefixes do not interfere with the alphabetical 
 * ordering of the actual files.
 */
void sortAfterFilenamesOnlyFlag();

#endif // SORT_H
