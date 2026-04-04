// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THEMES_H
#define THEMES_H

// Menu Themes
inline std::string menuColor = "cyan"; 
inline std::string reset = "\033[0;1m";


inline std::string getMenuColor() {
    return (menuColor == "green")  ? "\033[1;32m"              : 
           (menuColor == "cyan")   ? "\033[1;38;2;0;200;200m"  : 
           (menuColor == "white")  ? "\033[1;38;5;250m"        : 
                                           "\033[0m";                  // Reset/Default
}

inline std::string color = getMenuColor();

// List Themes
inline std::string globalListTheme = "forest";

struct ListTheme {
    std::string_view primary;
    std::string_view secondary;
    std::string_view accent;
    std::string_view muted;
    std::string_view highlight;
};

inline ListTheme OriginalTheme = {"\033[1;38;5;94m",  "\033[1;31m",        "\033[1;32m",        "\033[38;5;245m",  "\033[1;93m"};
inline ListTheme ClassicTheme  = {"\033[1;38;5;94m",  "\033[1;31m",        "\033[1;95m",        "\033[38;5;246m",  "\033[1;93m"};
inline ListTheme HighContrast  = {"\033[1;37m",        "\033[1;33m",        "\033[1;36m",        "\033[38;5;250m",  "\033[1;32m"};
inline ListTheme NeonTheme     = {"\033[1;38;5;199m",  "\033[1;38;5;39m",   "\033[1;38;5;82m",   "\033[38;5;241m",  "\033[38;5;226m"};
inline ListTheme OceanTheme    = {"\033[1;38;5;33m",   "\033[1;38;5;39m",   "\033[1;38;5;51m",   "\033[38;5;67m",   "\033[1;38;5;117m"};
inline ListTheme SunsetTheme   = {"\033[1;38;5;208m",  "\033[1;38;5;196m",  "\033[1;38;5;220m",  "\033[38;5;130m",  "\033[1;38;5;213m"};
inline ListTheme ForestTheme   = {"\033[1;38;5;70m",   "\033[1;38;5;64m",   "\033[1;38;5;149m",  "\033[38;5;58m",   "\033[1;38;5;154m"};
inline ListTheme MidnightTheme = {"\033[1;38;5;57m",   "\033[1;38;5;99m",   "\033[1;38;5;141m",  "\033[38;5;61m",   "\033[1;38;5;183m"};
inline ListTheme MonoTheme     = {"\033[1;37m",         "\033[1;37m",        "\033[1;97m",        "\033[38;5;240m",  "\033[4;37m"};
inline ListTheme RetroTheme    = {"\033[1;38;5;214m",  "\033[1;38;5;130m",  "\033[1;38;5;136m",  "\033[38;5;94m",   "\033[1;38;5;220m"};
inline ListTheme CrimsonTheme  = {"\033[1;38;5;160m",  "\033[1;38;5;124m",  "\033[1;38;5;203m",  "\033[38;5;95m",   "\033[1;38;5;217m"};
inline ListTheme DraculaTheme  = {"\033[1;38;5;141m",  "\033[1;38;5;212m",  "\033[1;38;5;84m",   "\033[38;5;61m",   "\033[38;5;228m"};

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
    auto it = themeMap.find(globalListTheme);
    return (it != themeMap.end()) ? it->second : &OriginalTheme;
}

#endif // THEMES_H
