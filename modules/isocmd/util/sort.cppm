// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.util.sort;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * Compares two strings using case-insensitive natural ordering.
 */
int naturalCompare(std::string_view a, std::string_view b);

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

} // extern "C++"
