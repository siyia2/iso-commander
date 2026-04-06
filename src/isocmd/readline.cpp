// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../readline.h"
#include "../themes.h"

/**
 * @file readline_completion.cpp
 * @brief Custom GNU Readline completion and pagination logic for CLI commands and device mapping.
 */

static int current_page = 0;
static char last_common_prefix[1024] = "";

/**
 * @brief List of available special commands for completion.
 */
const char* special_cmds[] = {
    "!clr", "!clr_paths", "!clr_filter", "?config", "?stats",
    "*pagination:", "*fl_m", "*cl_m", "*fl_u", "*cl_u", "*fl_o", "*cl_o", 
    "*fl_w", "*cl_w", "*fl_c", "*cl_c", "*flno:on", "*flno:off", 
    "*auto:on", "*auto:off",
    "*skin:green", "*skin:cyan", "*skin:white", 
    "*skin:purple", "*skin:amber", "*skin:rose",
    "*theme:original", "*theme:classic", "*theme:high_contrast", 
    "*theme:neon", "*theme:ocean", "*theme:sunset", 
    "*theme:forest", "*theme:midnight", "*theme:mono", 
    "*theme:retro", "*theme:crimson", "*theme:dracula",
    NULL
};

/**
 * @brief Generates completion matches for special commands based on prefix.
 * @param text The partial text to match.
 * @param state Readline state (0 for first call).
 * @return Allocated string of the match or NULL.
 */
char* command_generator(const char* text, int state) {
    static int list_index, len;
    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    const char* name;
    while ((name = special_cmds[list_index++])) {
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }
    return NULL;
}

/**
 * @brief Dispatches completion requests to the command generator if triggers are detected.
 * @param text The partial text.
 * @param start Start index in line buffer.
 * @param end End index in line buffer.
 * @return Array of matches or nullptr.
 */
char** my_special_completion_entry(const char* text, int start, int end) {
    (void)start;
    (void)end;

    if (text[0] == '!' || text[0] == '?' || text[0] == '*') {
		// This stops Readline from adding a space after the match
        rl_completion_append_character = '\0';
        return rl_completion_matches(text, command_generator);
    }

    return nullptr; 
}

/**
 * @brief Custom hook for GNU Readline to display completions with pagination and color themes.
 * @param matches Array of completion matches.
 * @param num_matches Total number of matches found.
 * @param max_length Length of the longest match string.
 */
void customListingsFunction(char **matches, int num_matches, int max_length) {
    (void)max_length;

    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    // labelCol: Uses brown (replaced 130m)
    // hintCol: Uses yellow (replaced 93m)
    // dirCol: Uses blue (replaced 34m)
    const char* labelCol = isOrig ? originalColors::brown.data()  : theme->muted.data();
    const char* hintCol  = isOrig ? originalColors::yellow.data() : theme->accent.data();
    const char* dirCol   = isOrig ? originalColors::blue.data()   : theme->accent.data();
    
    // fileCol: Standard reset to default
    // resetCol: Uses your 24-bit Bold White
    const char* fileCol  = originalColors::boldAlt.data(); 
    const char* resetCol = originalColors::boldAlt.data();


    const char* current_prefix = matches[0]; 
    if (strcmp(last_common_prefix, current_prefix) != 0) {
        current_page = 0;
        strncpy(last_common_prefix, current_prefix, sizeof(last_common_prefix) - 1);
        last_common_prefix[sizeof(last_common_prefix) - 1] = '\0';
    }

    printf("\033[s"); 
    std::cout << "\033[J"; 
    printf("\n");

    int total_pages = 1;
    int start_index = 1; 
    int items_to_display;

    if (ITEMS_PER_PAGE <= 0) {
        items_to_display = num_matches;
    } else {
        total_pages = ((size_t)num_matches + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
        if (current_page >= total_pages) current_page = 0; 
        start_index = current_page * (int)ITEMS_PER_PAGE + 1;
        int remaining = num_matches - (start_index - 1);
        items_to_display = (remaining > (int)ITEMS_PER_PAGE) ? (int)ITEMS_PER_PAGE : remaining;
    }

    if (num_matches > 1) {
        if (total_pages > 1) {
            printf("\n%sTab Completion Matches [page %d/%d%s] (%sCtrl+l%s → clear%s):%s\n\n",
                   labelCol, current_page + 1, total_pages, labelCol, hintCol, resetCol, labelCol, resetCol);
        } else {
            printf("\n%sTab Completion Matches (%sCtrl+l%s → clear%s):%s\n\n",
                   labelCol, hintCol, resetCol, labelCol, resetCol);
        }
    }

    if (ITEMS_PER_PAGE > 0 && (size_t)num_matches > ITEMS_PER_PAGE) {
        current_page++;
        if (current_page >= total_pages) current_page = 0;
    }

    const char* base_path = matches[1];
    int base_len = 0;
    const char* last_slash = strrchr(base_path, '/');
    base_len = (last_slash != NULL) ? (last_slash - base_path + 1) : 0;

    size_t max_item_length = 0;
    for (int i = start_index; i < start_index + items_to_display; i++) {
        size_t item_length = strlen(matches[i] + base_len);
        if (item_length > max_item_length) max_item_length = item_length;
    }

    int num_columns = (items_to_display <= 2) ? items_to_display : 3;
    const int column_spacing = 4;
    int column_width = (num_columns < 3) ? ((max_item_length + 2 > 60) ? 60 : max_item_length + 2) 
                                         : ((max_item_length < 38) ? max_item_length + 2 : 40);
    const int total_column_width = column_width + column_spacing;
    int rows = (items_to_display + num_columns - 1) / num_columns;

    auto isDirectory = [](const char* path) -> bool {
        struct stat path_stat;
        return (stat(path, &path_stat) == 0) && S_ISDIR(path_stat.st_mode);
    };

    auto smartTruncate = [](const char* str, int max_width) -> std::string {
        std::string result(str);
        size_t len = result.length();
        if (len <= (size_t)max_width) return result;

        size_t dot_pos = result.find_last_of('.');
        if (dot_pos != std::string::npos && dot_pos > 0 && len - dot_pos <= 10) {
            std::string ext = result.substr(dot_pos);
            int prefix_len = std::max(3, max_width - (int)ext.length() - 3);
            if (prefix_len >= 3) return result.substr(0, prefix_len) + "..." + ext;
        }
        int prefix_len = (max_width - 3) / 2;
        int suffix_len = max_width - 3 - prefix_len;
        return result.substr(0, prefix_len) + "..." + result.substr(len - suffix_len, suffix_len);
    };

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < num_columns; col++) {
            int page_offset = row + col * rows;
            int index = start_index + page_offset;

            if (page_offset < items_to_display) {
                const char* full_path = matches[index];
                const char* relative_path = full_path + base_len;
                bool is_dir = isDirectory(full_path);
                std::string formatted;

                if (is_dir) {
                    formatted = std::string(dirCol) + smartTruncate(relative_path, column_width - 1) + "/" + resetCol;
                } else {
                    formatted = std::string(fileCol) + smartTruncate(relative_path, column_width) + resetCol;
                }

                printf("%s", formatted.c_str());

                if (col < num_columns - 1 && page_offset < items_to_display - 1) {
                    int visible_length = (int)strlen(relative_path) + (is_dir ? 1 : 0);
                    int displayed_length = std::min(visible_length, column_width);
                    int padding = total_column_width - displayed_length;
                    for (int i = 0; i < padding; i++) printf(" ");
                }
            }
        }
        printf("\033[0m\n"); 
    }

    if (ITEMS_PER_PAGE > 0 && (size_t)num_matches > ITEMS_PER_PAGE) {
        printf("\n%s[%d/%d matches — press %sTab%s for next page]%s\n",
               labelCol, start_index + items_to_display - 1, num_matches, 
               hintCol, labelCol, resetCol);
    }

    printf("\033[u"); 
}

CompleterData g_completerData = {nullptr, nullptr};

/**
 * @brief Callback for Readline to handle ISO-to-USB mapping completions.
 * @param text The partial text.
 * @param start Start index in line buffer.
 * @param end End index in line buffer.
 * @return Array of matches or nullptr.
 */
char** completion_cb(const char* text, int start, int end) {
    current_page = 0;
    last_common_prefix[0] = '\0';
    
    rl_attempted_completion_over = 1; 
    char** matches = nullptr;
    std::string current_word(rl_line_buffer + start, end - start);
    
    bool is_device_completion = (current_word.find('>') != std::string::npos);
    
    if (!is_device_completion) {
        if (g_completerData.sortedIsos) {
            std::vector<std::string> possible_completions;
            size_t list_index = 0;
            while (list_index < g_completerData.sortedIsos->size()) {
                const std::string opt = std::to_string(++list_index) + ">";
                if (opt.find(text) == 0)
                    possible_completions.push_back(opt);
            }
            
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_completions) {
                if (full_line.find(comp) == std::string::npos) {
                    all_present = false;
                    break;
                }
            }
            
            if (!all_present && !possible_completions.empty()) {
                matches = rl_completion_matches(text, [](const char* text, int state) -> char* {
                    static size_t list_index;
                    if (!state) {
                        list_index = 0;
                        rl_completion_append_character = '\0';
                    }
                    while (list_index < g_completerData.sortedIsos->size()) {
                        const std::string opt = std::to_string(++list_index) + ">";
                        if (opt.find(text) == 0)
                            return strdup(opt.c_str());
                    }
                    return (char*)nullptr; 
                });
            }
        }
    }
    else {
        if (g_completerData.usbDevices) {
            std::string fullText(text);
            size_t pos = fullText.find_last_of('>');
            std::string prefix, deviceSubText;
            if (pos != std::string::npos) {
                prefix = fullText.substr(0, pos + 1);
                deviceSubText = fullText.substr(pos + 1);
            } else {
                deviceSubText = fullText;
            }
            
            std::vector<std::string> possible_device_completions;
            for (size_t i = 0; i < g_completerData.usbDevices->size(); i++) {
                const std::string& dev = (*g_completerData.usbDevices)[i];
                if (dev.find(deviceSubText) == 0) {
                    std::string completion = prefix + dev;
                    possible_device_completions.push_back(completion);
                }
            }
            
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_device_completions) {
                if (full_line.find(comp) == std::string::npos) {
                    all_present = false;
                    break;
                }
            }
            
            if (!all_present && !possible_device_completions.empty()) {
                static std::string s_prefix;
                static std::string s_deviceSubText;
                s_prefix = prefix;
                s_deviceSubText = deviceSubText;
                
                rl_completion_append_character = '\0';
                matches = rl_completion_matches(fullText.c_str(), [](const char* /*unused*/, int state) -> char* {
                    static size_t list_index;
                    if (!state) {
                        list_index = 0;
                    }
                    while (list_index < g_completerData.usbDevices->size()) {
                        const std::string& dev = (*g_completerData.usbDevices)[list_index++];
                        if (dev.find(s_deviceSubText) == 0) {
                            std::string completion = s_prefix + dev;
                            return strdup(completion.c_str());
                        }
                    }
                    return (char*)nullptr;
                });
            }
        }
    }
    return matches;
}

/**
 * @brief Resets the pagination state for completions.
 */
void resetReadlinePagination() {
    current_page = 0;
    last_common_prefix[0] = '\0';
}

/**
 * @brief Re-enables custom completion hooks and history navigation keybindings.
 */
void restoreReadline() {
    rl_completion_display_matches_hook = customListingsFunction;
    rl_attempted_completion_function = nullptr;
    rl_bind_keyseq("\033[A", rl_get_previous_history);
    rl_bind_keyseq("\033[B", rl_get_next_history);
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
}

/**
 * @brief Disables standard completion and history navigation for confirmation prompts.
 */
void disableReadlineForConfirmation() {
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    rl_bind_keyseq("\033[A", prevent_readline_keybindings);
    rl_bind_keyseq("\033[B", prevent_readline_keybindings);
}

/**
 * @brief Dummy function to intercept and disable specific keypresses in Readline.
 * @return Always 0.
 */
int prevent_readline_keybindings(int, int) {
    return 0;
}

/**
 * @brief Clears the terminal screen, scrollback buffer, and resets pagination.
 * @return Always 0.
 */
int clear_screen_and_buffer(int, int) {
    clearScrollBuffer();
    fflush(stdout);
    rl_forced_update_display();
    
    current_page = 0;
    last_common_prefix[0] = '\0';
    
    return 0;
}
