// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

// Project Headers
#include "../inputHandling.h"
#include "../pausePrompt.h"
#include "../themes.h"

namespace {

    /**
     * @brief Theme color roles used to pick a fallback color from MainTheme.
     */
    enum class ColorRole : std::uint8_t {
        Accent,
        Highlight,
        Primary,
        Muted,
    };

    /**
     * @brief Resolves a color for the current theme using string_view to avoid copies.
     *
     * When @p isOriginal is true, @p originalColor is returned as-is.
     * Otherwise the themed color matching @p role is returned.
     */
    [[nodiscard]] inline std::string_view resolveColor(
        const MainTheme* theme,
        bool isOriginal,
        std::string_view originalColor,
        ColorRole role) noexcept {
        if (isOriginal || theme == nullptr) {
            return originalColor;
        }

        switch (role) {
            case ColorRole::Accent:    return theme->accent;
            case ColorRole::Highlight:   return theme->highlight;
            case ColorRole::Primary:   return theme->primary;
            case ColorRole::Muted: return theme->muted;
        }
        return originalColor; // unreachable, silences some compilers
    }

    /**
     * @brief Aggregates resolved theme colors used by all help screens.
     *
     * Construct once per help screen; all members are lightweight views into
     * either the global palette or the active theme, so no allocation occurs.
     */
    struct ThemeColors {
        const MainTheme* theme;
        bool isOriginal;

        std::string_view title;
        std::string_view head;
        std::string_view keysYellow;
        std::string_view keysBlue;
        std::string_view keysPurple;

        ThemeColors() noexcept
            : theme(getActiveTheme()),
              isOriginal(globalTheme == "original"),
              title     (resolveColor(theme, isOriginal, UI::Palette::Cyan,   ColorRole::Accent)),
              head      (resolveColor(theme, isOriginal, UI::Palette::Green,  ColorRole::Accent)),
              keysYellow(resolveColor(theme, isOriginal, UI::Palette::Yellow, ColorRole::Highlight)),
              keysBlue  (resolveColor(theme, isOriginal, UI::Palette::Blue,   ColorRole::Primary)),
              keysPurple(resolveColor(theme, isOriginal, UI::Palette::Purple, ColorRole::Muted))
        {}
    };

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
void helpSelections(bool& isAtISOListForHelp, const bool& isUnmount, const bool& isMount) {
    clearScrollBuffer();
    const ThemeColors tc;

    printSection(tc, "\n1. Hotkeys:", std::string(UI::Palette::BoldReset) +
        "   • Exit         : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
        "   • Quick Return : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
        "   • Clear Line   : " + std::string(tc.keysYellow) + "Ctrl+u");

    printSection(tc, isUnmount ? "\n2. Selecting Mount-Points (↵):" : "\n2. Selecting Files (↵):",
        std::string(UI::Palette::BoldReset) +
        "   • Single/Multiple : " + std::string(tc.keysPurple) + "'1' or '1 5 6'\n" +
        std::string(UI::Palette::BoldReset) +
        "   • Range/Combine   : " + std::string(tc.keysPurple) + "'1-3' or '1-3 5 7-9'\n" +
        std::string(UI::Palette::BoldReset) +
        "   • Pending" + ((isUnmount || isMount) ? "/All" : "    ") + "     : " +
        std::string(tc.keysPurple) +
        "'1-3 5;'" + ((isUnmount || isMount) ? " or '00'" : "")
    );
    printSection(tc, "\n3. Special Keys:",
        "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'~'" + std::string(UI::Palette::BoldReset) + "                : View Full/Compact\n" +
        (!isUnmount ?
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'*'" + std::string(UI::Palette::BoldReset) + "                : View FilenamesOnly (¬filtered)\n"
            : "") +
        "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'/'" + std::string(UI::Palette::BoldReset) + "                : Filter (e.g. term1;term2 or term1&term2)\n" +
        (isAtISOListForHelp ?
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'R'" + std::string(UI::Palette::BoldReset) + "                : Refresh ISO list from FolderPath history\n"
            : "") +
        "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'P'|'C' " + std::string(UI::Palette::BoldReset) + "           : Process|Clear pending items\n" +
        "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'PgDn'|'PgUp'|'g#' " + std::string(UI::Palette::BoldReset) + ": Pagination Next|Previous|GoTo page"
    );
    printSection(tc, "\n4. Tips:",
        "   • Indexes correspond only to their generated list\n"
        "   • Indexes^ refer to the original unfiltered list\n"
        "   • Filtering spans all pages and is incremental");
    pressEnterToReturn();
}

/**
 * @brief Displays a help guide for the settings editor.
 */
void helpSettingsEditor() {
    clearScrollBuffer();
    const ThemeColors tc;

    printSection(tc, "\n1. Hotkeys:", std::string(UI::Palette::BoldReset) +
        "   • Exit         : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
        "   • Quick Return : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
        "   • Clear Line   : " + std::string(tc.keysYellow) + "Ctrl+u");
    printSection(tc, "\n2. Selecting Edits (↵):", std::string(UI::Palette::BoldReset) +
        "   • Single/Multiple : " + std::string(tc.keysPurple) + "'1' or '1 5 6'\n" + std::string(UI::Palette::BoldReset) +
        "   • Range/Combine   : " + std::string(tc.keysPurple) + "'1-3' or '1-3 5 7-9'" + std::string(UI::Palette::BoldReset));
        printSection(tc, "\n4. Tips:",
        "   • On reset defaults are auto-saved to disk\n"
        "   • Succesful edits are auto-saved to disk");
    pressEnterToReturn();
}

/**
 * @brief Displays a help guide for directory-related prompts (Copy/Move and ISO convert2iso).
 */
void helpSearches(bool isCpMv, bool import2ISO) {
    clearScrollBuffer();
    const ThemeColors tc;

    // 1. Hotkeys
    std::string keys =
    "   • Exit          : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
    "   • Quick Return  : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset);
    if (!isCpMv) keys += std::string(UI::Palette::BoldReset) + "   • Cancel Search : " + std::string(tc.keysYellow) + "Ctrl+c\n";
    keys += std::string(UI::Palette::BoldReset) + "   • Clear Line    : " + std::string(tc.keysYellow) + "Ctrl+u\n";
    keys += std::string(UI::Palette::BoldReset) + "   • Declutter     : " + std::string(tc.keysYellow) + "Ctrl+l";
    printSection(tc, "\n1. Hotkeys:", keys);

    // 2. Selecting FolderPaths
    std::string paths =
        "   • Single/Multiple : '/dir/' or '/dir1/;/dir2/'\n";
    if (isCpMv)
        paths += "   • Overwrite       : Append -o (e.g., '/dir/ -o')";
    printSection(tc, "\n2. Selecting FolderPaths (↵):", paths);

    if (isCpMv) {
        // 3. Tips (Cp/Mv specific)
        printSection(tc, "\n3. Tips:",
            "   • 'mv' same-device: instant (metadata update)\n"
            "   • 'mv' mult-dest: uses cp+rm (slower)");
    } else {
        // 3. Cleanup/Display (convert2iso / import2iso specific)
        std::string displayCmds =
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysYellow) +
            "'!clr'       " + std::string(UI::Palette::BoldReset) +
            (import2ISO ? " : Clear IsoDatabase\n" : " : Clear corresponding ImageCache\n") +
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysYellow) +
            "'!clr_paths' " + std::string(UI::Palette::BoldReset) + " : Clear FolderPath history\n" +
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysYellow) +
            "'!clr_filter'" + std::string(UI::Palette::BoldReset) + " : Clear FilterTerm history\n";

        if (!import2ISO) {
            displayCmds +=
                "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) +
                "'ls'|'*stats'" + std::string(UI::Palette::BoldReset) + " : Display entries|stats";
        } else {
            displayCmds +=
                "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) +
                "'*stats'     " + std::string(UI::Palette::BoldReset) + " : Display stats";
        }
        printSection(tc, "3. Cleanup/Display Commands (↵):", displayCmds);
        printSection(tc, "\n4. Tips:",
            "   • " + std::string(UI::Palette::BoldReset) + "Tab completion supports ! and * command prefixes\n"
            "   • " + std::string(UI::Palette::BoldReset) + "Invalid paths => non-existent FolderPaths");
    }
    pressEnterToReturn();
}

/**
 * @brief Displays a help guide for ISO-to-Device mappings.
 */
void helpMappings() {
    clearScrollBuffer();
    const ThemeColors tc;

    printSection(tc, "\n1. Hotkeys:",
        "   • Exit         : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
        "   • Quick Return : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
        "   • Clear Line   : " + std::string(tc.keysYellow) + "Ctrl+u\n" + std::string(UI::Palette::BoldReset) +
        "   • Declutter    : " + std::string(tc.keysYellow) + "Ctrl+l");

    printSection(tc, "\n2. Selecting Pairs (↵):",
        "   • Syntax   : Index>Device (e.g., '1>/dev/sdc')\n"
        "   • Multiple : Separate with ';' (e.g., '1>/dev/sdc;2>/dev/sdd')");

    printSection(tc, "\n3. Tips:",
        "   • Press Enter to refresh USB Flash Devices\n"
        "   • Tab-complete INDEX>DEVICE pairs for faster assignment\n"
        "   • Only unmounted parent devices are eligible for write2usb");

    pressEnterToReturn();
}
