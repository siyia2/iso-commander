#include "../headers.h"


// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_history_cache.txt";
const std::string historyPatternFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_pattern_cache.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 25;

const int MAX_HISTORY_PATTERN_LINES = 25;

// Function to load history from readline
void loadHistory(bool& historyPattern) {
    // Only load history from file if it's not already populated in memory
    if (history_length == 0) {
        std::ifstream file;
        if (!historyPattern) {
            file.open(historyFilePath);
        } else {
            file.open(historyPatternFilePath);
        }

        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                add_history(line.c_str());
            }
            file.close();
        }
    }
}


// Function to save history from readline
void saveHistory(bool& historyPattern) {
    std::ofstream historyFile;
    
    // Choose file path based on historyPattern flag
    std::string targetFilePath = !historyPattern ? 
        historyFilePath : historyPatternFilePath;
    
    // Open file in truncate mode
    historyFile.open(targetFilePath, std::ios::out | std::ios::trunc);
    
    if (!historyFile.is_open()) {
        return;
    }

    HIST_ENTRY **histList = history_list();
    if (!histList) return;

    std::unordered_map<std::string, size_t> lineIndices;
    std::vector<std::string> uniqueLines;

    // Iterate through history entries
    for (int i = 0; histList[i]; i++) {
        std::string line(histList[i]->line);
        if (line.empty()) continue;

        // Remove existing duplicate if present, then add to end
        auto it = lineIndices.find(line);
        if (it != lineIndices.end()) {
            uniqueLines.erase(uniqueLines.begin() + it->second);
            lineIndices.erase(it);
        }

        // Add new line to end and update index
        lineIndices[line] = uniqueLines.size();
        uniqueLines.push_back(line);
    }

    // Determine max lines based on pattern flag
    size_t maxLines = !historyPattern ? 
        MAX_HISTORY_LINES : MAX_HISTORY_PATTERN_LINES;

    // Trim excess lines if needed
    if (uniqueLines.size() > maxLines) {
        uniqueLines.erase(
            uniqueLines.begin(), 
            uniqueLines.begin() + (uniqueLines.size() - maxLines)
        );
    }

    // Write unique lines to file
    for (const auto& line : uniqueLines) {
        historyFile << line << std::endl;
    }

    historyFile.close();
}
