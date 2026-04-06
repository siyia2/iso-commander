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
    
    // Use Cyan for title and Green for headers in original mode
    std::string titleC  = isOriginal ? std::string(originalColors::cyan) : std::string(theme->accent);
    std::string headC   = isOriginal ? std::string(originalColors::green) : std::string(theme->accent);
    std::string yellowC = std::string(originalColors::yellow);
    std::string blueC   = std::string(originalColors::blue);

    std::cout << "\n" << titleC << "===== Help Guide For Lists =====" << originalColors::reset << "\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:" << originalColors::reset << "\n"
              << "   • Quick Return:" << yellowC << " Ctrl+d " << originalColors::reset << "\n"
              << "   • Clear Line:" << yellowC << " Ctrl+u " << originalColors::reset << "\n" << std::endl;
    
    std::cout << headC << "2. Selecting Items:" << originalColors::reset << "\n"
              << "   • Single item: Enter a number (e.g., '1')\n"
              << "   • Multiple items: Separate with spaces (e.g., '1 5 6')\n"
              << "   • Range of items: Use a hyphen (e.g., '1-3')\n"
              << "   • Combine methods: '1-3 5 7-9'\n"
              << "   • Mark as pending: '1-3 5 7-9;'\n"
              << "   • Select all: Enter '00' (for mount/umount only)\n" << std::endl;
    
    std::cout << headC << "3. Special Commands:" << originalColors::reset << "\n"
              << "   • Enter " << blueC << "'~'" << originalColors::reset << " - Switch between compact and full list\n"
              << "   • Enter " << blueC << "'*'" << originalColors::reset << " - Toggle filename-only lists (requires unfiltered, 'umount' excluded)\n"
              << "   • Enter " << blueC << "'/'" << originalColors::reset << " - Filter the current list based on search terms (e.g., 'term' or 'term1;term2')\n"
              << "   • Enter " << blueC << "'/term1;term2'" << originalColors::reset << " - Directly filter the list for items containing 'term1' or 'term2'\n"
              << "   • Enter " << blueC << "'n'" << originalColors::reset << " - Go to next page if pages > 1\n"
              << "   • Enter " << blueC << "'p'" << originalColors::reset << " - Go to previous page if pages > 1\n"
              << "   • Enter " << blueC << "'g<num>'" << originalColors::reset << " - Go to page if pages > 1 (e.g., 'g3')\n"
              << "   • Enter " << blueC << "'proc'" << originalColors::reset << " - Process pending items\n"
              << "   • Enter " << blueC << "'clr'" << originalColors::reset << " - Clear pending items\n" << std::endl;
              
    std::cout << headC << "4. Tips:" << originalColors::reset << "\n"
              << "   • Indexes correspond only to their generated list" << originalColors::reset << "\n"
              << "   • Indexes^ refer to the original unfiltered list" << originalColors::reset << "\n\n"
              << "   • Filtering is adaptive, incremental, and unconstrained by pagination" << originalColors::reset << "\n"
              << "   • If filtering has no matches, no message or list update is issued\n" << std::endl;
              
    std::cout << color << "↵ to return..." << originalColors::reset;
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
    
    std::string titleC  = isOriginal ? std::string(originalColors::cyan) : std::string(theme->accent);
    std::string headC   = isOriginal ? std::string(originalColors::green) : std::string(theme->accent);
    std::string yellowC = std::string(originalColors::yellow);
    std::string blueC   = std::string(originalColors::blue);
    std::string orangeC = std::string(originalColors::orange);
    std::string purpleC = std::string(originalColors::purple);

    std::cout << "\n" << titleC << "===== Help Guide For " 
          << (isCpMv ? "Cp/Mv FolderPath" : (import2ISO ? "Import2ISO FolderPath" : "Convert2ISO FolderPath")) 
          << " Prompt =====" << originalColors::reset << "\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:" << originalColors::reset << "\n"
               << "     • Quick Return: " << yellowC << " Ctrl+d " << originalColors::reset << "\n"
               << (!isCpMv ? "     • Cancel Search: " + yellowC + " Ctrl+c " + std::string(originalColors::reset) + "\n" : "")
               << "     • Clear Line:   " << yellowC << " Ctrl+u " << originalColors::reset << "\n" << std::endl;
               
    std::cout << headC << "2. Selecting FolderPaths:" << originalColors::reset << "\n"
              << "     • Single directory: Enter a directory (e.g., '/directory/')\n"
              << "     • Multiple directories: Separate with ; (e.g., '/directory1/;/directory2/')" << (isCpMv ? "" : "\n") << std::endl;
    
    if (isCpMv) {
        std::cout << "     • Overwrite files for cp/mv: Append -o (e.g., '/directory/ -o' or '/directory1/;/directory2/ -o')\n" << std::endl;
        
        std::cout << headC << "2. Tips:" << originalColors::reset << "\n"
        << "     • Performing mv to a single destination path on the same device is instant\n"
        << "     • Performing mv to multiple destination paths uses cp and fs::remove (slower)\n" << std::endl;
    }
    
    if (!isCpMv) {
        std::cout << headC << "| ✓ Special Commands starting with '!', '?', or '*' are supported by Tab completion |\n\n";
        std::cout << headC << "3. Special Cleanup Commands:" << originalColors::reset << "\n";
        if (!import2ISO) {
            std::cout << "     • Enter " << yellowC << "'!clr'" << originalColors::reset << "        - Clear the corresponding buffer\n";
        }
        if (import2ISO) {
            std::cout << "     • Enter " << yellowC << "'!clr'" << originalColors::reset << "       - Clear ISO database\n";
        }
        std::cout << "     • Enter " << yellowC << "'!clr_paths'" << originalColors::reset << "  - Clear FolderPath database\n"
                  << "     • Enter " << yellowC << "'!clr_filter'" << originalColors::reset << " - Clear FilterTerm database\n" << std::endl;

        std::cout << headC << "4. Special Display Commands:" << originalColors::reset << "\n";
        if (!import2ISO) {
            std::cout << "     • Enter " << blueC << "'ls'" << originalColors::reset << "           - List corresponding cached entries\n";
        }
            std::cout << "     • Enter " << blueC << "'?config'" << originalColors::reset << "      - Display current configuration\n";
            std::cout << "     • Enter " << blueC << "'?stats'" << originalColors::reset << "       - Display application statistics\n" << std::endl;
                    
        std::cout << headC << "5. Configuration Commands (persistent - saved to config file):" << originalColors::reset << "\n\n";
        
        std::cout << "    " << orangeC << "A. Set Max Items/Page (default: 25):" << originalColors::reset << "\n"
          << "        • Enter " << purpleC << "'*pagination:{number}'" << originalColors::reset << " (e.g., " << purpleC << "'*pagination:50'" << originalColors::reset << ")\n"
          << "        • Disable: " << purpleC << "{number}" << originalColors::reset << " == 0 (e.g., " << purpleC << "'*pagination:0'" << originalColors::reset << ")\n" << std::endl;
                     
        std::cout << orangeC << "    B. Set Default Display Modes (fl = full list, cl = compact list | default: cl, unmount → fl):" << originalColors::reset << "\n"
                <<  "        • Mount list:        Enter " << purpleC << "'*fl_m'" << originalColors::reset << " or " << purpleC << "'*cl_m'\n"
                <<  "        • Umount list:       Enter " << purpleC << "'*fl_u'" << originalColors::reset << " or " << purpleC << "'*cl_u'\n"
                <<  "        • cp/mv/rm list:     Enter " << purpleC << "'*fl_o'" << originalColors::reset << " or " << purpleC << "'*cl_o'\n"
                <<  "        • Write list:        Enter " << purpleC << "'*fl_w'" << originalColors::reset << " or " << purpleC << "'*cl_w'\n"
                <<  "        • Conversion lists: Enter " << purpleC << "'*fl_c'" << originalColors::reset << " or " << purpleC << "'*cl_c'\n"
                <<  "        • Combine settings: Use multiple letters after " << purpleC << "'*fl_'" << originalColors::reset << " or " << purpleC << "'*cl_'" << originalColors::reset << " (e.g., " << purpleC << "'*cl_mu'" << originalColors::reset << ")\n"
              << std::endl;

        std::cout << orangeC << "    C. Filename-only Lists (default: on):" << originalColors::reset << "\n"
                  << "        • Enter " << purpleC << "'*flno:on'" << originalColors::reset << " or " << purpleC << "'*flno:off'" << originalColors::reset << " - Enable/Disable filename-only lists\n\n";

        std::cout << orangeC << "    D. Skin Color (default: white):" << originalColors::reset << "\n"
                  << "        • Enter " << purpleC << "'*skin:{color}'" << originalColors::reset << " - Valid: green, cyan, white, purple, amber, rose\n\n";

        std::cout << orangeC << "    E. UI Theme (default: original):" << originalColors::reset << "\n"
                  << "        • Enter " << purpleC << "'*theme:{name}'" << originalColors::reset << " - Valid: original, classic, high_contrast, neon, etc.\n\n";
              
        if (import2ISO) { 
            std::cout << "    " << orangeC << "F. Auto-Update ISO Database (default: off):" << originalColors::reset << "\n"
            << "        • Enter " << purpleC << "'*auto:on'" << originalColors::reset << " or " << purpleC << "'*auto:off'" << originalColors::reset << " - Enable/Disable background imports\n\n";
        }
    }
    
    std::cout << color << "↵ to return..." << originalColors::reset;
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
    
    std::string titleC  = isOriginal ? std::string(originalColors::cyan) : std::string(theme->accent);
    std::string headC   = isOriginal ? std::string(originalColors::green) : std::string(theme->accent);
    std::string yellowC = std::string(originalColors::yellow);

    std::cout << "\n" << titleC << "===== Help Guide For Mappings =====" << originalColors::reset << "\n" << std::endl;
    
    std::cout << headC << "1. Hotkeys:" << originalColors::reset << "\n"
              << "   • Quick Return:" << yellowC << " Ctrl+d " << originalColors::reset << "\n"
              << "   • Clear Line:" << yellowC << " Ctrl+u " << originalColors::reset << "\n"
              << "   • Declutter Screen:" << yellowC << " Ctrl+l " << originalColors::reset << "\n" << std::endl;
    
    std::cout << headC << "2. Selecting Mappings:" << originalColors::reset << "\n"
              << "   • Mapping = NewISOIndex>RemovableUSBDevice\n"
              << "   • Single mapping: Enter a mapping (e.g., '1>/dev/sdc')\n"
              << "   • Multiple mappings: Separate with ; (e.g., '1>/dev/sdc;2>/dev/sdd')\n" << std::endl;
    
    std::cout << headC << "3. Tips:" << originalColors::reset << "\n"
              << "   • AutoComplete INDEX>DEVICE mappings with Tab" << originalColors::reset << "\n"
              << "   • Partitions are not eligible for write, only raw devices (e.g., '/dev/sdc')\n"
              << "   • USB detection relies on '/sys/class/block/sd*/removable' kernel value\n" << std::endl;
               
    std::cout << color << "↵ to return..." << originalColors::reset;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
