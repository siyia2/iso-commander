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
inline std::string globalListTheme = "original";

struct ListTheme {
    std::string_view primary;
    std::string_view secondary;
    std::string_view accent;
    std::string_view muted;
    std::string_view highlight;
};

inline ListTheme OriginalTheme = {"\033[1;38;5;94m", "\033[31;1m", "\033[32;1m", "\033[38;5;245m", "\033[1;93m"};
inline ListTheme ClassicTheme  = {"\033[1;38;5;94m", "\033[31;1m", "\033[95;1m", "\033[38;5;246m", "\033[1;93m"};
inline ListTheme HighContrast  = {"\033[1;37m", "\033[1;33m", "\033[1;36m", "\033[38;5;250m", "\033[1;32m"};
inline ListTheme NeonTheme     = {"\033[38;5;199m", "\033[38;5;39m", "\033[38;5;82m", "\033[38;5;248m", "\033[38;5;226m"};

#endif // THEMES_H
