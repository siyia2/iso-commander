// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>

// Project Headers
#include "../rendering.h"
#include "../history.h"
#include "../inputHandling.h"
#include "../pausePrompts.h"
#include "../state.h"
#include "../themes.h"

/**
 * @brief Validates whether a folder-path history file is effectively empty.
 *
 * A file is considered empty if it is missing (stat fails), has zero size,
 * contains only whitespace, or contains no line whose first character is a
 * forward slash. A line is considered a valid path entry only if its very
 * first byte is '/'; lines with leading whitespace before '/' do not count.
 *
 * @param filePath The filesystem path to the history file.
 * @return true  If the file is missing, empty, or contains no valid path entry.
 * @return false If the file contains at least one non-whitespace line and at
 *               least one line starting with '/'.
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
 *
 * Always calls clear_history() on entry, so the in-memory list is reset
 * regardless of the max-lines setting. Selects the path-history or
 * filter-history file based on @p filterHistory and reads it under a shared
 * advisory lock (flock). If the configured max lines for this history type
 * is 0, history is disabled: the (already cleared) in-memory list is left
 * empty and the file on disk is not touched. If the file does not exist,
 * returns after the in-memory clear.
 *
 * Lines longer than 4095 bytes are effectively split across multiple
 * add_history() calls because fgets() reads at most sizeof(buf)-1 bytes.
 *
 * @param filterHistory True for filter history, false for path history.
 */
void loadHistory(bool& filterHistory) {
    clear_history();
    std::string targetFilePath = !filterHistory ? GlobalState::historyFilePath : GlobalState::filterHistoryFilePath;
    size_t maxLines = !filterHistory ? GlobalState::MAX_HISTORY_LINES : GlobalState::MAX_HISTORY_PATTERN_LINES;
    if (maxLines == 0) return;

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
 *
 * Selects the path-history or filter-history file based on @p filterHistory.
 * If the configured max lines for this history type is 0, history is
 * disabled: the in-memory list is cleared and the file on disk is left
 * untouched. Otherwise creates the parent directory if absent, opens the
 * file with O_WRONLY | O_CREAT (mode 0644), takes an exclusive advisory lock
 * (flock), and truncates it via ftruncate(). Iterates history_list() from
 * newest to oldest, deduplicating entries and stopping once maxLines unique
 * entries have been collected; the collected list is then reversed to
 * oldest-first file order and written. If history_list() is empty or null,
 * the file is still truncated to zero, so an empty in-memory history
 * legitimately wipes the file.
 *
 * Any failure in open(), flock(), or ftruncate() returns without modifying
 * the file or the in-memory history.
 *
 * @param filterHistory True for filter history, false for path history.
 */
void saveHistory(bool& filterHistory) {
    std::string targetFilePath = !filterHistory ? GlobalState::historyFilePath : GlobalState::filterHistoryFilePath;
    size_t maxLines = !filterHistory ? GlobalState::MAX_HISTORY_LINES : GlobalState::MAX_HISTORY_PATTERN_LINES;

    if (maxLines == 0) {
        clear_history();
        return;
    }

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
 *
 * Ignores SIGINT and disables Ctrl-D handling, then resolves the theme and
 * dispatches on @p inputSearch:
 *   "!clr_paths"  — targets the folder-path history file
 *   "!clr_filter" — targets the filter-term history file
 *   anything else — prints an invalid-command error to std::cerr and returns
 *                   without prompting or modifying anything
 *
 * The target file is derived from $HOME/.local/share/isocmd/database/ rather
 * than GlobalState::historyFilePath / filterHistoryFilePath; this is a
 * hardcoded path that assumes those globals point at the same location.
 * The file is truncated via std::ofstream with trunc; on success
 * clear_history() is called immediately (not from inside the render lambda),
 * and on failure nothing in memory is cleared.
 *
 * The success or failure message is rendered via a `render` lambda so it can
 * be replayed by the resize watcher and the press-Enter prompt without drift.
 * The side effects (file truncation and, on success, clear_history) happen
 * exactly once before the render lambda is first invoked; subsequent replays
 * only redraw. repaintHeader is skipped on the first draw because the prompt
 * is still visible, and invoked on later (resize) redraws.
 *
 * @param inputSearch   The command string ("!clr_paths" or "!clr_filter");
 *                      any other value produces an error message and returns.
 * @param repaintHeader Callback invoked to repaint the screen header on
 *                      redraws after the first; may be empty.
 */
void clearHistory(const std::string& inputSearch,
                  const std::function<void()>& repaintHeader) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    auto hc = resolveDatabaseTheme();

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
        std::cerr << "\n" << (hc.error) << "Invalid command: "
                  << (hc.warning) << "'" << inputSearch << "'"
                  << (hc.error) << ".\033[J" << std::endl;
        return;
    }

    // First draw: the prompt is still on screen. Later draws
    // (resize repaints) come after a clear, so redraw the header.
    bool firstDraw = true;

    std::ofstream ofs(filePath, std::ofstream::out | std::ofstream::trunc);
    if (!ofs) {
        auto render = [&] {
            SyncOutput sync;
            if (!firstDraw && repaintHeader) repaintHeader();
            firstDraw = false;

            std::cerr << "\n" << (hc.error)
                      << "Error clearing " << historyType << " database: "
                      << (hc.warning) << "File missing or inaccessible" << (hc.error) << "." << "\033[J" << std::endl;
        };
        pressEnterToContinueWithRepaint(render);
    } else {
        ofs.close();
        clear_history();

        auto render = [&] {
            SyncOutput sync;
            if (!firstDraw && repaintHeader) repaintHeader();
            firstDraw = false;

            std::cout << "\n" << (hc.highlight)
                      << historyType << " database cleared successfully.\033[J" << std::endl;
        };
        pressEnterToContinueWithRepaint(render);
    }
}
