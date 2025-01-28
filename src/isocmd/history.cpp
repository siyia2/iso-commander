// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_history_cache.txt";
const std::string historyPatternFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_filter_cache.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 50;

const int MAX_HISTORY_PATTERN_LINES = 25;

// Function to load history from readline
void loadHistory(bool& historyPattern) {
    // Only load history from file if it's not already populated in memory
    if (history_length == 0) {
        std::string targetFilePath = !historyPattern ? 
            historyFilePath : historyPatternFilePath;

        int fd = open(targetFilePath.c_str(), O_RDONLY);
        if (fd == -1) {
            return;  // File doesn't exist or couldn't be opened
        }

        // Acquire a shared lock for reading
        if (flock(fd, LOCK_SH) == -1) {
            close(fd);
            return;
        }

        std::ifstream file(targetFilePath);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                add_history(line.c_str());
            }
            file.close();
        }

        // Release the lock and close the file descriptor
        flock(fd, LOCK_UN);
        close(fd);
    }
}


// Function to save history from readline
void saveHistory(bool& historyPattern) {
    std::string targetFilePath = !historyPattern ? 
        historyFilePath : historyPatternFilePath;

    // Extract directory path from the target file path
    std::filesystem::path dirPath = std::filesystem::path(targetFilePath).parent_path();

    // Create the directory if it does not exist
    if (!std::filesystem::exists(dirPath)) {
        if (!std::filesystem::create_directories(dirPath)) {
            return;  // Directory creation failed, exit function
        }
    }

    int fd = open(targetFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        return;  // Failed to open file
    }

    // Acquire an exclusive lock for writing
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }

    std::ofstream historyFile(targetFilePath, std::ios::out | std::ios::trunc);
    if (!historyFile.is_open()) {
        flock(fd, LOCK_UN);  // Release the lock
        close(fd);
        return;
    }

    HIST_ENTRY **histList = history_list();
    if (!histList) {
        flock(fd, LOCK_UN);  // Release the lock
        close(fd);
        return;
    }

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

    // Release the lock and close the file descriptor
    flock(fd, LOCK_UN);
    close(fd);
}
