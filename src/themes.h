// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THEMES_H
#define THEMES_H

/**
 * @brief Current active skin name.
 */
inline std::string skin = "white"; 

/**
 * @brief Global reset sequence for skin terminal formatting.
 */
inline std::string reset = "\033[0;1;38;2;215;215;215m";

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Maps user-defined skin names to high-fidelity ANSI color sequences.
 * @return A string containing the ANSI escape code for the selected skin.
 */
inline std::string getskin() {
    if (skin == "green")   return "\033[1;38;2;0;255;0m";
    if (skin == "cyan")    return "\033[1;38;2;0;200;200m";
    if (skin == "purple")  return "\033[1;38;2;189;147;249m";
    if (skin == "amber")   return "\033[1;38;2;255;176;0m";
    if (skin == "rose")    return "\033[1;38;2;255;121;198m";
    if (skin == "white")   return "\033[1;38;2;215;215;215m"; // Brighter Silver/Grey RGB
    return "\033[0m";
}

/**
 * @brief The resolved ANSI color string based on the initial skin value.
 * @note This is initialized once at startup.
 */
inline std::string color = getskin();

/**
 * @brief The globally selected theme identifier.
 */
inline std::string globalTheme = "original";

/**
 * @brief Defines a complete color palette for UI list rendering.
 */
struct ListTheme {
    std::string_view primary;
    std::string_view secondary;
    std::string_view accent;
    std::string_view muted;
    std::string_view highlight;
    std::string_view background;
    std::string_view warning;
};

/**
 * @brief Container for modern 24-bit RGB terminal color specifications (Bold).
 * @details Uses True Color escape sequences with the bold attribute included.
 * Format: \033[1;38;2;R;G;Bm
 */
/**
 * @brief Container for modern 24-bit RGB terminal color specifications (Bold).
 * @details Uses True Color escape sequences with the bold attribute included.
 * Format: \033[1;38;2;R;G;Bm
 */
struct originalColors {
    static constexpr std::string_view resetPlain = "\033[0m";
	static constexpr std::string_view boldAlt 	 = "\033[0;1;38;2;215;215;215m";
    static constexpr std::string_view dim        = "\033[0;38;2;130;130;130m";

    static constexpr std::string_view red        = "\033[1;38;2;255;40;40m";
    
    static constexpr std::string_view green      = "\033[1;38;2;0;255;50m"; 
    
    static constexpr std::string_view yellow     = "\033[1;38;2;255;255;0m";
    static constexpr std::string_view blue       = "\033[1;38;2;0;125;255m";
    
    static constexpr std::string_view magenta    = "\033[1;38;2;255;0;255m"; 

    static constexpr std::string_view cyan       = "\033[1;38;2;103;233;235m";
    static constexpr std::string_view orange     = "\033[1;38;2;255;120;0m";
    static constexpr std::string_view brown      = "\033[1;38;2;165;75;25m";
    static constexpr std::string_view darkCyan   = "\033[1;38;2;0;160;160m";
    static constexpr std::string_view purple     = "\033[1;38;2;140;70;200m";
    static constexpr std::string_view dimGray    = "\033[1;38;2;100;100;100m";
    
    static constexpr std::string_view bgNavy     = "\033[1;48;2;0;0;175m";

    // Readline-wrapped variants
    static constexpr std::string_view rl_blue    = "\001\033[1;38;2;0;125;255m\002";
    static constexpr std::string_view rl_green   = "\001\033[1;38;2;0;255;50m\002";
    static constexpr std::string_view rl_orange  = "\001\033[1;38;2;255;120;0m\002";
    static constexpr std::string_view rl_yellow  = "\001\033[1;38;2;255;255;0m\002";
    static constexpr std::string_view rl_cyan    = "\001\033[1;38;2;103;233;235m\002";
    static constexpr std::string_view rl_purple     = "\001\033[1;38;2;140;70;200m\002";
    static constexpr std::string_view rl_boldAlt = "\001\033[0;1;38;2;215;215;215m\002";
    // Readline-wrapped RGB Bold White (215, 215, 215)
	static constexpr std::string_view rl_reset = "\001\033[0;1;38;2;215;215;215m\002";
};

// --- Theme Instances ---

// Dummy holder for originalTheme, since the original theme uses a complex combination from original colors above
inline ListTheme OriginalTheme = {"","","","","","",""};
																				// primary, secondary, accent, muted, highlight, background, warning
inline ListTheme ClassicTheme     = {"\033[1;38;2;172;121;0m",  "\033[1;38;2;255;28;28m",   "\033[1;38;2;255;0;255m",   "\033[1;38;2;148;148;148m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;60;60;60m",   "\033[1;38;2;255;135;0m"};
inline ListTheme HighContrast    = {"\033[1;38;2;255;255;255m", "\033[1;38;2;255;255;0m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;188;188;188m", "\033[1;38;2;0;255;0m",     "\033[1;48;2;70;70;70m",   "\033[1;38;2;255;135;0m"};
inline ListTheme NeonTheme       = {"\033[1;38;2;255;0;175m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;95;255;0m",    "\033[1;38;2;130;130;130m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;40;40;40m",   "\033[1;38;2;255;175;0m"};
inline ListTheme OceanTheme      = {"\033[1;38;2;0;135;255m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;95;135;175m",  "\033[1;38;2;135;215;255m", "\033[1;48;2;20;40;60m",   "\033[1;38;2;255;215;0m"};
inline ListTheme SunsetTheme     = {"\033[1;38;2;255;135;0m",   "\033[1;38;2;255;28;28m",   "\033[1;38;2;255;215;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;255;175;255m", "\033[1;48;2;80;30;0m",    "\033[1;38;2;255;255;0m"};
inline ListTheme ForestTheme     = {"\033[1;38;2;95;175;0m",    "\033[1;38;2;100;143;0m",   "\033[1;38;2;175;255;95m",  "\033[1;38;2;135;135;0m",   "\033[1;38;2;175;255;0m",   "\033[1;48;2;20;40;0m",    "\033[1;38;2;215;175;0m"};
inline ListTheme MidnightTheme   = {"\033[1;38;2;219;0;255m",   "\033[1;38;2;143;100;255m", "\033[1;38;2;175;135;255m", "\033[1;38;2;119;119;220m", "\033[1;38;2;215;175;255m", "\033[1;48;2;30;20;60m",   "\033[1;38;2;255;175;95m"};
inline ListTheme MonoTheme       = {"\033[1;38;2;255;255;255m", "\033[1;38;2;200;200;200m", "\033[1;38;2;160;160;160m", "\033[1;38;2;90;90;90m",   "\033[1;38;2;255;255;255m", "\033[1;48;2;50;50;50m",   "\033[1;38;2;130;130;130m"};
inline ListTheme RetroTheme      = {"\033[1;38;2;255;175;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;175;135;0m",   "\033[1;38;2;130;100;60m",  "\033[1;38;2;255;215;0m",   "\033[1;48;2;60;40;0m",    "\033[1;38;2;255;95;0m"};
inline ListTheme CrimsonTheme    = {"\033[1;38;2;252;37;37m",   "\033[1;38;2;239;64;64m",   "\033[1;38;2;255;140;140m", "\033[1;38;2;167;117;117m", "\033[1;38;2;255;200;200m", "\033[1;48;2;100;20;20m",  "\033[1;38;2;255;175;0m"};
inline ListTheme DraculaTheme    = {"\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;110;128;185m", "\033[1;38;2;241;250;140m", "\033[1;48;2;40;42;54m",   "\033[1;38;2;255;184;108m"};
inline ListTheme TokyoNightTheme = {"\033[1;38;2;122;162;247m", "\033[1;38;2;187;154;247m", "\033[1;38;2;158;206;106m", "\033[1;38;2;115;127;183m", "\033[1;38;2;224;175;104m", "\033[1;48;2;26;27;38m",   "\033[1;38;2;255;158;100m"};
/**
 * @brief Resolves the current theme structure based on the global system settings.
 * @return A pointer to a static ListTheme instance. Defaults to OriginalTheme if unknown.
 */
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
        {"tokyo",         &TokyoNightTheme},
    };

    auto it = themeMap.find(globalTheme);
    return (it != themeMap.end()) ? it->second : &OriginalTheme;
}

#endif // THEMES_H
