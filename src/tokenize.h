// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TOKENIZE_H
#define TOKENIZE_H

#include <vector>
#include <string>
#include <sstream> 
#include <unordered_set>
#include <iostream>

void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, 
std::unordered_set<int>& processedIndices);

#endif // TOKENIZE_H
