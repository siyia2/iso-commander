// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"


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


// Function to load history for readline
void loadHistory(bool& filterHistory) {
    // 1. Wipe current session memory so we don't mix "Filter" history with "Path" history
    clear_history();
    std::string targetFilePath = !filterHistory ? 
        historyFilePath : filterHistoryFilePath;
    // 2. Check if file exists before trying to lock/open
    if (!std::filesystem::exists(targetFilePath)) {
        return;
    }
    // 3. Open file descriptor for flock
    int fd = open(targetFilePath.c_str(), O_RDONLY);
    if (fd == -1) return;
    // Acquire a shared lock (multiple readers allowed, prevents writing)
    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }
    // 4. Read file into Readline buffer via the locked fd
    FILE* f = fdopen(fd, "r");
    if (f) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            // Strip trailing newline
            if (!line.empty() && line.back() == '\n')
                line.pop_back();
            if (!line.empty()) {
                add_history(line.c_str());
            }
        }
        // 5. Cleanup — fclose also closes fd, so only unlock before fclose
        flock(fd, LOCK_UN);
        fclose(f);
    } else {
        // 5. Cleanup (fdopen failed)
        flock(fd, LOCK_UN);
        close(fd);
    }
}
// Function to save history from readline
void saveHistory(bool& filterHistory) {
    std::string targetFilePath = !filterHistory ? 
        historyFilePath : filterHistoryFilePath;
    // 1. Get the correct limit
    size_t maxLines = !filterHistory ? 
        MAX_HISTORY_LINES : MAX_HISTORY_PATTERN_LINES;
    // 2. stifle_history caps future additions but does not retroactively truncate
    // the existing in-memory list, so the manual loop below handles the limit instead.
    std::filesystem::path dirPath = std::filesystem::path(targetFilePath).parent_path();
    if (!dirPath.empty() && !std::filesystem::exists(dirPath)) {
        std::filesystem::create_directories(dirPath);
    }
    // 3. Open the file ONCE. Use the file descriptor to create the stream.
    // We don't use O_TRUNC yet so we don't kill the file if the write fails.
    int fd = open(targetFilePath.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd == -1) return;
    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }
    // Now truncate manually since we have the lock
    if (ftruncate(fd, 0) == -1) {
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }
    HIST_ENTRY **histList = history_list();
    if (histList) {
        std::vector<std::string> finalLines;
        std::unordered_set<std::string> seen;
        int count = 0;
        while (histList[count]) count++;
        // Process backwards for uniqueness and limit
        for (int i = count - 1; i >= 0; i--) {
            if (!histList[i] || !histList[i]->line) continue;
            std::string line(histList[i]->line);
            if (line.empty()) continue;
            if (seen.find(line) == seen.end()) {
                finalLines.push_back(line);
                seen.insert(line);
            }
            if (finalLines.size() >= maxLines) break;
        }
        std::reverse(finalLines.begin(), finalLines.end());
        // Write to the file descriptor via the locked fd
        FILE* f = fdopen(fd, "w");
        if (f) {
            for (const auto& line : finalLines) {
                fprintf(f, "%s\n", line.c_str());
            }
            fflush(f);
            // Cleanup — fclose also closes fd, so only unlock before fclose
            flock(fd, LOCK_UN);
            fclose(f);
        } else {
            // fdopen failed, clean up manually
            flock(fd, LOCK_UN);
            close(fd);
        }
    } else {
        // No history to write, clean up
        flock(fd, LOCK_UN);
        close(fd);
    }
}


// Function to clear path and filter history
void clearHistory(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();

    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

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
        // Use theme->secondary (Error) and theme->warning (Highlight)
        std::cerr << "\n" << (isOrig ? originalColors::red : theme->secondary) << "Invalid command: " 
                  << (isOrig ? originalColors::yellow : theme->warning) << "'" << inputSearch << "'" 
                  << (isOrig ? originalColors::red : theme->secondary) << ".\033[J" << std::endl;
        return;
    }

    std::ofstream ofs(filePath, std::ofstream::out | std::ofstream::trunc);
    if (!ofs) {
        std::cerr << "\n" << (isOrig ? originalColors::red : theme->secondary) 
                  << "Error clearing " << historyType << " database: " 
                  << (isOrig ? originalColors::yellow : theme->warning) << "'" << filePath << "'" 
                  << (isOrig ? originalColors::red : theme->secondary) << ". File missing or inaccessible.\033[J" << std::endl;
    } else {
        ofs.close();
        // Use theme->accent (Success)
        std::cout << "\n" << (isOrig ? originalColors::green : theme->accent) 
                  << historyType << " database cleared successfully.\033[J" << std::endl;
        clear_history();
    }
    std::cout << color << "\n\033[1;32m↵ to continue..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
