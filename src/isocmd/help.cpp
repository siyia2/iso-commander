// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"


// Function to display how to select items from lists
void helpSelections() {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For Lists =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m1. Hotkeys:\033[0m\n"
			   << "   • Quick Return:\033[1;33m Ctrl+d \033[0m\n"
			   << "   • Clear Line:\033[1;33m Ctrl+u \033[0m\n" << std::endl;
    
    // Working with indices
    std::cout << "\033[1;32m2. Selecting Items:\033[0m\n"
              << "   • Single item: Enter a number (e.g., '1')\n"
              << "   • Multiple items: Separate with spaces (e.g., '1 5 6')\n"
              << "   • Range of items: Use a hyphen (e.g., '1-3')\n"
              << "   • Combine methods: '1-3 5 7-9'\n"
              << "   • Mark as pending: Append a semicolon '1-3 5 7-9;'\n"
              << "   • Select all: Enter '00' (for mount/umount only)\n" << std::endl;
    
    // Special commands
    std::cout << "\033[1;32m3. Special Commands:\033[0m\n"
			  << "   • Enter \033[1;34m'~'\033[0m - Switch between compact and full list\n"
			  << "   • Enter \033[1;34m'/'\033[0m - Filter the current list based on search terms (e.g., 'term' or 'term1;term2')\n"
              << "   • Enter \033[1;34m'/term1;term2'\033[0m - Directly filter the list for items containing 'term1' or 'term2'\n"
			  << "   • Enter \033[1;34m'n'\033[0m - Go to next page if pages > 1\n"
			  << "   • Enter \033[1;34m'p'\033[0m - Go to previous page if pages > 1\n"
			  << "   • Enter \033[1;34m'g<num>'\033[0m - Go to page if pages > 1 (e.g., 'g3')\n"
			  << "   • Enter \033[1;34m'proc'\033[0m - Process pending items\n"
			  << "   • Enter \033[1;34m'clr'\033[0m - Clear pending items\n" << std::endl;
              
    
     // Selection tips
    std::cout << "\033[1;32m4. Tips:\033[0m\n"
			  << "   • Filtered indexes can be utilized only within their generated list\033[0m\n"
			  << "   • Index^ can be utilized only within the original unfiltered list\033[0m\n"
			  << "   • Filtering is adaptive, incremental, and unconstrained by pagination\033[0m\n"
              << "   • If filtering has no matches, no message or list update is issued\n" << std::endl;
              
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for directory prompts
void helpSearches(bool isCpMv, bool import2ISO) {
    std::signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
    disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For " 
          << (isCpMv ? "Cp/Mv FolderPath" : (import2ISO ? "Import2ISO FolderPath" : "Convert2ISO FolderPath")) 
          << " Prompt =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m1. Hotkeys:\033[0m\n"
			   << "   • Quick Return:\033[1;33m Ctrl+d \033[0m\n"
			   << "   • Clear Line:\033[1;33m Ctrl+u \033[0m\n"
               << "   • Declutter Screen:\033[1;33m Ctrl+l \033[0m\n" << std::endl;
               
    std::cout << "\033[1;32m2. Selecting FolderPaths:\033[0m\n"
              << "   • Single directory: Enter a directory (e.g., '/directory/')\n"
              << "   • Multiple directories: Separate with ; (e.g., '/directory1/;/directory2/')" << (isCpMv ? "" : "\n") << std::endl;
    if (isCpMv) {
        std::cout << "   • Overwrite files for cp/mv: Append -o (e.g., '/directory/ -o' or '/directory1/;/directory2/ -o')\n" << std::endl;
        
        std::cout << "\033[1;32m2. Tips:\033[0m\n"
        << "   • Performing mv on single destination path on the same device is instant\n"
        << "   • Performing mv on multiple destination paths utilizes cp and fs::remove (slower)\n" << std::endl;
    }
    
    if (!isCpMv) {
        std::cout << "\033[1;32m3. Special Cleanup Commands:\033[0m\n";
        if (!import2ISO) {
            std::cout << "   • Enter \033[1;33m'!clr'\033[0m - Clear the corresponding buffer\n";
        }
        if (import2ISO) {
            std::cout << "   • Enter \033[1;33m'!clr'\033[0m - Clear ISO database\n";
        }
		std::cout << "   • Enter \033[1;33m'!clr_paths'\033[0m - Clear FolderPath database\n"
				  << "   • Enter \033[1;33m'!clr_filter'\033[0m - Clear FilterTerm database\n" << std::endl;
		std::cout << "\033[1;32m4. Special Display Commands:\033[0m\n";
        if (!import2ISO) {
            std::cout << "   • Enter \033[1;34m'ls'\033[0m - List corresponding cached entries\n";
        }
			std::cout << "   • Enter \033[1;34m'config'\033[0m - Display current configuration\n";
            std::cout << "   • Enter \033[1;34m'stats'\033[0m - Display application statistics\n" << std::endl;
					
       std::cout << "\033[1;32m" << "5. Configuration Commands:\033[0m\n\n";
       
		std::cout << "   \033[1;38;5;208mA. Set Max Items/Page (default: 25):\033[0m\n"
          << "      • Enter '*pagination_{number}' (e.g., '*pagination_50')\n"
          << "      • Disable: {number} <= 0 (e.g., '*pagination_-1' or '*pagination_0')\n"  << std::endl;
                     
		std::cout << "\033[1;38;5;208m   B. Set Default Display Modes (fl = full list, cl = compact list | default: cl, unmount → fl):\033[0m\n"
				<<  "      • Mount list:       Enter \033[1;35m'*fl_m'\033[0m or \033[1;35m'*cl_m'\033[0m\n"
				<<  "      • Umount list:      Enter \033[1;35m'*fl_u'\033[0m or \033[1;35m'*cl_u'\033[0m\n"
				<<  "      • cp/mv/rm list:    Enter \033[1;35m'*fl_o'\033[0m or \033[1;35m'*cl_o'\033[0m\n"
				<<  "      • Write list:       Enter \033[1;35m'*fl_w'\033[0m or \033[1;35m'*cl_w'\033[0m\n"
				<<  "      • Conversion lists: Enter \033[1;35m'*fl_c'\033[0m or \033[1;35m'*cl_c'\033[0m\n"
				<<  "      • Combine settings: Use multiple letters after \033[1;35m'*fl_'\033[0m or \033[1;35m'*cl_'\033[0m (e.g., \033[1;35m'*cl_mu'\033[0m for mount and umount lists)\n"
              << std::endl;
              
		if (import2ISO) { 
			std::cout << "   \033[1;38;5;208mC. Auto-Update ISO Database (default: disabled):\033[0m\n"
            << "      • Enter \033[1;35m'*auto_on'\033[0m or \033[1;35m'*auto_off'\033[0m - Enable/Disable automatic ISO imports from stored folder paths\n\n";
		}
    }
    
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Help guide for iso and device mapping
void helpMappings() {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    clearScrollBuffer();
    
    // Title
    std::cout << "\n\033[1;36m===== Help Guide For Mappings =====\033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m1. Hotkeys:\033[0m\n"
			  << "   • Quick Return:\033[1;33m Ctrl+d \033[0m\n"
			  << "   • Clear Line:\033[1;33m Ctrl+u \033[0m\n"
              << "   • Declutter Screen:\033[1;33m Ctrl+l \033[0m\n" << std::endl;
    
    std::cout << "\033[1;32m2. Selecting Mappings:\033[0m\n"
			  << "   • Mapping = NewISOIndex>RemovableUSBDevice\n"
              << "   • Single mapping: Enter a mapping (e.g., '1>/dev/sdc')\n"
              << "   • Multiple mappings: Separate with ; (e.g., '1>/dev/sdc;2>/dev/sdd' or '1>/dev/sdc;1>/dev/sdd')\n" << std::endl;
    
    std::cout << "\033[1;32m3. Tips:\033[0m\n"
              << "   • AutoComplete INDEX>DEVICE mappings with Tab\033[0m\n"
              << "   • Partitions are not eligible for write, only raw devices (e.g., '/dev/sdc')\n"
              << "   • USB detection relies on '/sys/class/block/sd*/removable' kernel value\n" << std::endl;
               
    // Prompt to continue
    std::cout << "\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
