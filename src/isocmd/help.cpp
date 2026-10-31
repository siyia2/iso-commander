// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

// Project Headers
#include "../inputHandling.h"
#include "../rendering.h"
#include "../pausePrompts.h"
#include "../themes.h"

namespace {

    /**
     * @brief Theme color roles used to pick a fallback color from MainTheme.
     *
     * Backed by std::uint8_t and used only internally by resolveColor() and
     * ThemeColors. The set of enumerators is exhaustive for the switch in
     * resolveColor(); adding a new role requires adding a matching case there.
     */
    enum class ColorRole : std::uint8_t {
        Accent,
        Highlight,
        Primary,
        Muted,
    };

    /**
     * @brief Resolves a color for the current theme.
     *
     * When @p isOriginal is true (or @p theme is null), @p originalColor is
     * returned as-is. Otherwise the themed color matching @p role is returned
     * (theme->accent/highlight/primary/muted). Marked noexcept and
     * [[nodiscard]]; the trailing `return originalColor;` after the switch is
     * a defensive fallback that is unreachable for the current ColorRole set.
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
            case ColorRole::Highlight: return theme->highlight;
            case ColorRole::Primary:   return theme->primary;
            case ColorRole::Muted:     return theme->muted;
        }
        return originalColor; // unreachable
    }

    /**
     * @brief Aggregates resolved theme colors used by all help screens.
     *
     * The default constructor reads the current active theme and the
     * "original" flag from globals (getActiveTheme(), globalTheme) at
     * construction time. All members are lightweight std::string_view handles
     * into either the global palette or the active theme, so no allocation
     * occurs.
     *
     * Note: help functions construct a fresh ThemeColors inside their render
     * lambda on every replay (including resize redraws), so theme changes made
     * while the help screen is displayed are reflected on the next repaint.
     * The "construct once per help screen" intuition is therefore wrong — one
     * instance is constructed per draw.
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
 * @brief Prints a themed section header followed by its body.
 *
 * The header is printed in the theme's section color (tc.head) followed by
 * UI::Palette::BoldReset and a newline. The body (if any) is printed verbatim
 * and followed by std::endl (flush); an empty body still emits one blank line
 * for spacing. Any coloring inside the body must be embedded by the caller —
 * this function does not apply theme colors to it. Spacing before the header
 * is controlled by the caller's @p head string (callers typically prefix it
 * with "\n").
 */
void printSection(const ThemeColors& tc, std::string_view head, const std::string& body) {
    std::cout << tc.head << head << UI::Palette::BoldReset << "\n";
    if (!body.empty()) {
        std::cout << body << std::endl;
    } else {
        std::cout << std::endl;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public help functions
// ---------------------------------------------------------------------------

/**
 * @brief Displays an interactive help guide detailing how to select and
 * filter items within lists.
 *
 * The body is wrapped in a `render` lambda so a resize while waiting at the
 * "↵ to return..." prompt redraws the help text (not just the prompt). The
 * lambda captures all three parameters by reference; they are live references
 * the caller may update between invocations, so each replay must observe the
 * current values. A fresh ThemeColors is constructed on every replay, so a
 * theme change while the screen is open is reflected on the next redraw.
 *
 * Content adjustments:
 *   • @p isUnmount switches the section-2 heading from "Selecting Files" to
 *     "Selecting Mount-Points" and suppresses the '*' (FilenamesOnly) line in
 *     section 3.
 *   • (@p isUnmount || @p isMount) appends "/All" to the "Pending" label in
 *     section 2 and adds the "'00'" alternative to the pending-indices entry.
 *   • @p isAtISOListForHelp includes the 'R' (Refresh ISO list from
 *     FolderPath history) line in section 3.
 *
 * Sections rendered are numbered 1, 2, 3, 4.
 *
 * @param isAtISOListForHelp Live reference; when true, the ISO-list-only
 *                           'R' refresh entry is included.
 * @param isUnmount          Live reference; true when the help is shown from
 *                           an unmount flow (changes heading and hides '*').
 * @param isMount            Live reference; true when the help is shown from
 *                           a mount flow (contributes to the "Pending/All"
 *                           label and '00' alternative).
 */
void helpSelections(bool& isAtISOListForHelp, const bool& isUnmount, const bool& isMount) {
    auto render = [&] {
        const ThemeColors tc;
        SyncOutput sync;
        clearScrollBuffer();

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
                "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'*'" + std::string(UI::Palette::BoldReset) + "                : View FilenamesOnly\n"
                : "") +
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'/'" + std::string(UI::Palette::BoldReset) + "                : Filter (! = NOT, ; = OR, & = AND)\n" +
            (isAtISOListForHelp ?
                "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'R'" + std::string(UI::Palette::BoldReset) + "                : Refresh ISO list from FolderPath history\n"
                : "") +
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'P'|'C' " + std::string(UI::Palette::BoldReset) + "           : Process|Clear pending indices\n" +
            "   " + std::string(UI::Palette::BoldReset) + "• " + std::string(tc.keysBlue) + "'PgDn'|'PgUp'|'g#' " + std::string(UI::Palette::BoldReset) + ": Pagination Next|Previous|GoTo page"
        );
        printSection(tc, "\n4. Tips:",
            "   • Indices correspond only to their generated list\n"
            "   • Indices^ refer to the original unfiltered list\n"
            "   • Filtering spans all pages and is incremental");
    };

    pressEnterToReturnWithRepaint(render);
}

/**
 * @brief Displays a help guide for the settings editor.
 *
 * Renders three numbered sections ("1. Hotkeys", "2. Selecting Edits",
 * "3. Tips") via a `render` lambda. The lambda is replayed by the resize
 * watcher and the press-Enter prompt, with a fresh ThemeColors constructed
 * on each replay so theme changes are picked up on the next redraw.
 *
 * Colors: hotkey values use `keysYellow`, selection examples use `keysPurple`.
 *
 * Blocks until the user presses Enter, then returns.
 */
void helpSettingsEditor() {
    auto render = [&] {
        const ThemeColors tc;
        SyncOutput sync;
        clearScrollBuffer();

        printSection(tc, "\n1. Hotkeys:", std::string(UI::Palette::BoldReset) +
            "   • Exit         : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
            "   • Quick Return : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset) +
            "   • Clear Line   : " + std::string(tc.keysYellow) + "Ctrl+u");
        printSection(tc, "\n2. Selecting Edits (↵):", std::string(UI::Palette::BoldReset) +
            "   • Single/Multiple : " + std::string(tc.keysPurple) + "'1' or '1 5 6'\n" + std::string(UI::Palette::BoldReset) +
            "   • Range/Combine   : " + std::string(tc.keysPurple) + "'1-3' or '1-3 5 7-9'" + std::string(UI::Palette::BoldReset));
        printSection(tc, "\n3. Tips:",
            "   • On reset defaults are auto-saved to disk\n"
            "   • Successful edits are auto-saved to disk\n"
            "   • Falls back to memory if config is unavailable");
    };

    pressEnterToReturnWithRepaint(render);
}

/**
 * @brief Displays a help guide for directory-related prompts (Copy/Move and
 * ISO import/convert flows).
 *
 * The content adapts to @p isCpMv (copy/move vs import/convert), to
 * @p import2ISO (ISO import vs image-cache convert), and, for copy/move, to
 * @p isCopy (tips and overwrite guidance differ between cp and mv). Uses the
 * same `render` lambda + pressEnterToReturnWithRepaint pattern as the other
 * help screens; a fresh ThemeColors is constructed per replay.
 *
 * Content adjustments:
 *   • When @p isCpMv is false, an extra "Cancel Search : Ctrl+c" hotkey line
 *     is inserted, and a Cleanup/Display Commands section is shown instead of
 *     the copy/move tips. In that section, the '!clr' description is
 *     "Clear IsoDatabase" when @p import2ISO is true and "Clear corresponding
 *     ImageCache" otherwise; the display line is "'ls'|'*stats'" when
 *     @p import2ISO is false and "'*stats'" only when it is true.
 *   • When @p isCpMv is true, an Overwrite line is added to section 2, and
 *     the tips section differs between cp and mv (self-copy vs same-device /
 *     multi-destination behavior); @p isCopy selects between them.
 *
 * Rendered section numbering is contiguous in both branches (1,2,3 for
 * copy/move; 1,2,3,4 for import/convert).
 *
 * @param isCpMv     True for copy/move flows; false for import/convert.
 * @param import2ISO True when the flow imports ISOs into the database.
 * @param isCopy     True for copy (cp); ignored unless @p isCpMv is true.
 */
void helpSearches(bool isCpMv, bool import2ISO, bool isCopy) {
    auto render = [&] {
        const ThemeColors tc;
        SyncOutput sync;
        clearScrollBuffer();

        std::string keys =
        "   • Exit          : " + std::string(tc.keysYellow) + "Esc\n" + std::string(UI::Palette::BoldReset) +
        "   • Quick Return  : " + std::string(tc.keysYellow) + "Ctrl+d\n" + std::string(UI::Palette::BoldReset);
        if (!isCpMv) keys += std::string(UI::Palette::BoldReset) + "   • Cancel Search : " + std::string(tc.keysYellow) + "Ctrl+c\n";
        keys += std::string(UI::Palette::BoldReset) + "   • Clear Line    : " + std::string(tc.keysYellow) + "Ctrl+u\n";
        keys += std::string(UI::Palette::BoldReset) + "   • Declutter     : " + std::string(tc.keysYellow) + "Ctrl+l";
        printSection(tc, "\n1. Hotkeys:", keys);

        std::string paths =
            "   • Single/Multiple : '/dir/' or '/dir1/;/dir2/'\n";
        if (isCpMv)
            paths += "   • Overwrite       : Append -o (e.g., '/dir/ -o')";
        printSection(tc, "\n2. Selecting FolderPaths (↵):", paths);

        if (isCpMv) {
            printSection(tc, "\n3. Tips:",
                    isCopy
                        ? "   • 'cp' cannot copy a source file to itself\n"
                          "   • 'cp' filename conflicts are queued sequentially"
                        : "   • 'mv' same-device: instant (metadata update)\n"
                          "   • 'mv' multi-destination: uses cp+rm (slower)\n"
                          "   • 'mv' filename conflicts are queued sequentially");
        } else {
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
    };

    pressEnterToReturnWithRepaint(render);
}

/**
 * @brief Displays a help guide for ISO-to-Device mappings (write2usb).
 *
 * Renders sections "Hotkeys", "Selecting Pairs", and "Tips" via a `render`
 * lambda replayed by the resize watcher and the press-Enter prompt, with a
 * fresh ThemeColors constructed per replay. Takes no parameters; the layout
 * is fixed. Section numbering is contiguous (1, 2, 3).
 */
void helpMappings() {
    auto render = [&] {
        const ThemeColors tc;
        SyncOutput sync;
        clearScrollBuffer();

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
    };

    pressEnterToReturnWithRepaint(render);
}
