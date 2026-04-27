// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file themes.h
 * @brief Terminal UI theme management system for ISO Commander.
 */

#ifndef THEMES_H
#define THEMES_H

#include "headers.h"

namespace UI {

    /**
     * @brief Raw 24-bit ANSI Escape Sequences.
     */
    namespace Palette {
        static constexpr std::string_view Reset        = "\033[0m";
        static constexpr std::string_view BoldReset    = "\033[0;1;38;2;215;215;215m";
        static constexpr std::string_view Dim          = "\033[0;38;2;130;130;130m";
        static constexpr std::string_view DimGray      = "\033[1;38;2;100;100;100m";

        static constexpr std::string_view Red          = "\033[1;38;2;255;40;40m";
        static constexpr std::string_view Green        = "\033[1;38;2;0;255;50m"; 
        static constexpr std::string_view Yellow       = "\033[1;38;2;255;255;0m";
        static constexpr std::string_view Blue         = "\033[1;38;2;0;125;255m";
        static constexpr std::string_view Magenta      = "\033[1;38;2;255;0;255m"; 
        static constexpr std::string_view Cyan         = "\033[1;38;2;103;233;235m";
        static constexpr std::string_view DarkCyan     = "\033[1;38;2;0;160;160m";
        static constexpr std::string_view Orange       = "\033[1;38;2;255;120;0m";
        static constexpr std::string_view Purple       = "\033[1;38;2;140;70;200m";
        static constexpr std::string_view Brown        = "\033[1;38;2;165;75;25m";
        static constexpr std::string_view Amber        = "\033[1;38;2;255;176;0m";
        static constexpr std::string_view Rose         = "\033[1;38;2;255;121;198m";
        static constexpr std::string_view White        = "\033[1;38;2;215;215;215m";
        static constexpr std::string_view BGNavy       = "\033[1;48;2;0;0;175m";
        
        // Readline-wrapped variants
        static constexpr std::string_view RL_Reset     = "\001\033[0;1;38;2;215;215;215m\002";
        static constexpr std::string_view RL_Blue      = "\001\033[1;38;2;0;125;255m\002";
        static constexpr std::string_view RL_Green     = "\001\033[1;38;2;0;255;50m\002";
        static constexpr std::string_view RL_Red       = "\001\033[1;38;2;255;40;40m\002";
        static constexpr std::string_view RL_Yellow    = "\001\033[1;38;2;255;255;0m\002";
        static constexpr std::string_view RL_BoldAlt   = "\001\033[0;1;38;2;215;215;215m\002";
    }

	struct MainTheme { std::string_view primary, secondary, accent, muted, highlight, background, warning; 
	};

	struct PrintListTheme { std::string_view accent, head, num, iso, img, mnt, square, indexA, indexB, dir, bracketBg, procText; 
	};

	struct WriteTheme { std::string errLabel, errPath, warnLabel, infoLabel, bold, headerCol, indexCol, pathCol, fileCol, sizeCol, warnCol, 
		colorSuccess, colorFailure, colorWarning, colorStatus, speedCol, deviceCol, rl_labelCol, rl_primaryCol, rl_highlightCol, rl_errorCol, rl_resetCol; 
	};

	struct CpMvRmColors { std::string_view arrow, dir, iso, error_label, error_path, success_label, success_path, dest_path, abort, prompt_green, prompt_blue; 
	};

	struct ConversionThemeStrings { std::string_view errLabel, errPath, missingLabel, okLabel, okPath, skipLabel, skipPath; 
	};

	struct VerboseAndDatabaseTheme { std::string red, yellow, green, purple, magenta, blue, orange, bold, reset; 
	};

	struct ReadlineAndPromptTheme { std::string primary, filter, highlight, reset, iso, accent; 
	};

	struct SemanticUIColors { std::string_view label, accent, warning, error, reset, path, highlight, data, str; 
	};

	struct ReadlineColors { const char* label, *hint, *dir, *file, *reset; 
	};

	struct ProgressBarColors { const char* success, *failure, *warning, *status, *reset; 
	};
}

using MainTheme      		  = UI::MainTheme;
using PrintListTheme 		  = UI::PrintListTheme;
using WriteTheme     		  = UI::WriteTheme;
using VerboseAndDatabaseTheme = UI::VerboseAndDatabaseTheme;
using ReadlineAndPromptTheme  = UI::ReadlineAndPromptTheme;
using CpMvRmColors            = UI::CpMvRmColors;
using ConversionThemeStrings  = UI::ConversionThemeStrings;
using SemanticUIColors        = UI::SemanticUIColors;
using ReadlineColors          = UI::ReadlineColors;
using ProgressBarColors       = UI::ProgressBarColors;

// --- GLOBAL STATE ---

inline std::string_view skin = "white"; 
inline std::string globalTheme = "original";
inline std::string_view color;

inline std::string_view getskin() {
    using namespace UI::Palette;
    static constexpr std::pair<std::string_view, std::string_view> skinMap[] = {
        {"amber",  Amber},
        {"cyan",   Cyan},
        {"green",  Green},
        {"purple", Purple},
        {"rose",   Rose},
        {"white",  White},
    };
    auto it = std::lower_bound(std::begin(skinMap), std::end(skinMap), skin,
        [](const auto& entry, std::string_view val) { return entry.first < val; });
    color = (it != std::end(skinMap) && it->first == skin) ? it->second : Reset;
    return color;
}

// --- THEME DEFINITIONS ---

// Dummy holder for originalTheme, since the original theme uses a complex combination from original colors above
inline constexpr UI::MainTheme OriginalTheme  = {"","","","","","",""};
																													// primary, secondary, accent, muted, highlight, background, warning
inline constexpr UI::MainTheme ClassicTheme    =  {  "\033[1;38;2;172;121;0m",   "\033[1;38;2;255;28;28m",   "\033[1;38;2;255;0;255m",   "\033[1;38;2;148;148;148m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;60;60;60m",   "\033[1;38;2;255;135;0m"   };
inline constexpr UI::MainTheme HighContrast    =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;255;255;0m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;188;188;188m", "\033[1;38;2;0;255;0m",     "\033[1;48;2;70;70;70m",   "\033[1;38;2;255;135;0m"   };
inline constexpr UI::MainTheme NeonTheme       =  {  "\033[1;38;2;255;0;175m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;95;255;0m",    "\033[1;38;2;130;130;130m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;40;40;40m",   "\033[1;38;2;255;175;0m"   };
inline constexpr UI::MainTheme OceanTheme      =  {  "\033[1;38;2;0;135;255m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;95;135;175m",  "\033[1;38;2;135;215;255m", "\033[1;48;2;20;40;60m",   "\033[1;38;2;255;215;0m"   };
inline constexpr UI::MainTheme SunsetTheme     =  {  "\033[1;38;2;255;135;0m",   "\033[1;38;2;255;28;28m",   "\033[1;38;2;255;215;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;255;175;255m", "\033[1;48;2;80;30;0m",    "\033[1;38;2;255;255;0m"   };
inline constexpr UI::MainTheme ForestTheme     =  {  "\033[1;38;2;95;175;0m",    "\033[1;38;2;100;143;0m",   "\033[1;38;2;175;255;95m",  "\033[1;38;2;135;135;0m",   "\033[1;38;2;175;255;0m",   "\033[1;48;2;20;40;0m",    "\033[1;38;2;215;175;0m"   };
inline constexpr UI::MainTheme MidnightTheme   =  {  "\033[1;38;2;219;0;255m",   "\033[1;38;2;143;100;255m", "\033[1;38;2;175;135;255m", "\033[1;38;2;119;119;220m", "\033[1;38;2;215;175;255m", "\033[1;48;2;30;20;60m",   "\033[1;38;2;255;175;95m"  };
inline constexpr UI::MainTheme MonoTheme       =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;200;200;200m", "\033[1;38;2;160;160;160m", "\033[1;38;2;90;90;90m",    "\033[1;38;2;255;255;255m", "\033[1;48;2;50;50;50m",   "\033[1;38;2;130;130;130m" };
inline constexpr UI::MainTheme RetroTheme      =  {  "\033[1;38;2;255;175;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;175;135;0m",   "\033[1;38;2;130;100;60m",  "\033[1;38;2;255;215;0m",   "\033[1;48;2;60;40;0m",    "\033[1;38;2;255;95;0m"    };
inline constexpr UI::MainTheme CrimsonTheme    =  {  "\033[1;38;2;252;37;37m",   "\033[1;38;2;239;64;64m",   "\033[1;38;2;255;140;140m", "\033[1;38;2;167;117;117m", "\033[1;38;2;255;200;200m", "\033[1;48;2;60;10;10m",   "\033[1;38;2;255;175;0m"   };
inline constexpr UI::MainTheme DraculaTheme    =  {  "\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;110;128;185m", "\033[1;38;2;241;250;140m", "\033[1;48;2;40;42;54m",   "\033[1;38;2;255;184;108m" };
inline constexpr UI::MainTheme TokyoNightTheme =  {  "\033[1;38;2;135;175;255m",  "\033[1;38;2;224;82;151m", "\033[1;38;2;158;206;106m", "\033[1;38;2;115;127;183m", "\033[1;38;2;224;175;104m", "\033[1;48;2;26;27;38m",   "\033[1;38;2;255;158;100m" };

// --- FUNCTION DECLARATIONS ---

WriteTheme getWriteTheme();
PrintListTheme getListColors();
CpMvRmColors getCpMvRmColors();
ConversionThemeStrings getConversionThemeStrings();
ReadlineColors resolveReadlineTheme();
ProgressBarColors resolveProgressTheme();

SemanticUIColors resolveOptionsTheme();
SemanticUIColors resolveVerboseTheme();
SemanticUIColors resolveDatabaseTheme();

VerboseAndDatabaseTheme getVerboseTheme();
VerboseAndDatabaseTheme getDatabaseTheme();

ReadlineAndPromptTheme getPromptTheme();
ReadlineAndPromptTheme getFilterTheme(const std::string& operationColor, bool includeIso);

/**
 * @brief Resolves the current theme structure based on the globalTheme string.
 */
inline const UI::MainTheme* getActiveTheme() {
    static constexpr std::pair<std::string_view, const UI::MainTheme*> themeTable[] = {
        {"classic",       &ClassicTheme},
        {"crimson",       &CrimsonTheme},
        {"dracula",       &DraculaTheme},
        {"forest",        &ForestTheme},
        {"high_contrast", &HighContrast},
        {"midnight",      &MidnightTheme},
        {"mono",          &MonoTheme},
        {"neon",          &NeonTheme},
        {"ocean",         &OceanTheme},
        {"original",      &OriginalTheme},
        {"retro",         &RetroTheme},
        {"sunset",        &SunsetTheme},
        {"tokyo",         &TokyoNightTheme},
    };
    auto it = std::lower_bound(std::begin(themeTable), std::end(themeTable), globalTheme,
        [](const auto& entry, const std::string& val) { return entry.first < val; });
    return (it != std::end(themeTable) && it->first == globalTheme) ? it->second : &OriginalTheme;
}

#endif // THEMES_H
