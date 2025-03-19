// SPDX-License-Identifier: GPL-2.0-or-later

#include "../headers.h"


// Function to check if a string starts with '0' for tokenize input
bool startsWithZero(const std::string& str) {
    return !str.empty() && str[0] == '0';
}


// Function to check if a string is numeric for tokenize
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// General function to tokenize input strings
void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<int>& processedIndices) {
    std::istringstream iss(input);
    std::string token;

    std::unordered_set<std::string> invalidInputs;
    std::unordered_set<std::string> invalidIndices;
    std::unordered_set<std::string> invalidRanges;

    while (iss >> token) {
        if (startsWithZero(token)) {
            invalidIndices.insert(token);
            continue;
        }

        if (std::count(token.begin(), token.end(), '-') > 1) {
            invalidInputs.insert(token);
            continue;
        }

        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;
            try {
                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument&) {
                invalidInputs.insert(token);
                continue;
            } catch (const std::out_of_range&) {
                invalidRanges.insert(token);
                continue;
            }

            if (start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size() || start == 0 || end == 0) {
                invalidRanges.insert(token);
                continue;
            }

            int step = (start <= end) ? 1 : -1;
            for (int i = start; (start <= end) ? (i <= end) : (i >= end); i += step) {
                if (i >= 1 && i <= static_cast<int>(isoFiles.size())) {
                    if (processedIndices.find(i) == processedIndices.end()) {
                        processedIndices.insert(i);
                    }
                }
            }
        } else if (isNumeric(token)) {
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size()) {
                if (processedIndices.find(num) == processedIndices.end()) {
                    processedIndices.insert(num);
                }
            } else {
                invalidIndices.insert(token);
            }
        } else {
            invalidInputs.insert(token);
        }
    }

    // Helper to format error messages with pluralization
    auto formatCategory = [](const std::string& singular, const std::string& plural,
                            const std::unordered_set<std::string>& items) {
        if (items.empty()) return std::string();
        std::ostringstream oss;
        oss << "\033[1;91m" << (items.size() > 1 ? plural : singular) << ": '";
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (it != items.begin()) oss << " ";
            oss << *it;
        }
        oss << "'.\033[0;1m";
        return oss.str();
    };

    // Add formatted messages with conditional pluralization
    if (!invalidInputs.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid input", "Invalid inputs", invalidInputs));
    }
    if (!invalidIndices.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid index", "Invalid indexes", invalidIndices));
    }
    if (!invalidRanges.empty()) {
        uniqueErrorMessages.insert(formatCategory("Invalid range", "Invalid ranges", invalidRanges));
    }
}
