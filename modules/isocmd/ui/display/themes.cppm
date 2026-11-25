// SPDX-License-Identifier: GPL-3.0-or-later

module;

export module isocmd.ui.display.themes;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ui/display/themes ----
/**
 * @file themes.h
 * @brief Terminal UI theme management system for ISO Commander.
 */



namespace UI {
    namespace Palette {

        inline constexpr std::string_view Reset    = "\033[0m";
        inline constexpr std::string_view Dim      = "\033[0;38;2;110;110;110m";
        inline constexpr std::string_view DimGray  = "\033[1;38;2;110;110;110m";
        inline constexpr std::string_view Red      = "\033[1;38;2;235;40;40m";
        inline constexpr std::string_view Green    = "\033[1;38;2;90;215;35m";
        inline constexpr std::string_view Yellow   = "\033[1;38;2;212;184;0m";
        inline constexpr std::string_view Blue     = "\033[1;38;2;0;115;215m";
        inline constexpr std::string_view Magenta  = "\033[1;38;2;235;50;245m";
        inline constexpr std::string_view Cyan     = "\033[1;38;2;86;182;194m";
        inline constexpr std::string_view DarkCyan = "\033[1;38;2;0;160;160m";
        inline constexpr std::string_view Black    = "\033[1;38;2;62;62;68m";
        inline constexpr std::string_view Orange   = "\033[1;38;2;235;120;20m";
        inline constexpr std::string_view Purple   = "\033[1;38;2;170;130;230m";
        inline constexpr std::string_view Brown    = "\033[1;38;2;150;70;20m";
        inline constexpr std::string_view Amber    = "\033[1;38;2;232;166;64m";
        inline constexpr std::string_view Rose     = "\033[1;38;2;232;118;170m";
        inline constexpr std::string_view White    = "\033[1;38;2;222;222;222m";
        inline constexpr std::string_view Gray     = "\033[1;38;2;140;140;148m";
        inline constexpr std::string_view BGNavy   = "\033[1;37;48;2;20;20;150m";
        inline constexpr std::string_view idxGreen    = "\033[1;38;2;120;220;160m";

        inline constexpr std::string_view RL_Blue   = "\001\033[1;38;2;0;115;215m\002";
        inline constexpr std::string_view RL_Green  = "\001\033[1;38;2;90;215;35m\002";
        inline constexpr std::string_view RL_Red    = "\001\033[1;38;2;235;40;40m\002";
        inline constexpr std::string_view RL_Yellow = "\001\033[1;38;2;212;184;0m\002";
        inline constexpr std::string_view RL_idxGreen = "\001\033[1;38;2;120;220;160m\002";


        // --- Dynamic Colors ---
        inline std::string defaultColor = "white";
        inline std::string BoldReset = "\033[1;38;2;222;222;222m";
        inline std::string RL_BoldAlt = "\001\033[1;38;2;222;222;222m\002";

        /**
        * @brief Updates BoldReset and RL_BoldAlt based on the current 'defaultColor' string.
        */
        void updateDefaultColors();
    }


	struct MainTheme { std::string_view primary, secondary, accent, muted, highlight, background, warning, indexDouble;
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

	struct ReadlineColors {
    const char* label, *hint, *dir, *file, *reset;
    const char* warning, *accent;
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

std::string_view getskin();

// --- THEME DEFINITIONS ---

// Dummy holder for originalTheme, since the original theme uses a complex combination from original colors above
inline constexpr UI::MainTheme OriginalTheme  = {"","","","","","","",""};
                                                                            // primary, secondary, accent, muted, highlight, background, warning, indexDouble
inline constexpr UI::MainTheme ClassicTheme    =  {  "\033[1;38;2;229;169;61m",  "\033[1;38;2;224;108;117m", "\033[1;38;2;198;120;221m", "\033[1;38;2;140;140;148m", "\033[1;38;2;240;220;120m", "\033[48;2;32;30;28m",   "\033[1;38;2;255;152;72m",  "\033[1;38;2;86;182;194m"  };
inline constexpr UI::MainTheme HighContrast    =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;255;238;88m",  "\033[1;38;2;90;230;240m",  "\033[1;38;2;190;190;190m", "\033[1;38;2;105;240;120m", "\033[48;2;12;12;12m",   "\033[1;38;2;255;160;70m",  "\033[1;38;2;120;150;255m" };
inline constexpr UI::MainTheme NeonTheme       =  {  "\033[1;38;2;255;95;190m",  "\033[1;38;2;60;190;255m",  "\033[1;38;2;140;255;80m",  "\033[1;38;2;130;130;150m", "\033[1;38;2;255;235;80m",  "\033[48;2;22;18;34m",   "\033[1;38;2;255;170;60m",  "\033[1;38;2;170;120;255m" };
inline constexpr UI::MainTheme OceanTheme      =  {  "\033[1;38;2;90;165;245m",  "\033[1;38;2;80;205;215m",  "\033[1;38;2;130;225;190m", "\033[1;38;2;110;140;175m", "\033[1;38;2;160;225;245m", "\033[48;2;16;30;46m",   "\033[1;38;2;255;205;90m",  "\033[1;38;2;255;130;90m"  };
inline constexpr UI::MainTheme SunsetTheme     =  {  "\033[1;38;2;245;150;70m",  "\033[1;38;2;232;92;92m",   "\033[1;38;2;222;115;165m", "\033[1;38;2;178;128;102m", "\033[1;38;2;255;196;170m", "\033[48;2;38;22;22m",   "\033[1;38;2;255;226;100m", "\033[1;38;2;80;190;200m"  };
inline constexpr UI::MainTheme ForestTheme     =  {  "\033[1;38;2;125;190;95m",  "\033[1;38;2;90;185;160m",  "\033[1;38;2;190;230;120m", "\033[1;38;2;130;150;100m", "\033[1;38;2;240;225;140m", "\033[48;2;18;30;20m",   "\033[1;38;2;235;170;70m",  "\033[1;38;2;170;135;235m" };
inline constexpr UI::MainTheme MidnightTheme   =  {  "\033[1;38;2;190;120;255m", "\033[1;38;2;130;140;255m", "\033[1;38;2;240;140;220m", "\033[1;38;2;125;125;190m", "\033[1;38;2;220;205;255m", "\033[48;2;24;20;42m",   "\033[1;38;2;255;175;110m", "\033[1;38;2;110;200;150m" };
inline constexpr UI::MainTheme MonoTheme       =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;180;180;180m", "\033[1;38;2;200;205;210m", "\033[1;38;2;90;90;90m",    "\033[1;38;2;210;210;210m", "\033[48;2;40;40;40m",   "\033[1;38;2;170;170;170m", "\033[1;38;2;130;130;130m" };
inline constexpr UI::MainTheme RetroTheme      =  {  "\033[1;38;2;240;178;60m",  "\033[1;38;2;214;120;60m",  "\033[1;38;2;190;185;100m", "\033[1;38;2;140;115;85m",  "\033[1;38;2;255;214;120m", "\033[48;2;34;24;14m",   "\033[1;38;2;240;100;60m",  "\033[1;38;2;90;160;230m"  };
inline constexpr UI::MainTheme CrimsonTheme    =  {  "\033[1;38;2;232;84;84m",   "\033[1;38;2;240;140;110m", "\033[1;38;2;230;120;170m", "\033[1;38;2;165;120;120m", "\033[1;38;2;255;205;200m", "\033[48;2;34;14;16m",   "\033[1;38;2;255;185;70m",  "\033[1;38;2;70;190;190m"  };
inline constexpr UI::MainTheme DraculaTheme    =  {  "\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;110;128;185m", "\033[1;38;2;241;250;140m", "\033[48;2;28;30;40m",   "\033[1;38;2;255;184;108m", "\033[1;38;2;0;134;57m"    };
inline constexpr UI::MainTheme TokyoNightTheme =  {  "\033[1;38;2;135;175;255m", "\033[1;38;2;224;82;151m",  "\033[1;38;2;158;206;106m", "\033[1;38;2;115;127;183m", "\033[1;38;2;224;175;104m", "\033[48;2;26;27;38m",   "\033[1;38;2;255;158;100m", "\033[1;38;2;31;173;104m"  };
inline constexpr UI::MainTheme PaperTheme      =  {  "\033[1;38;2;235;225;200m", "\033[1;38;2;230;110;95m",  "\033[1;38;2;180;145;225m", "\033[1;38;2;150;140;120m", "\033[1;38;2;130;200;120m", "\033[48;2;35;28;18m",   "\033[1;38;2;235;160;70m",  "\033[1;38;2;90;200;210m"  };
inline constexpr UI::MainTheme SakuraTheme     =  {  "\033[1;38;2;240;150;190m", "\033[1;38;2;235;120;105m", "\033[1;38;2;160;145;235m", "\033[1;38;2;160;140;152m", "\033[1;38;2;110;200;150m", "\033[48;2;30;20;35m",   "\033[1;38;2;235;165;70m",  "\033[1;38;2;90;200;220m"  };

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
const UI::MainTheme* getActiveTheme();

} // extern "C++"
