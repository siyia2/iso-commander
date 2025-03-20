// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"


// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_path_database.txt";
const std::string filterHistoryFilePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/iso_commander_filter_database.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 50;

const int MAX_HISTORY_PATTERN_LINES = 25;


// Function to check if folder path history is empty
bool isHistoryFileEmpty(const std::string& filePath) {
    // Check if the file exists
    struct stat fileInfo;
    if (stat(filePath.c_str(), &fileInfo) != 0) {
        // File doesn't exist, consider it as empty
        return true;
    }

    // Check if the file size is 0
    if (fileInfo.st_size == 0) {
        return true;
    }

    // Open the file and check its content
    std::ifstream file(filePath);
    if (!file.is_open()) {
        // Unable to open the file, consider it as empty
        return true;
    }

    bool hasNonWhitespace = false;
    bool hasEntryStartingWithSlash = false;
    std::string line;

    while (std::getline(file, line)) {
        // Check if the line contains non-whitespace characters
        for (char ch : line) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                hasNonWhitespace = true;
                break;
            }
        }

        // Check if the line starts with '/'
        if (!line.empty() && line[0] == '/') {
            hasEntryStartingWithSlash = true;
        }
    }

    // If the file has no non-whitespace characters, it's empty
    if (!hasNonWhitespace) {
        return true;
    }

    // If the file has no entries starting with '/', it's considered empty
    if (!hasEntryStartingWithSlash) {
        return true;
    }

    // Otherwise, the file is not empty
    return false;
}


// Function to load history from readline
void loadHistory(bool& filterHistory) {
    // Only load history from file if it's not already populated in memory
    if (history_length == 0) {
        std::string targetFilePath = !filterHistory ? 
            historyFilePath : filterHistoryFilePath;

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
void saveHistory(bool& filterHistory) {
    std::string targetFilePath = !filterHistory ? 
        historyFilePath : filterHistoryFilePath;

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
    size_t maxLines = !filterHistory ? 
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


// Function to clear path and filter history
void clearHistory(const std::string& inputSearch) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    const std::string basePath = std::string(getenv("HOME")) + "/.local/share/isocmd/database/";
    std::string filePath;
    std::string historyType;

    if (inputSearch == "!clr_paths") {
        filePath = basePath + "iso_commander_path_database.txt";
        historyType = "FolderPath";
    } else if (inputSearch == "!clr_filter") {
        filePath = basePath + "iso_commander_filter_database.txt";
        historyType = "FilterTerm";
    } else {
        std::cerr << "\n\001\033[1;91mInvalid command: \001\033[1;93m'" 
                  << inputSearch << "'\001\033[1;91m." << std::endl;
        return;
    }

    if (std::remove(filePath.c_str()) != 0) {
        std::cerr << "\n\001\033[1;91mError clearing " << historyType << " database: \001\033[1;93m'" 
                  << filePath << "'\001\033[1;91m. File missing or inaccessible." << std::endl;
    } else {
        std::cout << "\n\001\033[1;92m" << historyType << " database successfully." << std::endl;
        clear_history();
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
