// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file themes.h
 * @brief Terminal UI theme management system for ISO Commander.
 */

#ifndef THEMES_H
#define THEMES_H

// C++ Standard Library Headers
#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace UI {
    namespace Palette {

        static constexpr std::string_view Reset    = "\033[0m";
        static constexpr std::string_view Dim      = "\033[0;38;2;110;110;110m";
        static constexpr std::string_view DimGray  = "\033[1;38;2;110;110;110m";
        static constexpr std::string_view Red      = "\033[1;38;2;235;40;40m";
        static constexpr std::string_view Green    = "\033[1;38;2;90;215;35m";
        static constexpr std::string_view Yellow   = "\033[1;38;2;212;184;0m";
        static constexpr std::string_view Blue     = "\033[1;38;2;0;115;215m";
        static constexpr std::string_view Magenta  = "\033[1;38;2;235;50;245m";
        static constexpr std::string_view Cyan     = "\033[1;38;2;80;190;195m";
        static constexpr std::string_view DarkCyan = "\033[1;38;2;0;160;160m";
        static constexpr std::string_view black    = "\033[1;38;2;40;40;40m";
        static constexpr std::string_view Orange   = "\033[1;38;2;235;120;20m";
        static constexpr std::string_view Purple   = "\033[1;38;2;150;105;210m";
        static constexpr std::string_view Brown    = "\033[1;38;2;150;70;20m";
        static constexpr std::string_view Amber    = "\033[1;38;2;230;158;0m";
        static constexpr std::string_view Rose     = "\033[1;38;2;240;110;185m";
        static constexpr std::string_view White    = "\033[1;38;2;215;215;215m";
        static constexpr std::string_view Gray     = "\033[1;38;2;120;120;120m";
        static constexpr std::string_view BGNavy   = "\033[1;37;48;2;20;20;150m";

        static constexpr std::string_view RL_Blue   = "\001\033[1;38;2;0;115;215m\002";
        static constexpr std::string_view RL_Green  = "\001\033[1;38;2;90;215;35m\002";
        static constexpr std::string_view RL_Red    = "\001\033[1;38;2;235;40;40m\002";
        static constexpr std::string_view RL_Yellow = "\001\033[1;38;2;212;184;0m\002";

        // --- Dynamic Colors ---
        inline std::string defaultText = "white";
        inline std::string BoldReset = "\033[1;38;2;215;215;215m";
        inline std::string RL_BoldAlt = "\001\033[1;38;2;215;215;215m\002";

        /**
        * @brief Updates BoldReset and RL_BoldAlt based on the current 'defaultText' string.
        */
        inline void updateDefaultColors() {
            using namespace UI::Palette;

            static constexpr std::pair<std::string_view, std::string_view> colorMap[] = {
                {"ash",      "\033[1;38;2;178;190;181m"},
                {"charcoal", "\033[1;38;2;54;69;79m"},
                {"gray",     "\033[1;38;2;120;120;120m"},
                {"black", "\033[1;38;2;40;40;40m"},
                {"silver",   "\033[1;38;2;192;192;192m"},
                {"steel",    "\033[1;38;2;112;128;144m"},
                {"white",    "\033[1;38;2;215;215;215m"}
            };

            auto it = std::lower_bound(
                std::begin(colorMap),
                std::end(colorMap),
                defaultText,
                [](const auto& entry, std::string_view val) {
                    return entry.first < val;
                }
            );

            std::string_view code =
                (it != std::end(colorMap) && it->first == defaultText)
                    ? it->second
                    : "\033[1;38;2;215;215;215m";

            BoldReset = std::string(code);

            RL_BoldAlt =
                "\001" + std::string(code.substr(0, code.size() - 1)) + "m\002";
        }
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
        {"amber",    Amber},
        {"cyan",     Cyan},
        {"gray",     Gray},
        {"green",    Green},
        {"black",    black},
        {"purple",   Purple},
        {"rose",     Rose},
        {"white",    White},
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
inline constexpr UI::MainTheme ClassicTheme    =  {  "\033[1;38;2;172;121;0m",   "\033[1;38;2;255;28;28m",   "\033[1;38;2;235;50;245m",  "\033[1;38;2;148;148;148m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;40;40;40m",   "\033[1;38;2;255;135;0m"   };
inline constexpr UI::MainTheme HighContrast    =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;255;255;0m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;188;188;188m", "\033[1;38;2;0;255;0m",     "\033[1;48;2;70;70;70m",   "\033[1;38;2;255;135;0m"   };
inline constexpr UI::MainTheme NeonTheme       =  {  "\033[1;38;2;255;0;175m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;95;255;0m",    "\033[1;38;2;130;130;130m", "\033[1;38;2;255;255;0m",   "\033[1;48;2;40;40;40m",   "\033[1;38;2;255;175;0m"   };
inline constexpr UI::MainTheme OceanTheme      =  {  "\033[1;38;2;0;135;255m",   "\033[1;38;2;0;175;255m",   "\033[1;38;2;0;255;255m",   "\033[1;38;2;95;135;175m",  "\033[1;38;2;135;215;255m", "\033[1;48;2;20;40;60m",   "\033[1;38;2;255;215;0m"   };
inline constexpr UI::MainTheme SunsetTheme     =  {  "\033[1;38;2;255;135;0m",   "\033[1;38;2;255;28;28m",   "\033[1;38;2;255;215;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;255;175;255m", "\033[1;48;2;55;18;0m",    "\033[1;38;2;255;255;0m"   };
inline constexpr UI::MainTheme ForestTheme     =  {  "\033[1;38;2;95;175;0m",    "\033[1;38;2;100;143;0m",   "\033[1;38;2;175;255;95m",  "\033[1;38;2;135;135;0m",   "\033[1;38;2;175;255;0m",   "\033[1;48;2;20;40;0m",    "\033[1;38;2;215;175;0m"   };
inline constexpr UI::MainTheme MidnightTheme   =  {  "\033[1;38;2;219;0;255m",   "\033[1;38;2;143;100;255m", "\033[1;38;2;175;135;255m", "\033[1;38;2;119;119;220m", "\033[1;38;2;215;175;255m", "\033[1;48;2;30;20;60m",   "\033[1;38;2;255;175;95m"  };
inline constexpr UI::MainTheme MonoTheme       =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;200;200;200m", "\033[1;38;2;160;160;160m", "\033[1;38;2;90;90;90m",    "\033[1;38;2;255;255;255m", "\033[1;48;2;40;40;40m",   "\033[1;38;2;130;130;130m" };
inline constexpr UI::MainTheme RetroTheme      =  {  "\033[1;38;2;255;175;0m",   "\033[1;38;2;197;107;0m",   "\033[1;38;2;175;135;0m",   "\033[1;38;2;130;100;60m",  "\033[1;38;2;255;215;0m",   "\033[1;48;2;40;25;0m",    "\033[1;38;2;255;95;0m"    };
inline constexpr UI::MainTheme CrimsonTheme    =  {  "\033[1;38;2;252;37;37m",   "\033[1;38;2;239;64;64m",   "\033[1;38;2;255;140;140m", "\033[1;38;2;167;117;117m", "\033[1;38;2;255;200;200m", "\033[1;48;2;40;5;5m",     "\033[1;38;2;255;175;0m"   };
inline constexpr UI::MainTheme DraculaTheme    =  {  "\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;110;128;185m", "\033[1;38;2;241;250;140m", "\033[1;48;2;28;30;40m",   "\033[1;38;2;255;184;108m" };
inline constexpr UI::MainTheme TokyoNightTheme =  {  "\033[1;38;2;135;175;255m", "\033[1;38;2;224;82;151m",  "\033[1;38;2;158;206;106m", "\033[1;38;2;115;127;183m", "\033[1;38;2;224;175;104m", "\033[1;48;2;26;27;38m",   "\033[1;38;2;255;158;100m" };
inline constexpr UI::MainTheme PaperTheme      =  {  "\033[1;38;2;74;56;0m",     "\033[1;38;2;192;57;43m",   "\033[1;38;2;123;77;176m",  "\033[1;38;2;136;128;112m", "\033[1;38;2;46;125;50m",   "\033[1;48;2;35;28;18m",   "\033[1;38;2;180;83;9m"    };
inline constexpr UI::MainTheme SakuraTheme     =  {  "\033[1;38;2;139;26;74m",   "\033[1;38;2;192;57;43m",   "\033[1;38;2;106;90;205m",  "\033[1;38;2;158;138;149m", "\033[1;38;2;46;125;85m",   "\033[1;48;2;30;20;35m",   "\033[1;38;2;212;121;10m"  };

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
        {"paper",         &PaperTheme},
        {"retro",         &RetroTheme},
        {"sakura",        &SakuraTheme},
        {"sunset",        &SunsetTheme},
        {"tokyo",         &TokyoNightTheme},
    };
    auto it = std::lower_bound(std::begin(themeTable), std::end(themeTable), globalTheme,
        [](const auto& entry, const std::string& val) { return entry.first < val; });
    return (it != std::end(themeTable) && it->first == globalTheme) ? it->second : &OriginalTheme;
}

#endif // THEMES_H
