// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstdio>

// C / System Headers
#include <sys/stat.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../inputHandling.h"
#include "../write2usb.h"
#include "../readline.h"
#include "../state.h"
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
    "!clr", "!clr_paths", "!clr_filter", "*stats",
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

    if (text[0] == '!' || text[0] == '*') {
        return rl_completion_matches(text, command_generator);
    }

    return nullptr; 
}

/**
 * @brief Custom hook for GNU Readline to display completions with pagination and color themes.
 *
 * @param matches  Array of completion matches in Readline format: matches[0] is the
 *                 common prefix of all completions (used for pagination state tracking,
 *                 not displayed), and matches[1..num_matches] are the actual completion
 *                 candidates.
 * @param num_matches  Total number of completion candidates (excluding matches[0]).
 * @param max_length   Length of the longest match string as provided by Readline.
 *                     Unused by this implementation — column widths are computed
 *                     directly from the match array.
 */
void customListingsFunction(char **matches, int num_matches, int max_length) {
    (void)max_length;

    // Resolve the raw pointer struct
    ReadlineColors rc = resolveReadlineTheme();

    // --- Detect if we are completing special commands ---
    bool is_special_cmd_completion = false;
    if (num_matches > 0) {
        const char* first_match = matches[1];
        if (first_match && (first_match[0] == '!' || first_match[0] == '?' || first_match[0] == '*')) {
            is_special_cmd_completion = true;
        }
    }

    // --- Pagination state management ---
    const char* current_prefix = matches[0];
    if (strcmp(last_common_prefix, current_prefix) != 0) {
        current_page = 0;
        strncpy(last_common_prefix, current_prefix, sizeof(last_common_prefix) - 1);
        last_common_prefix[sizeof(last_common_prefix) - 1] = '\0';
    }

    printf("\033[s\033[J\n"); // Save cursor and clear

    int total_pages = 1;
    int start_index = 1;
    int items_to_display;

    if (GlobalState::ITEMS_PER_PAGE <= 0) {
        items_to_display = num_matches;
    } else {
        total_pages = ((size_t)num_matches + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;
        if (current_page >= total_pages) current_page = 0;
        start_index = current_page * (int)GlobalState::ITEMS_PER_PAGE + 1;
        int remaining = num_matches - (start_index - 1);
        items_to_display = (remaining > (int)GlobalState::ITEMS_PER_PAGE) ? (int)GlobalState::ITEMS_PER_PAGE : remaining;
    }

    // --- Header printing using raw pointers ---
    if (num_matches > 1) {
        const char* header_label = is_special_cmd_completion ? "CMD Completion Matches" : "Tab Completion Matches";
        
        if (total_pages > 1) {
            printf("\n%s%s [page %d/%d%s] (%sCtrl+l%s → clear%s):%s\n\n",
                   rc.label, header_label, current_page + 1, total_pages, rc.label, 
                   rc.hint, rc.reset, rc.label, rc.reset);
        } else {
            printf("\n%s%s (%sCtrl+l%s → clear%s):%s\n\n",
                   rc.label, header_label, rc.hint, rc.reset, rc.label, rc.reset);
        }
    }

    // Advance page for next call (identical)
    if (GlobalState::ITEMS_PER_PAGE > 0 && (size_t)num_matches > GlobalState::ITEMS_PER_PAGE) {
        current_page++;
        if (current_page >= total_pages) current_page = 0;
    }

    // --- Common layout helpers ---
    auto smartTruncate = [](const char* str, int max_width) -> std::string {
        std::string result(str);
        size_t len = result.length();
        if (len <= (size_t)max_width) return result;

        // Try to preserve extension for files (only relevant in file branch, but harmless elsewhere)
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

    // Precompute display strings and lengths for each visible item
    struct DisplayItem {
        std::string display_text;   // truncated, colored string ready for printing
        int visual_length;          // length without ANSI codes
        const char* raw_match;      // original match for potential directory check
    };
    std::vector<DisplayItem> display_items;
    display_items.reserve(items_to_display);

    // Base path handling (only used in file branch)
    const char* base_path = matches[1];
    int base_len = 0;
    if (!is_special_cmd_completion) {
        const char* last_slash = strrchr(base_path, '/');
        base_len = (last_slash != NULL) ? (last_slash - base_path + 1) : 0;
    }

    // Colors for special command prefixes
    const char* exclColor = UI::Palette::Yellow.data();
    const char* qmarkColor = UI::Palette::Blue.data();
    const char* starColor = UI::Palette::Purple.data();
    const char* resetPlain = UI::Palette::Reset.data();

    // Lambda to check if a path is a directory (file branch only)
    auto isDirectory = [](const char* path) -> bool {
        struct stat path_stat;
        return (stat(path, &path_stat) == 0) && S_ISDIR(path_stat.st_mode);
    };

    // Determine maximum visual length for column layout
    size_t max_item_length = 0;
    for (int i = 0; i < items_to_display; ++i) {
        int idx = start_index + i;
        const char* raw = matches[idx];
        std::string display;
        int vis_len = 0;

        if (is_special_cmd_completion) {
            // Special command: whole string
            const char* color = resetPlain;
            if (raw[0] == '!') color = exclColor;
            else if (raw[0] == '?') color = qmarkColor;
            else if (raw[0] == '*') color = starColor;
            std::string truncated = smartTruncate(raw, 1000); // temporarily use large width
            vis_len = (int)truncated.length();
            display = std::string(color) + truncated + resetPlain;
        } else {
            // File/directory: relative path
            const char* relative = raw + base_len;
            bool is_dir = isDirectory(raw);
            if (is_dir) {
                std::string truncated = smartTruncate(relative, 1000);
                vis_len = (int)truncated.length() + 1; // +1 for trailing '/'
                display = std::string(rc.dir) + truncated + "/" + rc.reset;
            } else {
                std::string truncated = smartTruncate(relative, 1000);
                vis_len = (int)truncated.length();
                display = std::string(rc.file) + truncated + rc.reset;
            }
        }
        display_items.push_back({display, vis_len, raw});
        if ((size_t)vis_len > max_item_length) max_item_length = vis_len;
    }

    // Column layout computation
    int num_columns = (items_to_display <= 4) ? 1 : 3;
    const int column_spacing = 4;
    int column_width = (num_columns < 3) ? ((max_item_length + 2 > 60) ? 60 : max_item_length + 2)
                                         : ((max_item_length < 38) ? max_item_length + 2 : 40);
    const int total_column_width = column_width + column_spacing;
    int rows = (items_to_display + num_columns - 1) / num_columns;

    // Now re-truncate with actual column width and update display strings
    for (auto& item : display_items) {
        const char* raw = item.raw_match;
        if (is_special_cmd_completion) {
            const char* color = resetPlain;
            if (raw[0] == '!') color = exclColor;
            else if (raw[0] == '?') color = qmarkColor;
            else if (raw[0] == '*') color = starColor;
            std::string truncated = smartTruncate(raw, column_width);
            item.visual_length = (int)truncated.length();
            item.display_text = std::string(color) + truncated + resetPlain;
        } else {
            const char* relative = raw + base_len;
            bool is_dir = isDirectory(raw);
            if (is_dir) {
                std::string truncated = smartTruncate(relative, column_width - 1);
                item.visual_length = (int)truncated.length() + 1;
                item.display_text = std::string(rc.dir) + truncated + "/" + rc.reset;
            } else {
                std::string truncated = smartTruncate(relative, column_width);
                item.visual_length = (int)truncated.length();
                item.display_text = std::string(rc.file) + truncated + rc.reset;
            }
        }
    }

    // --- Render grid ---
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < num_columns; col++) {
            int page_offset = row + col * rows;
            if (page_offset < items_to_display) {
                const auto& item = display_items[page_offset];
                printf("%s", item.display_text.c_str());

                // Add padding if not last column and not last item
                if (col < num_columns - 1 && page_offset < items_to_display - 1) {
                    int padding = std::max(0, total_column_width - item.visual_length);
                    for (int i = 0; i < padding; i++) printf(" ");
                }
            }
        }
        printf("\033[0m\n");
    }

    // --- Footer pagination ---
    if (GlobalState::ITEMS_PER_PAGE > 0 && (size_t)num_matches > GlobalState::ITEMS_PER_PAGE) {
        printf("\n%s[%d/%d matches — press %sTab%s for next page]%s\n",
               rc.label, start_index + items_to_display - 1, num_matches,
               rc.hint, rc.label, rc.reset);
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

//=============================================================================
// Event Driven Key Section
//=============================================================================

/* --- Navigation & View Handlers --- */

int pgup_handler(int, int) {
    rl_replace_line("PgUp", 0);
    rl_done = 1;
    return 0;
}

int pgdn_handler(int, int) {
    rl_replace_line("PgDn", 0);
    rl_done = 1;
    return 0;
}

int toggleList_handler(int, int) {
    rl_replace_line("~", 0);
    rl_done = 1;
    return 0;
}

/* --- Action & Command Handlers --- */

int proc_handler(int, int) {
    rl_replace_line("proc", 0);
    rl_done = 1;
    return 0;
}

int clr_handler(int, int) {
    rl_replace_line("clr", 0);
    rl_done = 1;
    return 0;
}

int refresh_handler(int, int) {
    rl_replace_line("R", 0);
    rl_done = 1;
    return 0;
}

int filter_handler(int, int) {
    rl_replace_line("/", 0);
    rl_done = 1;
    return 0;
}

int toggleFlno_handler(int, int) {
    rl_replace_line("*", 0);
    rl_done = 1;
    return 0;
}

/* --- Settings & State Handlers --- */

int save_handler(int, int) {
    rl_replace_line("s", 0);
    rl_done = 1;
    return 0;
}

int reset_handler(int, int) {
    rl_replace_line("r", 0);
    rl_done = 1;
    return 0;
}

/* --- Global Utility Handlers --- */

int help_handler(int, int) {
    rl_replace_line("?", 0);
    rl_done = 1;
    return 0;
}

int exit_handler(int, int) {
    rl_replace_line("<", 0);
    rl_done = 1;
    return 0;
}

/**
 * @brief Keybindings for the main File Selection interface.
 */
void setup_custom_keybindingsForSelect(void) {
    // Navigation
    rl_bind_keyseq("\\e[5~", pgup_handler);
    rl_bind_keyseq("\\e[6~", pgdn_handler);

    // Commands
    rl_bind_keyseq("*", toggleFlno_handler);
    rl_bind_keyseq("/", filter_handler);
    rl_bind_keyseq("P", proc_handler);
    rl_bind_keyseq("C", clr_handler);
    rl_bind_keyseq("R", refresh_handler);
    rl_bind_keyseq("~", toggleList_handler);
    rl_bind_keyseq("<", exit_handler);
    rl_bind_keyseq("?", help_handler);
}

/**
 * @brief Keybindings for the Settings Editor.
 */
void setup_custom_keybindingsForSettingsEditor(void) {
    rl_bind_keyseq("r", reset_handler);
    rl_bind_keyseq("?", help_handler);
    rl_bind_keyseq("<", exit_handler);
}

/**
 * @brief Keybindings for the Search Prompts.
 */
void setup_custom_keybindingsForSearches(void) {
    rl_bind_keyseq("?", help_handler);
    rl_bind_keyseq("<", exit_handler);
}

/**
 * @brief Restores all bindings used in the Selection UI to defaults.
 */
void reset_custom_keybindingsForSelect(void) {
    // Restore navigation to history
    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

    // Restore characters to standard insertion
    rl_bind_keyseq("*", rl_insert);
    rl_bind_keyseq("/", rl_insert);
    rl_bind_keyseq("P", rl_insert);
    rl_bind_keyseq("C", rl_insert);
    rl_bind_keyseq("R", rl_insert);
    rl_bind_keyseq("~", rl_insert);
    rl_bind_keyseq("<", rl_insert);
    rl_bind_keyseq("?", rl_insert);
}

/**
 * @brief Restores bindings used in File Operations (Cp/Mv/USB).
 */
void reset_custom_keybindingsForCpMvWrite2Usb(void) {
    rl_bind_keyseq("*", rl_insert);
    rl_bind_keyseq("/", rl_insert);
    rl_bind_keyseq("P", rl_insert);
    rl_bind_keyseq("R", rl_insert);
    rl_bind_keyseq("C", rl_insert);
    rl_bind_keyseq("~", rl_insert);
}

/**
 * @brief Restores bindings used in the Settings Editor.
 */
void reset_custom_keybindingsForRm(void) {
    // Restore characters to standard insertion
    rl_bind_keyseq("*", rl_insert);
    rl_bind_keyseq("/", rl_insert);
    rl_bind_keyseq("P", rl_insert);
    rl_bind_keyseq("C", rl_insert);
    rl_bind_keyseq("R", rl_insert);
    rl_bind_keyseq("~", rl_insert);
    rl_bind_keyseq("<", rl_insert);
    rl_bind_keyseq("?", rl_insert);
}

/**
 * @brief Restores bindings used in the Settings Editor.
 */
void reset_custom_keybindingsForSettingsEditor(void) {
    rl_bind_keyseq("r", rl_insert);
    rl_bind_keyseq("?", rl_insert);
    rl_bind_keyseq("<", rl_insert);
}

void reset_custom_keybindingsForSearches(void) {
    rl_bind_keyseq("?", rl_insert);
    rl_bind_keyseq("<", rl_insert);
}
