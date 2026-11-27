// SPDX-License-Identifier: GPL-3.0-or-later

module;

export module isocmd.ui.display.themes;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @file themes.cppm
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

		inline constexpr std::string_view PromptBlue   = "\001\033[1;38;2;0;115;215m\002";
		inline constexpr std::string_view PromptGreen  = "\001\033[1;38;2;90;215;35m\002";
		inline constexpr std::string_view PromptRed    = "\001\033[1;38;2;235;40;40m\002";
		inline constexpr std::string_view PromptYellow = "\001\033[1;38;2;212;184;0m\002";
		inline constexpr std::string_view PromptIndexGreen = "\001\033[1;38;2;120;220;160m\002";

		// --- Dynamic Colors ---
		extern std::string defaultColor;
		extern std::string BoldReset;
		extern std::string PromptBoldAlt;

		/// Updates BoldReset and PromptBoldAlt based on the current 'defaultColor' string.
		void updateDefaultColors();
	}

	struct MainTheme { std::string_view primary, secondary, accent, muted, highlight, background, warning, indexDouble;
	};

	struct PrintListTheme { std::string_view accent, head, num, iso, img, mnt, square, indexA, indexB, dir, bracketBg, procText;
	};

	struct WriteTheme { std::string errLabel, errPath, warnLabel, infoLabel, bold, headerCol, indexCol, pathCol, fileCol, sizeCol, warnCol,
		colorSuccess, colorFailure, colorWarning, colorStatus, speedCol, deviceCol, editorLabelCol, editorPrimaryCol, editorHighlightCol, editorErrorCol, editorResetCol;
	};

	struct CpMvRmColors { std::string_view arrow, dir, iso, error_label, error_path, success_label, success_path, dest_path, abort, prompt_green, prompt_blue;
	};

	struct ConversionThemeStrings { std::string_view errLabel, errPath, missingLabel, okLabel, okPath, skipLabel, skipPath;
	};

	struct VerboseAndDatabaseTheme { std::string red, yellow, green, purple, magenta, blue, orange, bold, reset;
	};

	struct LineEditorAndPromptTheme { std::string primary, filter, highlight, reset, iso, accent;
	};

	struct SemanticUIColors { std::string_view label, accent, warning, error, reset, path, highlight, data, str;
	};

	struct LineEditorColors {
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
using LineEditorAndPromptTheme  = UI::LineEditorAndPromptTheme;
using CpMvRmColors            = UI::CpMvRmColors;
using ConversionThemeStrings  = UI::ConversionThemeStrings;
using SemanticUIColors        = UI::SemanticUIColors;
using LineEditorColors          = UI::LineEditorColors;
using ProgressBarColors       = UI::ProgressBarColors;

// --- GLOBAL STATE ---

inline std::string_view skin = "white";
extern std::string globalTheme;
inline std::string_view color;

std::string_view getskin();

// --- FUNCTION DECLARATIONS ---

/// Returns the fixed IDX operation color, independent of the active theme.
std::string_view getOperationColor(std::string_view operation);

WriteTheme getWriteTheme();
PrintListTheme getListColors();
CpMvRmColors getCpMvRmColors();
ConversionThemeStrings getConversionThemeStrings();
LineEditorColors resolveLineEditorTheme();
ProgressBarColors resolveProgressTheme();

SemanticUIColors resolveOptionsTheme();
SemanticUIColors resolveVerboseTheme();
SemanticUIColors resolveDatabaseTheme();

VerboseAndDatabaseTheme getVerboseTheme();
VerboseAndDatabaseTheme getDatabaseTheme();

LineEditorAndPromptTheme getPromptTheme();
LineEditorAndPromptTheme getFilterTheme(const std::string& operationColor, bool includeIso);

/// Resolves the current theme structure based on the globalTheme string.
const UI::MainTheme* getActiveTheme();

} // extern "C++"
