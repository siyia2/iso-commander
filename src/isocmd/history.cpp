// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// C / System Headers
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../history.h"
#include "../inputHandling.h"
#include "../pausePrompt.h"
#include "../state.h"
#include "../themes.h"

/**
 * @brief Validates if a folder path history file is effectively empty.
 * * Performs checks for file existence, size, and content validity. A file is 
 * considered empty if it contains only whitespace or lacks any entries 
 * starting with a forward slash.
 * * @param filePath The filesystem path to the history file.
 * @return true If the file is missing, size zero, or contains no valid paths.
 * @return false If the file contains at least one valid path entry.
 */
bool isHistoryFileEmpty(const std::string& filePath) {
    struct stat fileInfo;
    if (stat(filePath.c_str(), &fileInfo) != 0) {
        return true;
    }

    if (fileInfo.st_size == 0) {
        return true;
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
        return true;
    }

    bool hasNonWhitespace = false;
    bool hasEntryStartingWithSlash = false;
    std::string line;

    while (std::getline(file, line)) {
        for (char ch : line) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                hasNonWhitespace = true;
                break;
            }
        }

        if (!line.empty() && line[0] == '/') {
            hasEntryStartingWithSlash = true;
        }
    }

    if (!hasNonWhitespace || !hasEntryStartingWithSlash) {
        return true;
    }

    return false;
}

/**
 * @brief Loads history from a file into the GNU Readline buffer.
 * * Swaps the current history context based on whether the user is filtering 
 * or navigating. Uses advisory file locking (flock) to ensure thread-safe 
 * and process-safe reads.
 * * @param filterHistory Boolean toggle; true for filter history, false for path history.
 */
void loadHistory(bool& filterHistory) {
    clear_history();
    std::string targetFilePath = !filterHistory ? GlobalState::historyFilePath : GlobalState::filterHistoryFilePath;

    if (!std::filesystem::exists(targetFilePath)) {
        return;
    }

    int fd = open(targetFilePath.c_str(), O_RDONLY);
    if (fd == -1) return;

    if (flock(fd, LOCK_SH) == -1) {
        close(fd);
        return;
    }

    FILE* f = fdopen(fd, "r");
    if (f) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            if (!line.empty() && line.back() == '\n')
                line.pop_back();
            if (!line.empty()) {
                add_history(line.c_str());
            }
        }
        flock(fd, LOCK_UN);
        fclose(f);
    } else {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/**
 * @brief Saves the current Readline history to a persistent file.
 * * Performs deduplication (keeping only the most recent unique entries) 
 * and truncates the file to the maximum allowed lines. Employs exclusive 
 * file locking (flock) during the write process.
 * * @param filterHistory Boolean toggle; determines which database file to write to.
 */
void saveHistory(bool& filterHistory) {
    std::string targetFilePath = !filterHistory ? GlobalState::historyFilePath : GlobalState::filterHistoryFilePath;
    size_t maxLines = !filterHistory ? GlobalState::MAX_HISTORY_LINES : GlobalState::MAX_HISTORY_PATTERN_LINES;

    std::filesystem::path dirPath = std::filesystem::path(targetFilePath).parent_path();
    if (!dirPath.empty() && !std::filesystem::exists(dirPath)) {
        std::filesystem::create_directories(dirPath);
    }

    int fd = open(targetFilePath.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd == -1) return;

    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }

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

        FILE* f = fdopen(fd, "w");
        if (f) {
            for (const auto& line : finalLines) {
                fprintf(f, "%s\n", line.c_str());
            }
            fflush(f);
            flock(fd, LOCK_UN);
            fclose(f);
        } else {
            flock(fd, LOCK_UN);
            close(fd);
        }
    } else {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/**
 * @brief Clears the specified history database file and the current session history.
 * * Handles user commands to wipe either path or filter databases. 
 * Re-routes output based on the active theme for error/success messaging.
 * * @param inputSearch The command string (e.g., "!clr_paths" or "!clr_filter").
 */
void clearHistory(const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    const MainTheme* theme = getActiveTheme();
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
        std::cerr << "\n" << (isOrig ? UI::Palette::Red : theme->secondary) << "Invalid command: " 
                  << (isOrig ? UI::Palette::Yellow : theme->warning) << "'" << inputSearch << "'" 
                  << (isOrig ? UI::Palette::Red : theme->secondary) << ".\033[J" << std::endl;
        return;
    }

    std::ofstream ofs(filePath, std::ofstream::out | std::ofstream::trunc);
    if (!ofs) {
        std::cerr << "\n" << (isOrig ? UI::Palette::Red : theme->secondary) 
                  << "Error clearing " << historyType << " database: " 
                  << (isOrig ? UI::Palette::Yellow : theme->warning) << "'" << filePath << "'" 
                  << (isOrig ? UI::Palette::Red : theme->secondary) << ". File missing or inaccessible.\033[J" << std::endl;
    } else {
        ofs.close();
        std::cout << "\n" << (isOrig ? UI::Palette::Green : theme->accent) 
                  << historyType << " database cleared successfully.\033[J" << std::endl;
        clear_history();
    }
    pressEnterToContinue();
}
