// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

namespace {

/**
 * @brief Resolves a color for the current theme using string_view to avoid copies.
 */
inline std::string_view resolveColor(const MainTheme* theme,
                                     bool isOriginal,
                                     std::string_view originalColor) noexcept {
    return isOriginal ? originalColor : std::string_view(theme->accent);
}

/**
 * @brief Aggregates resolved theme colors used by all help screens.
 */
struct ThemeColors {
    const MainTheme* theme = getActiveTheme();
    bool isOriginal = (globalTheme == "original");
    std::string_view title = resolveColor(theme, isOriginal, UI::Palette::Cyan);
    std::string_view head  = resolveColor(theme, isOriginal, UI::Palette::Green);
};

/**
 * @brief Standardizes help screen initialization and header rendering.
 */
void setupHelp(std::string_view title, const ThemeColors& tc) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();
    clearScrollBuffer();
    std::cout << "\n" << tc.title << "===== " << title << " =====" 
              << UI::Palette::BoldReset << "\n" << std::endl;
}

/**
 * @brief Helper to print a themed section header and its bulleted content.
 */
void printSection(const ThemeColors& tc, std::string_view head, const std::string& body) {
    std::cout << tc.head << head << UI::Palette::BoldReset << "\n";
    if (!body.empty()) {
        std::cout << body << std::endl;
    } else {
        std::cout << std::endl; // Just one extra line for spacing if no body
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public help functions
// ---------------------------------------------------------------------------

/**
 * @brief Displays an interactive help guide detailing how to select and filter items within lists.
 */
void helpSelections() {
    const ThemeColors tc;
    setupHelp("Help Guide For Lists", tc);

    printSection(tc, "1. Hotkeys:", std::string(UI::Palette::BoldReset) +
        "   • Quick Return : " + std::string(UI::Palette::Yellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
        "   • Clear Line   : " + std::string(UI::Palette::Yellow) + "Ctrl+u");

    printSection(tc, "\n2. Selecting Items (↵):", std::string(UI::Palette::BoldReset) +
        "   • Single/Multiple : " + std::string(UI::Palette::Purple) + "'1' or '1 5 6'\n" + std::string(UI::Palette::BoldReset) +
        "   • Range/Combine   : " + std::string(UI::Palette::Purple) + "'1-3' or '1-3 5 7-9'\n" + std::string(UI::Palette::BoldReset) +
        "   • Pending/All     : " + std::string(UI::Palette::Purple) + "'1-3 5;' or '00' " + std::string(UI::Palette::Yellow) +"('00' ↔ mount/umount)");

    printSection(tc, "\n3. Special Keys:",
    "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(UI::Palette::Blue) + "'~'|'*'" + std::string(UI::Palette::BoldReset) + "            : View (Full/Compact)|FilenamesOnly (¬filtered, ¬umount)\n" +
    "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(UI::Palette::Blue) + "'/'" + std::string(UI::Palette::BoldReset) + "                : Filter (e.g. term1;term2)\n" +
    "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(UI::Palette::Blue) + "'PgUp'|'PgDn'|'g' " + std::string(UI::Palette::BoldReset) + " : Pagination (Next, Previous, Go to page)\n" +
    "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(UI::Palette::Blue) + "'P'|'C' " + std::string(UI::Palette::BoldReset) + "           : Process or Clear pending indices");
    printSection(tc, "\n4. Tips:",
        "   • Indexes correspond only to their generated list\n"
        "   • Indexes^ refer to the original unfiltered list\n"
        "   • Filtering is adaptive, incremental, and unconstrained by pagination");

    pressEnterToReturn();
}

/**
 * @brief Displays a help guide for directory-related prompts (Copy/Move and ISO convert2iso).
 */
void helpSearches(bool isCpMv, bool import2ISO) {
    const ThemeColors tc;
    std::string titleStr = isCpMv ? "Cp/Mv FolderPath" : (import2ISO ? "Import2ISO FolderPath" : "Convert2ISO FolderPath");
    setupHelp("Help Guide For " + titleStr + " Prompt", tc);

    // 1. Hotkeys
    std::string keys = std::string(UI::Palette::BoldReset) + "   • Quick Return  : " + std::string(UI::Palette::Yellow) + "Ctrl+d\n";
    if (!isCpMv) keys += std::string(UI::Palette::BoldReset) + "   • Cancel Search : " + std::string(UI::Palette::Yellow) + "Ctrl+c\n";
    keys += std::string(UI::Palette::BoldReset) + "   • Clear Line    : " + std::string(UI::Palette::Yellow) + "Ctrl+u";
    printSection(tc, "1. Hotkeys:", keys);

    // 2. Selecting FolderPaths
    printSection(tc, "\n2. Selecting FolderPaths (↵):",
        std::string("   • Single/Multiple : '/dir/' or '/dir1/;/dir2/'\n") +
        (isCpMv ? "   • Overwrite       : Append -o (e.g., '/dir/ -o')\n" : ""));

    if (isCpMv) {
        // 3. Tips (Cp/Mv specific)
        printSection(tc, "3. Tips:", "   • 'mv' to a single destination on the same device is instant\n"
                                     "   • 'mv' to multiple destinations uses cp and remove (slower)");
    } else {
        // 3. Cleanup/Display (convert2iso specific)
        printSection(tc, std::string(UI::Palette::Cyan) + "| Tab completion: Supports ! and ? command prefixes |", "");

        std::string displayCmds = "   " + std::string(UI::Palette::BoldReset) + "•" + std::string(UI::Palette::Yellow) + " '!clr'|'!clr_paths'|'!clr_filter'" + std::string(UI::Palette::BoldReset) + " : Clear corresponding cache|database\n";
        
        displayCmds += "   " + std::string(UI::Palette::BoldReset) + "•" + std::string(UI::Palette::Blue) + " ";
        
        // Aligned spacing for both conditions
        if (!import2ISO) {
            displayCmds += "'ls'|'?stats'           " + std::string(UI::Palette::BoldReset) + "          : Display cached image entries|stats";
        } else {
            // Added extra spaces to compensate for the missing "ls / " string (5 chars)
            displayCmds += "'?stats'               " + std::string(UI::Palette::BoldReset) + "           : Display stats";
        }

        printSection(tc, "3. Cleanup/Display Commands (↵):", displayCmds);
    }

    pressEnterToReturn();
}

/**
 * @brief Displays a help guide for ISO-to-Device mappings.
 */
void helpMappings() {
    const ThemeColors tc;
    setupHelp("Help Guide For Mappings", tc);

    printSection(tc, "1. Hotkeys:",
        std::string(UI::Palette::BoldReset) + "   • Quick Return : " + std::string(UI::Palette::Yellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
        "   • Clear Line   : " + std::string(UI::Palette::Yellow) + "Ctrl+u\n" + std::string(UI::Palette::BoldReset) +
        "   • Declutter    : " + std::string(UI::Palette::Yellow) + "Ctrl+l");

    printSection(tc, "\n2. Selecting Mappings (↵):",
        "   • Syntax   : Index>Device (e.g., '1>/dev/sdc')\n"
        "   • Multiple : Separate with ';' (e.g., '1>/dev/sdc;2>/dev/sdd')");

    printSection(tc, "\n3. Tips:",
        "   • Tab-complete INDEX>DEVICE pairs for faster mapping\n"
        "   • Partitions are not eligible for write2usb, only raw devices\n"
        "   • USB detection relies on '/sys/class/block/sd*/removable' kernel value");

    pressEnterToReturn();
}
