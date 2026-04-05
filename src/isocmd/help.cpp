// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

/**
 * @brief Retrieves the current theme object and checks if the active theme is the 'original' preset.
 * * @param[out] theme Reference to a pointer that will hold the active ListTheme.
 * @param[out] isOriginal Reference to a boolean set to true if the theme is "original".
 */
void getThemeState(const ListTheme*& theme, bool& isOriginal) {
    theme = getActiveTheme();
    isOriginal = (globalTheme == "original");
}

/**
 * @brief Displays an interactive help guide detailing how to select and filter items within lists.
 * * Covers hotkeys (Ctrl+d, Ctrl+u), index selection syntax (ranges, multiple items, pending markers), 
 * and special list commands like pagination and filtering.
 */
void helpSelections() {
    const ListTheme* theme;
    bool isOriginal;
    getThemeState(theme, isOriginal);

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();
    
    std::string titleC  = isOriginal ? "\033[1;36m" : std::string(theme->accent);
    std::string headC   = isOriginal ? "\033[1;32m" : std::string(theme->accent);
    std::string yellowC = "\033[1;33m";
    std::string blueC   = "\033[1;34m";

    std::cout << "\n" << titleC << "===== Help Guide For Lists =====\033[0m\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:\033[0m\n"
               << "   • Quick Return:" << yellowC << " Ctrl+d \033[0m\n"
               << "   • Clear Line:" << yellowC << " Ctrl+u \033[0m\n" << std::endl;
    
    std::cout << headC << "2. Selecting Items:\033[0m\n"
              << "   • Single item: Enter a number (e.g., '1')\n"
              << "   • Multiple items: Separate with spaces (e.g., '1 5 6')\n"
              << "   • Range of items: Use a hyphen (e.g., '1-3')\n"
              << "   • Combine methods: '1-3 5 7-9'\n"
              << "   • Mark as pending: '1-3 5 7-9;'\n"
              << "   • Select all: Enter '00' (for mount/umount only)\n" << std::endl;
    
    std::cout << headC << "3. Special Commands:\033[0m\n"
              << "   • Enter " << blueC << "'~'\033[0m - Switch between compact and full list\n"
              << "   • Enter " << blueC << "'*'\033[0m - Toggle filename-only lists (requires unfiltered, 'umount' excluded)\n"
              << "   • Enter " << blueC << "'/'\033[0m - Filter the current list based on search terms (e.g., 'term' or 'term1;term2')\n"
              << "   • Enter " << blueC << "'/term1;term2'\033[0m - Directly filter the list for items containing 'term1' or 'term2'\n"
              << "   • Enter " << blueC << "'n'\033[0m - Go to next page if pages > 1\n"
              << "   • Enter " << blueC << "'p'\033[0m - Go to previous page if pages > 1\n"
              << "   • Enter " << blueC << "'g<num>'\033[0m - Go to page if pages > 1 (e.g., 'g3')\n"
              << "   • Enter " << blueC << "'proc'\033[0m - Process pending items\n"
              << "   • Enter " << blueC << "'clr'\033[0m - Clear pending items\n" << std::endl;
              
    std::cout << headC << "4. Tips:\033[0m\n"
              << "   • Indexes correspond only to their generated list\033[0m\n"
              << "   • Indexes^ refer to the original unfiltered list\033[0m\n\n"
              << "   • Filtering is adaptive, incremental, and unconstrained by pagination\033[0m\n"
              << "   • If filtering has no matches, no message or list update is issued\n" << std::endl;
              
    std::cout << color << "↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Displays a help guide for directory-related prompts (Copy/Move and ISO Conversion).
 * * Provides instructions on entering paths, using overwrite flags, clearing databases, 
 * and persistent configuration commands (pagination, display modes, themes, and skins).
 * * @param isCpMv Boolean flag indicating if the help is for Copy/Move operations.
 * @param import2ISO Boolean flag indicating if the help is for the Import2ISO tool.
 */
void helpSearches(bool isCpMv, bool import2ISO) {
    const ListTheme* theme;
    bool isOriginal;
    getThemeState(theme, isOriginal);

    std::signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();
    
    std::string titleC  = isOriginal ? "\033[1;36m" : std::string(theme->accent);
    std::string headC   = isOriginal ? "\033[1;32m" : std::string(theme->accent);
    std::string yellowC = "\033[1;33m";
    std::string blueC   = "\033[1;34m";
    std::string orangeC = "\033[1;38;5;208m";
    std::string purpleC = "\033[1;35m";

    std::cout << "\n" << titleC << "===== Help Guide For " 
          << (isCpMv ? "Cp/Mv FolderPath" : (import2ISO ? "Import2ISO FolderPath" : "Convert2ISO FolderPath")) 
          << " Prompt =====\033[0m\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:\033[0m\n"
               << "     • Quick Return: " << yellowC << " Ctrl+d \033[0m\n"
               << (!isCpMv ? "     • Cancel Search: " + yellowC + " Ctrl+c \033[0m\n" : "")
               << "     • Clear Line:   " << yellowC << " Ctrl+u \033[0m\n" << std::endl;
               
    std::cout << headC << "2. Selecting FolderPaths:\033[0m\n"
              << "     • Single directory: Enter a directory (e.g., '/directory/')\n"
              << "     • Multiple directories: Separate with ; (e.g., '/directory1/;/directory2/')" << (isCpMv ? "" : "\n") << std::endl;
    
    if (isCpMv) {
        std::cout << "     • Overwrite files for cp/mv: Append -o (e.g., '/directory/ -o' or '/directory1/;/directory2/ -o')\n" << std::endl;
        
        std::cout << headC << "2. Tips:\033[0m\n"
        << "     • Performing mv to a single destination path on the same device is instant\n"
        << "     • Performing mv to multiple destination paths uses cp and fs::remove (slower)\n" << std::endl;
    }
    
    if (!isCpMv) {
        std::cout << headC << "| ✓ Special Commands below starting with '!', '?', or '*' are supported by Tab completion |\n\n";
        std::cout << headC << "3. Special Cleanup Commands:\033[0m\n";
        if (!import2ISO) {
            std::cout << "     • Enter " << yellowC << "'!clr'\033[0m        - Clear the corresponding buffer\n";
        }
        if (import2ISO) {
            std::cout << "     • Enter " << yellowC << "'!clr'\033[0m       - Clear ISO database\n";
        }
        std::cout << "     • Enter " << yellowC << "'!clr_paths'\033[0m  - Clear FolderPath database\n"
                  << "     • Enter " << yellowC << "'!clr_filter'\033[0m - Clear FilterTerm database\n" << std::endl;

        std::cout << headC << "4. Special Display Commands:\033[0m\n";
        if (!import2ISO) {
            std::cout << "     • Enter " << blueC << "'ls'\033[0m           - List corresponding cached entries\n";
        }
            std::cout << "     • Enter " << blueC << "'?config'\033[0m      - Display current configuration\n";
            std::cout << "     • Enter " << blueC << "'?stats'\033[0m       - Display application statistics\n" << std::endl;
                    
        std::cout << headC << "5. Configuration Commands (persistent - saved to config file):\033[0m\n\n";
        
        std::cout << "    " << orangeC << "A. Set Max Items/Page (default: 25):\033[0m\n"
          << "       • Enter " << purpleC << "'*pagination:{number}'\033[0m (e.g., " << purpleC << "'*pagination:50'\033[0m)\n"
          << "       • Disable: " << purpleC << "{number}\033[0m == 0 (e.g., " << purpleC << "'*pagination:0'\033[0m)\n" << std::endl;
                     
        std::cout << orangeC << "    B. Set Default Display Modes (fl = full list, cl = compact list | default: cl, unmount → fl):\033[0m\n"
                <<  "       • Mount list:        Enter \033[1;35m'*fl_m'\033[0m or \033[1;35m'*cl_m'\033[0m\n"
                <<  "       • Umount list:       Enter \033[1;35m'*fl_u'\033[0m or \033[1;35m'*cl_u'\033[0m\n"
                <<  "       • cp/mv/rm list:     Enter \033[1;35m'*fl_o'\033[0m or \033[1;35m'*cl_o'\033[0m\n"
                <<  "       • Write list:        Enter \033[1;35m'*fl_w'\033[0m or \033[1;35m'*cl_w'\033[0m\n"
                <<  "       • Conversion lists: Enter \033[1;35m'*fl_c'\033[0m or \033[1;35m'*cl_c'\033[0m\n"
                <<  "       • Combine settings: Use multiple letters after \033[1;35m'*fl_'\033[0m or \033[1;35m'*cl_'\033[0m (e.g., \033[1;35m'*cl_mu'\033[0m)\n"
              << std::endl;

        std::cout << orangeC << "    C. Filename-only Lists (default: on):\033[0m\n"
                  << "       • Enter " << purpleC << "'*flno:on'\033[0m or " << purpleC << "'*flno:off'\033[0m - Enable/Disable filename-only lists\n\n";

        std::cout << orangeC << "    D. Skin Color (default: white):\033[0m\n"
                  << "       • Enter " << purpleC << "'*skin:{color}'\033[0m - Valid: green, cyan, white, purple, amber, rose\033[m\n\n";

        std::cout << orangeC << "    E. UI Theme (default: original):\033[0m\n"
                  << "       • Enter " << purpleC << "'*theme:{name}'\033[0m - Valid: original, classic, high_contrast, neon, ocean, sunset, forest, midnight, mono, retro, crimson, dracula\033[m\n\n";
              
        if (import2ISO) { 
            std::cout << "    " << orangeC << "F. Auto-Update ISO Database (default: off):\033[0m\n"
            << "       • Enter " << purpleC << "'*auto:on'\033[0m or " << purpleC << "'*auto:off'\033[0m - Enable/Disable background imports\n\n";
        }
    }
    
    std::cout << color << "↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/**
 * @brief Displays a help guide for ISO-to-Device mappings.
 * * Explains the syntax for mapping an ISO index to a raw block device (e.g., '1>/dev/sdc'), 
 * multiple mapping logic, and provides technical tips regarding kernel-level USB detection.
 */
void helpMappings() {
    const ListTheme* theme;
    bool isOriginal;
    getThemeState(theme, isOriginal);

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();
    
    std::string titleC  = isOriginal ? "\033[1;36m" : std::string(theme->accent);
    std::string headC   = isOriginal ? "\033[1;32m" : std::string(theme->accent);
    std::string yellowC = "\033[1;33m";

    std::cout << "\n" << titleC << "===== Help Guide For Mappings =====\033[0m\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:\033[0m\n"
              << "   • Quick Return:" << yellowC << " Ctrl+d \033[0m\n"
              << "   • Clear Line:" << yellowC << " Ctrl+u \033[0m\n"
              << "   • Declutter Screen:" << yellowC << " Ctrl+l \033[0m\n" << std::endl;
    
    std::cout << headC << "2. Selecting Mappings:\033[0m\n"
              << "   • Mapping = NewISOIndex>RemovableUSBDevice\n"
              << "   • Single mapping: Enter a mapping (e.g., '1>/dev/sdc')\n"
              << "   • Multiple mappings: Separate with ; (e.g., '1>/dev/sdc;2>/dev/sdd')\n" << std::endl;
    
    std::cout << headC << "3. Tips:\033[0m\n"
              << "   • AutoComplete INDEX>DEVICE mappings with Tab\033[0m\n"
              << "   • Partitions are not eligible for write, only raw devices (e.g., '/dev/sdc')\n"
              << "   • USB detection relies on '/sys/class/block/sd*/removable' kernel value\n" << std::endl;
               
    std::cout << color << "↵ to return..." << reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
