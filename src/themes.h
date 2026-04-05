// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THEMES_H
#define THEMES_H

// =========================
// MENU COLOR CONFIGURATION
// =========================

// User-selected menu color (string-based for easy config parsing / CLI input)
inline std::string skin = "white"; 

// ANSI reset sequence (bold reset)
inline std::string reset = "\033[0;1m";

// Returns the ANSI escape code corresponding to the selected menu color.
// Falls back to full reset if the color is unknown (safe default).
inline std::string getskin() {
    return (skin == "green")   ? "\033[1;38;2;0;255;0m"     : // Bright green
           (skin == "cyan")    ? "\033[1;38;2;0;200;200m"   : // Soft cyan
           (skin == "purple")  ? "\033[1;38;2;189;147;249m" : // Dracula Purple
           (skin == "amber")   ? "\033[1;38;2;255;176;0m"   : // CRT Amber
           (skin == "rose")    ? "\033[1;38;2;255;121;198m" : // Hot pink/Rose
           (skin == "white")   ? "\033[1;38;5;250m"         : // Light gray
                                      "\033[0m";                   // Reset
}

// Cached color value (evaluated once at startup)
// NOTE: If skin changes at runtime, this will NOT update automatically.
inline std::string color = getskin();


// =========================
// LIST THEME CONFIGURATION
// =========================

// Active theme name (string allows easy switching via config / CLI)
inline std::string globalTheme = "original";

// Defines a color palette for list rendering.
// Using string_view avoids unnecessary allocations (points to static literals).
struct ListTheme {
    std::string_view primary;
    std::string_view secondary;
    std::string_view accent;
    std::string_view muted;
    std::string_view highlight;
    std::string_view background;
    std::string_view warning;   // Cautionary state (distinct from error/highlight)
};


struct originalColors {
    // ── Resets & base ────────────────────────────────────────────────
    static constexpr std::string_view reset        = "\033[0m";
    static constexpr std::string_view bold         = "\033[1m";
    static constexpr std::string_view boldAlt      = "\033[0;1m";   // same visual, different SGR form
    static constexpr std::string_view dim          = "\033[0;2m";

    // ── Named semantic colors ─────────────────────────────────────────
    static constexpr std::string_view red          = "\033[1;91m";  // errors / failures
    static constexpr std::string_view green        = "\033[1;92m";  // success / ok
    static constexpr std::string_view yellow       = "\033[1;93m";  // warnings / skipped / paths
    static constexpr std::string_view blue         = "\033[1;94m";  // ls / mount / dest paths
    static constexpr std::string_view magenta      = "\033[95;1m";  // missing
    static constexpr std::string_view orange       = "\033[1;38;5;208m";
    static constexpr std::string_view brown        = "\033[1;38;5;94m";
    static constexpr std::string_view darkCyan     = "\033[38;5;37;1m";
    static constexpr std::string_view purple       = "\033[1;35m";  // orig_missingLabel

    // ── Readline-wrapped variants (for prompt strings only) ───────────
    // \001...\002 tell readline not to count escape bytes in line length
    static constexpr std::string_view rl_red       = "\001\033[1;91m\002";
    static constexpr std::string_view rl_yellow    = "\001\033[1;93m\002";
    static constexpr std::string_view rl_reset     = "\001\033[0m\002";
};


// =========================
// PREDEFINED THEMES
// =========================
// Each theme is tuned for readability and visual identity.
// Colors are chosen to maintain contrast and avoid eye strain.

// Format: {Primary, Secondary, Accent, Muted, Highlight, Background, Warning}
inline ListTheme OriginalTheme = {"\033[1;38;2;135;95;0m",   "\033[1;38;2;255;0;0m",     "\033[1;38;2;0;255;0m",     "\033[1;38;2;142;142;142m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;142;142;142m", "\033[1;38;2;255;175;0m"};
inline ListTheme ClassicTheme  = {"\033[1;38;2;135;95;0m",   "\033[1;38;2;255;0;0m",     "\033[1;38;2;255;0;255m",   "\033[1;38;2;148;148;148m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;148;148;148m", "\033[1;38;2;255;135;0m"};
inline ListTheme HighContrast  = {"\033[1;38;2;255;255;255m", "\033[1;38;2;255;255;0m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;188;188;188m", "\033[1;38;2;0;255;0m",     "\033[1;48;2;188;188;188m", "\033[1;38;2;255;135;0m"};
inline ListTheme NeonTheme     = {"\033[1;38;2;255;0;175m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;95;255;0m",    "\033[1;38;2;98;98;98m",    "\033[1;38;2;255;255;0m",   "\033[1;48;2;98;98;98m",    "\033[1;38;2;255;175;0m"};
inline ListTheme OceanTheme    = {"\033[1;38;2;0;135;255m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;95;135;175m",  "\033[1;38;2;135;215;255m", "\033[1;48;2;95;135;175m",  "\033[1;38;2;255;215;0m"};
inline ListTheme SunsetTheme   = {"\033[1;38;2;255;135;0m",   "\033[1;38;2;255;0;0m",     "\033[1;38;2;255;215;0m",   "\033[1;38;2;175;95;0m",    "\033[1;38;2;255;175;255m", "\033[1;48;2;175;95;0m",    "\033[1;38;2;255;95;0m"};
inline ListTheme ForestTheme   = {"\033[1;38;2;95;175;0m",    "\033[1;38;2;95;135;0m",    "\033[1;38;2;175;255;95m",  "\033[1;38;2;95;95;0m",     "\033[1;38;2;175;255;0m",   "\033[1;48;2;95;95;0m",     "\033[1;38;2;215;175;0m"};
inline ListTheme MidnightTheme = {"\033[1;38;2;95;0;255m",    "\033[1;38;2;135;95;255m",  "\033[1;38;2;175;135;255m", "\033[1;38;2;95;95;175m",   "\033[1;38;2;215;175;255m", "\033[1;48;2;95;95;175m",   "\033[1;38;2;255;175;95m"};
inline ListTheme MonoTheme     = {"\033[1;38;2;255;255;255m", "\033[1;38;2;255;255;255m", "\033[1;38;2;238;238;238m", "\033[1;38;2;88;88;88m",    "\033[1;4;38;2;255;255;255m","\033[1;48;2;88;88;88m",    "\033[1;38;2;188;188;188m"};
inline ListTheme RetroTheme    = {"\033[1;38;2;255;175;0m",   "\033[1;38;2;175;95;0m",    "\033[1;38;2;175;135;0m",   "\033[1;38;2;135;95;0m",    "\033[1;38;2;255;215;0m",   "\033[1;48;2;135;95;0m",    "\033[1;38;2;255;95;0m"};
inline ListTheme CrimsonTheme  = {"\033[1;38;2;215;0;0m",     "\033[1;38;2;175;0;0m",     "\033[1;38;2;255;95;95m",   "\033[1;38;2;135;95;95m",   "\033[1;38;2;255;175;175m", "\033[1;48;2;135;95;95m",   "\033[1;38;2;255;175;0m"};
inline ListTheme DraculaTheme  = {"\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;98;114;164m",  "\033[1;38;2;241;250;140m",  "\033[1;48;2;98;114;164m",  "\033[1;38;2;255;184;108m"};


// =========================
// THEME RESOLUTION
// =========================

// Returns a pointer to the active theme based on globalTheme.
// Uses a static map so it's initialized only once (efficient lookup).
inline const ListTheme* getActiveTheme() {
    static const std::unordered_map<std::string, const ListTheme*> themeMap = {
        {"original",      &OriginalTheme},
        {"classic",       &ClassicTheme},
        {"high_contrast", &HighContrast},
        {"neon",          &NeonTheme},
        {"ocean",         &OceanTheme},
        {"sunset",        &SunsetTheme},
        {"forest",        &ForestTheme},
        {"midnight",      &MidnightTheme},
        {"mono",          &MonoTheme},
        {"retro",         &RetroTheme},
        {"crimson",       &CrimsonTheme},
        {"dracula",       &DraculaTheme},
    };

    // Lookup selected theme
    auto it = themeMap.find(globalTheme);

    // Return matched theme or fallback to OriginalTheme (safe default)
    return (it != themeMap.end()) ? it->second : &OriginalTheme;
}

#endif // THEMES_H
