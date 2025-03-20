// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../readline.h"


// Custom readline completion for displaying matching list in search prompts
void customListingsFunction(char **matches, int num_matches, int max_length) {
    (void)max_length; // Silencing unused parameter warning
    
    // Save the current cursor position
    printf("\033[s");
    // Clear any listings if visible and leave a new line
    std::cout << "\033[J";
    printf("\n");
    
    // Calculate how many items to display based on ITEMS_PER_PAGE
    // If ITEMS_PER_PAGE <= 0, show all matches
    int items_to_display;
    if (ITEMS_PER_PAGE <= 0) {
        items_to_display = num_matches;
    } else {
        // Fix signedness comparison issue by casting
        items_to_display = ((size_t)num_matches > ITEMS_PER_PAGE) ? 
                           (int)ITEMS_PER_PAGE : num_matches;
    }
    
    // Print header if we have multiple matches
    if (num_matches > 1) {
        printf("\n\033[1;38;5;130mTab Completion Matches (\033[1;93mCtrl+l\033[0;1m → clear\033[1;38;5;130m):\033[0m\n\n");
    }
    
    // Find common prefix among matches
    const char* base_path = matches[1];
    int base_len = 0;
    
    // Find the last occurrence of '/' before the part we're tab-completing
    const char* last_slash = strrchr(base_path, '/');
    if (last_slash != NULL) {
        base_len = last_slash - base_path + 1; // Include the slash
    }
    
    // Determine the maximum length of all items
    size_t max_item_length = 0;
    
    for (int i = 1; i <= items_to_display; i++) {
        const char* relative_path = matches[i] + base_len;
        size_t item_length = strlen(relative_path);
        
        if (item_length > max_item_length) {
            max_item_length = item_length;
        }
    }
    
    // Calculate number of columns based on items to display
    int num_columns = 3; // Default to 3 columns
    if (items_to_display <= 2) {
        // If we have 1 or 2 items, use fewer columns
        num_columns = items_to_display;
    }
    
    // Define column parameters
    const int column_spacing = 4;
    int column_width = 40; // Default width
    
    // Adjust column width based on number of columns and item length
    if (num_columns < 3) {
        // For fewer columns, we can use wider columns if needed
        // Use the max item length plus padding, but cap at a reasonable size
        column_width = (max_item_length + 2 > 60) ? 60 : max_item_length + 2;
    } else {
        // For 3 columns, use adaptive width but with more conservative limits
        if (max_item_length < 38) {
            column_width = max_item_length + 2; // Add 2 for padding
        }
    }
    
    const int total_column_width = column_width + column_spacing;
    
    // Calculate rows needed
    int rows = (items_to_display + num_columns - 1) / num_columns;
    
    // Function to check if a path is a directory
    auto isDirectory = [](const char* path) -> bool {
        struct stat path_stat;
        if (stat(path, &path_stat) != 0) {
            return false; // If stat fails, assume it's not a directory
        }
        return S_ISDIR(path_stat.st_mode);
    };
    
    // Function for smart truncation
    auto smartTruncate = [](const char* str, int max_width) -> std::string {
        std::string result(str);
        size_t len = result.length();
        
        if (len <= (size_t)max_width) {
            return result; // No truncation needed
        }
        
        // Find file extension if present
        size_t dot_pos = result.find_last_of('.');
        bool has_extension = (dot_pos != std::string::npos && dot_pos > 0 && len - dot_pos <= 10);
        
        // If we have a reasonable extension length (<=10 chars), preserve it
        if (has_extension && dot_pos > 0) {
            std::string ext = result.substr(dot_pos);
            
            // Minimum chars to keep at beginning (at least 3)
            int prefix_len = std::max(3, max_width - (int)ext.length() - 3);
            
            if (prefix_len >= 3) {
                // We have enough space for prefix + ... + extension
                return result.substr(0, prefix_len) + "..." + ext;
            }
        }
        
        // For other cases or very long extensions, use middle truncation
        int prefix_len = (max_width - 3) / 2;
        int suffix_len = max_width - 3 - prefix_len;
        
        return result.substr(0, prefix_len) + "..." + 
               result.substr(len - suffix_len, suffix_len);
    };
    
    // Print matches in the determined number of columns
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < num_columns; col++) {
            int index = row + col * rows + 1;
            
            if (index <= items_to_display) {
                const char* full_path = matches[index];
                const char* relative_path = full_path + base_len;
                
                // Check if the path is a directory
                bool is_dir = isDirectory(full_path);
                
                // Apply color and format based on file type
                std::string formatted;
                
                if (is_dir) {
                    // Use blue color for directories and append a slash
                    formatted = "\033[1;34m" + smartTruncate(relative_path, column_width - 1) + "/\033[0m";
                } else {
                    // Regular color for files
                    formatted = smartTruncate(relative_path, column_width);
                }
                
                // Last column doesn't need padding
                if (col == num_columns - 1 || index == items_to_display) {
                    printf("%s", formatted.c_str());
                } else {
                    // Need to handle ANSI escape codes when padding
                    // Standard padding won't work correctly with color codes, so we add spaces manually
                    int visible_length = strlen(relative_path);
                    if (is_dir) visible_length++; // Account for the slash
                    
                    printf("%s", formatted.c_str());
                    
                    // Add appropriate spacing (accounting for truncation)
                    int displayed_length = std::min((int)visible_length, column_width);
                    int padding = total_column_width - displayed_length;
                    for (int i = 0; i < padding; i++) {
                        printf(" ");
                    }
                }
            }
        }
        printf("\n");
    }
    
    // Only show the pagination message if we're actually limiting results
    // and ITEMS_PER_PAGE is positive
    if (ITEMS_PER_PAGE > 0 && (size_t)num_matches > ITEMS_PER_PAGE) {
        printf("\n\033[1;33m[Showing %d/%d matches... increase pagination limit to display more]\033[0;1m\n", 
               items_to_display, num_matches);
    }
    
    // Move the cursor back to the saved position
    printf("\033[u");
}


// Custom readline function for auto-completing write2usb mappings

// Global structure to hold completion data
CompleterData g_completerData = {nullptr, nullptr};


// Custom readline completion for write2usb function
char** completion_cb(const char* text, int start, int end) {
    rl_attempted_completion_over = 1; // Tell Readline we'll handle completion
    char** matches = nullptr;
    std::string current_word(rl_line_buffer + start, end - start);
    
    // Check if the current word contains a '>' character
    bool is_device_completion = (current_word.find('>') != std::string::npos);
    
    // Handle ISO index completion (N> format)
    if (!is_device_completion) {
        // Complete ISO indexes from sortedIsos
        if (g_completerData.sortedIsos) {
            // First collect all possible completions
            std::vector<std::string> possible_completions;
            size_t list_index = 0;
            while (list_index < g_completerData.sortedIsos->size()) {
                const std::string opt = std::to_string(++list_index) + ">";
                if (opt.find(text) == 0)
                    possible_completions.push_back(opt);
            }
            
            // Check if all possible completions are already in the prompt
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_completions) {
                if (full_line.find(comp) == std::string::npos) {
                    all_present = false;
                    break;
                }
            }
            
            // Only provide completions if not all are present
            if (!all_present && !possible_completions.empty()) {
                matches = rl_completion_matches(text, [](const char* text, int state) -> char* {
                    static size_t list_index;
                    if (!state) {
                        list_index = 0;
                        // Prevent trailing whitespace from being appended
                        rl_completion_append_character = '\0';
                    }
                    while (list_index < g_completerData.sortedIsos->size()) {
                        const std::string opt = std::to_string(++list_index) + ">";
                        if (opt.find(text) == 0)
                            return strdup(opt.c_str());
                    }
                    return (char*)nullptr; // Explicitly cast nullptr to char*
                });
            }
        }
    }
    // Handle device path completion
    else {
        // Complete device paths from usbDevices
        if (g_completerData.usbDevices) {
            // Convert the full text to a std::string
            std::string fullText(text);
            // Find the last '>' character to separate the prefix from the device part
            size_t pos = fullText.find_last_of('>');
            std::string prefix, deviceSubText;
            if (pos != std::string::npos) {
                prefix = fullText.substr(0, pos + 1);
                deviceSubText = fullText.substr(pos + 1);
            } else {
                // Fallback if for some reason there is no '>' character
                deviceSubText = fullText;
            }
            
            // Collect all possible device completions
            std::vector<std::string> possible_device_completions;
            for (size_t i = 0; i < g_completerData.usbDevices->size(); i++) {
                const std::string& dev = (*g_completerData.usbDevices)[i];
                if (dev.find(deviceSubText) == 0) {
                    std::string completion = prefix + dev;
                    possible_device_completions.push_back(completion);
                }
            }
            
            // Check if all possible completions are already in the prompt
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_device_completions) {
                if (full_line.find(comp) == std::string::npos) {
                    all_present = false;
                    break;
                }
            }
            
            // Only provide completions if not all are present
            if (!all_present && !possible_device_completions.empty()) {
                // Use static variables to pass data to the lambda (to avoid capture issues)
                static std::string s_prefix;
                static std::string s_deviceSubText;
                s_prefix = prefix;
                s_deviceSubText = deviceSubText;
                
                // Prevent trailing whitespace from being appended
                rl_completion_append_character = '\0';
                matches = rl_completion_matches(fullText.c_str(), [](const char* /*unused*/, int state) -> char* {
                    static size_t list_index;
                    if (!state) {
                        list_index = 0;
                    }
                    while (list_index < g_completerData.usbDevices->size()) {
                        const std::string& dev = (*g_completerData.usbDevices)[list_index++];
                        // Check if the device name starts with the device part
                        if (dev.find(s_deviceSubText) == 0) {
                            // Prepend the prefix to the matched device name
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


// General readline functions

// Function to restore readline history keys but prevent declutter and listings
void restoreReadline() {
    rl_completion_display_matches_hook = customListingsFunction;
    rl_attempted_completion_function = nullptr;
    rl_bind_keyseq("\033[A", rl_get_previous_history);
    rl_bind_keyseq("\033[B", rl_get_next_history);
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
}


// Function to disable readline history,declutter and listings
void disableReadlineForConfirmation() {
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    rl_bind_keyseq("\033[A", prevent_readline_keybindings);
    rl_bind_keyseq("\033[B", prevent_readline_keybindings);
}


// Function to negate original readline bindings
int prevent_readline_keybindings(int, int) {
    // Do nothing and return 0 
    return 0;
}


// Function to clear scroll buffer in addition to clearing screen with ctrl+l
int clear_screen_and_buffer(int, int) {
    // Clear scroll buffer and screen (works in most terminals)
    clearScrollBuffer();
    fflush(stdout);
    // Force readline to redisplay with the current prompt
    rl_forced_update_display();
    return 0;
}
