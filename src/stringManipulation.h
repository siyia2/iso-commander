// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef STRINGMANIPULATION_H
#define STRINGMANIPULATION_H

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <functional>
#include <charconv>

std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location);
std::tuple<std::string, std::string, std::string> parseMountPointComponents(std::string_view dir);
std::string trimWhitespace(const std::string& str);
void toLowerInPlace(std::string& str);

#endif // STRINGMANIPULATION_H
