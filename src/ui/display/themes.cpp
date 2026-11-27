// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.ui.display.themes;

namespace UI::Palette {
	std::string defaultColor = "white";
	std::string BoldReset = "\033[1;38;2;222;222;222m";
	std::string PromptBoldAlt = "\001\033[1;38;2;222;222;222m\002";
}

std::string globalTheme = "original";

namespace {
	// --- THEME DEFINITIONS ---

	// Dummy holder for originalTheme, since the original theme uses a complex combination from original colors above
	constexpr UI::MainTheme OriginalTheme  = {"","","","","","","",""};
	// primary, secondary, accent, muted, highlight, background, warning, indexDouble
	constexpr UI::MainTheme ClassicTheme    =  {  "\033[1;38;2;229;169;61m",  "\033[1;38;2;224;108;117m", "\033[1;38;2;198;120;221m", "\033[1;38;2;140;140;148m", "\033[1;38;2;240;220;120m", "\033[48;2;32;30;28m",   "\033[1;38;2;255;152;72m",  "\033[1;38;2;86;182;194m"  };
	constexpr UI::MainTheme HighContrast    =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;255;238;88m",  "\033[1;38;2;90;230;240m",  "\033[1;38;2;190;190;190m", "\033[1;38;2;105;240;120m", "\033[48;2;12;12;12m",   "\033[1;38;2;255;160;70m",  "\033[1;38;2;120;150;255m" };
	constexpr UI::MainTheme NeonTheme       =  {  "\033[1;38;2;255;95;190m",  "\033[1;38;2;60;190;255m",  "\033[1;38;2;140;255;80m",  "\033[1;38;2;130;130;150m", "\033[1;38;2;255;235;80m",  "\033[48;2;22;18;34m",   "\033[1;38;2;255;170;60m",  "\033[1;38;2;170;120;255m" };
	constexpr UI::MainTheme OceanTheme      =  {  "\033[1;38;2;90;165;245m",  "\033[1;38;2;80;205;215m",  "\033[1;38;2;130;225;190m", "\033[1;38;2;110;140;175m", "\033[1;38;2;160;225;245m", "\033[48;2;16;30;46m",   "\033[1;38;2;255;205;90m",  "\033[1;38;2;255;130;90m"  };
	constexpr UI::MainTheme SunsetTheme     =  {  "\033[1;38;2;245;150;70m",  "\033[1;38;2;232;92;92m",   "\033[1;38;2;222;115;165m", "\033[1;38;2;178;128;102m", "\033[1;38;2;255;196;170m", "\033[48;2;38;22;22m",   "\033[1;38;2;255;226;100m", "\033[1;38;2;80;190;200m"  };
	constexpr UI::MainTheme ForestTheme     =  {  "\033[1;38;2;125;190;95m",  "\033[1;38;2;90;185;160m",  "\033[1;38;2;190;230;120m", "\033[1;38;2;130;150;100m", "\033[1;38;2;240;225;140m", "\033[48;2;18;30;20m",   "\033[1;38;2;235;170;70m",  "\033[1;38;2;170;135;235m" };
	constexpr UI::MainTheme MidnightTheme   =  {  "\033[1;38;2;190;120;255m", "\033[1;38;2;130;140;255m", "\033[1;38;2;240;140;220m", "\033[1;38;2;125;125;190m", "\033[1;38;2;220;205;255m", "\033[48;2;24;20;42m",   "\033[1;38;2;255;175;110m", "\033[1;38;2;110;200;150m" };
	constexpr UI::MainTheme MonoTheme       =  {  "\033[1;38;2;255;255;255m", "\033[1;38;2;180;180;180m", "\033[1;38;2;200;205;210m", "\033[1;38;2;90;90;90m",    "\033[1;38;2;210;210;210m", "\033[48;2;40;40;40m",   "\033[1;38;2;170;170;170m", "\033[1;38;2;130;130;130m" };
	constexpr UI::MainTheme RetroTheme      =  {  "\033[1;38;2;240;178;60m",  "\033[1;38;2;214;120;60m",  "\033[1;38;2;190;185;100m", "\033[1;38;2;140;115;85m",  "\033[1;38;2;255;214;120m", "\033[48;2;34;24;14m",   "\033[1;38;2;240;100;60m",  "\033[1;38;2;90;160;230m"  };
	constexpr UI::MainTheme CrimsonTheme    =  {  "\033[1;38;2;232;84;84m",   "\033[1;38;2;240;140;110m", "\033[1;38;2;230;120;170m", "\033[1;38;2;165;120;120m", "\033[1;38;2;255;205;200m", "\033[48;2;34;14;16m",   "\033[1;38;2;255;185;70m",  "\033[1;38;2;70;190;190m"  };
	constexpr UI::MainTheme DraculaTheme    =  {  "\033[1;38;2;189;147;249m", "\033[1;38;2;255;121;198m", "\033[1;38;2;80;250;123m",  "\033[1;38;2;110;128;185m", "\033[1;38;2;241;250;140m", "\033[48;2;28;30;40m",   "\033[1;38;2;255;184;108m", "\033[1;38;2;0;134;57m"    };
	constexpr UI::MainTheme TokyoNightTheme =  {  "\033[1;38;2;135;175;255m", "\033[1;38;2;224;82;151m",  "\033[1;38;2;158;206;106m", "\033[1;38;2;115;127;183m", "\033[1;38;2;224;175;104m", "\033[48;2;26;27;38m",   "\033[1;38;2;255;158;100m", "\033[1;38;2;31;173;104m"  };
	constexpr UI::MainTheme PaperTheme      =  {  "\033[1;38;2;235;225;200m", "\033[1;38;2;230;110;95m",  "\033[1;38;2;180;145;225m", "\033[1;38;2;150;140;120m", "\033[1;38;2;130;200;120m", "\033[48;2;35;28;18m",   "\033[1;38;2;235;160;70m",  "\033[1;38;2;90;200;210m"  };
	constexpr UI::MainTheme SakuraTheme     =  {  "\033[1;38;2;240;150;190m", "\033[1;38;2;235;120;105m", "\033[1;38;2;160;145;235m", "\033[1;38;2;160;140;152m", "\033[1;38;2;110;200;150m", "\033[48;2;30;20;35m",   "\033[1;38;2;235;165;70m",  "\033[1;38;2;90;200;220m"  };
} // namespace

void UI::Palette::updateDefaultColors() {

	using namespace UI::Palette;

	static constexpr std::pair<std::string_view, std::string_view> colorMap[] = {
		{"ash",      "\033[1;38;2;172;192;174m"},
		{"black",    "\033[1;38;2;62;62;68m"},
		{"charcoal", "\033[1;38;2;80;94;108m"},
		{"gray",     "\033[1;38;2;146;146;152m"},
		{"silver",   "\033[1;38;2;192;192;192m"},
		{"steel",    "\033[1;38;2;106;128;154m"},
		{"white",    "\033[1;38;2;222;222;222m"}
	};

	auto it = std::lower_bound(
	    std::begin(colorMap),
	    std::end(colorMap),
	    defaultColor,
	    [](const auto& entry, std::string_view val) {

			return entry.first < val;

		}
	);

	std::string_view code =
	    (it != std::end(colorMap) && it->first == defaultColor)
	        ? it->second
	        : "\033[1;38;2;222;222;222m";

	BoldReset = std::string(code);

	PromptBoldAlt =
	    "\001" + std::string(code.substr(0, code.size() - 1)) + "m\002";

}

std::string_view getskin() {

	using namespace UI::Palette;
	static constexpr std::pair<std::string_view, std::string_view> skinMap[] = {
		{"amber",    Amber},
		{"black",    Black},
		{"cyan",     Cyan},
		{"gray",     Gray},
		{"green",    Green},
		{"purple",   Purple},
		{"rose",     Rose},
		{"white",    White},
	};
	auto it = std::lower_bound(std::begin(skinMap), std::end(skinMap), skin,
	    [](const auto& entry, std::string_view val) { return entry.first < val; });
	color = (it != std::end(skinMap) && it->first == skin) ? it->second : Reset;
	return color;

}

const UI::MainTheme* getActiveTheme() {

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

// ============================================================
//  Interactive Prompts & Input
// ============================================================

std::string_view getOperationColor(std::string_view operation) {

	using namespace UI::Palette;

	if (operation == "mount" || operation == "cp") return Green;
	if (operation == "umount" || operation == "mv" || operation == "write2usb" || operation == "w2usb") return Yellow;
	if (operation == "rm") return Red;
	if (operation == "c2iso" || operation == "ccd2iso" || operation == "mdf2iso" ||
	    operation == "nrg2iso" || operation == "chd2iso" || operation == "daa2iso") return Orange;
	return BoldReset;

}

LineEditorAndPromptTheme getPromptTheme() {

	const MainTheme* t = getActiveTheme();
	const bool orig = (globalTheme == "original");

	auto wrap = [](std::string_view s) -> std::string {

		return "\001" + std::string(s) + "\002";

	};

	LineEditorAndPromptTheme pt;

	if (orig) {
		pt.accent    = wrap(UI::Palette::Green);
		pt.primary   = wrap(UI::Palette::Blue);
		pt.filter    = wrap(UI::Palette::Cyan);
		pt.highlight = wrap(UI::Palette::Orange);
		pt.reset     = wrap(UI::Palette::BoldReset);
		pt.iso       = wrap(UI::Palette::Green);
	} else {
		pt.accent    = wrap(UI::Palette::Green);
		pt.primary   = wrap(t->muted);
		pt.filter    = wrap(UI::Palette::Cyan);
		pt.highlight = wrap(UI::Palette::Orange);
		pt.reset     = wrap(UI::Palette::BoldReset);
		pt.iso       = wrap(UI::Palette::Green);
	}

	return pt;

}

LineEditorAndPromptTheme getFilterTheme(const std::string& operationColor, bool includeIso) {

	const MainTheme* t = getActiveTheme();
	const bool orig = (globalTheme == "original");

	auto wrap = [](std::string_view s) -> std::string {

		return "\001" + std::string(s) + "\002";

	};

	LineEditorAndPromptTheme ft;

	if (orig) {
		ft.primary   = wrap(UI::Palette::Blue);
		ft.filter    = wrap(UI::Palette::Cyan);
		ft.highlight = wrap(UI::Palette::Orange);
		ft.reset     = wrap(UI::Palette::BoldReset);
		ft.iso       = includeIso ? wrap(UI::Palette::Green) : "";
		ft.accent    = UI::Palette::Brown;
	} else {
		ft.primary   = wrap(t->muted);
		ft.filter    = wrap(UI::Palette::Cyan);
		ft.highlight = operationColor.empty() ? wrap(t->highlight) : wrap(operationColor);
		ft.reset     = wrap(UI::Palette::BoldReset);
		ft.iso       = includeIso ? wrap(t->accent) : "";
		ft.accent    = t->muted;
	}

	return ft;

}

LineEditorColors resolveLineEditorTheme() {

	const MainTheme* theme = getActiveTheme();
	const bool isOrig = (globalTheme == "original");

	static constexpr std::string_view fileColor = UI::Palette::DimGray;

	if (isOrig) {
		return {
			UI::Palette::Brown.data(),       // label
			UI::Palette::Yellow.data(),      // hint
			UI::Palette::Blue.data(),        // dir
			fileColor.data(),                // file (dim gray)
			UI::Palette::Reset.data(),       // reset
			UI::Palette::Yellow.data(),      // warning
			UI::Palette::Purple.data()       // accent
		};
	}

	return {
		theme->muted.data(),                 // label
		theme->accent.data(),                // hint
		theme->accent.data(),                // dir
		fileColor.data(),                    // file (dim gray)
		UI::Palette::Reset.data(),           // reset
		theme->warning.data(),               // warning
		theme->accent.data()                 // accent
	};

}

// ============================================================
//  List & Navigation Display
// ============================================================

PrintListTheme getListColors() {

	const MainTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");
	return {
		isOriginal ? UI::Palette::DarkCyan  : theme->accent,
		isOriginal ? UI::Palette::Brown     : theme->muted,
		isOriginal ? UI::Palette::Yellow    : theme->warning,
		isOriginal ? UI::Palette::Magenta   : theme->accent,
		isOriginal ? UI::Palette::Orange    : theme->highlight,
		isOriginal ? UI::Palette::Blue      : theme->background,
		isOriginal ? UI::Palette::DimGray   : UI::Palette::DimGray,
		isOriginal ? UI::Palette::Red       : theme->secondary,
		isOriginal ? UI::Palette::Green     : theme->indexDouble,
		isOriginal ? UI::Palette::BoldReset : theme->muted,
		isOriginal ? UI::Palette::BGNavy    : theme->background,  // bracketBg
		isOriginal ? UI::Palette::Green     : theme->accent       // procText
	};

}

// ============================================================
//  File Operations
// ============================================================

CpMvRmColors getCpMvRmColors() {

	const MainTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");

	CpMvRmColors colors;
	colors.arrow         = UI::Palette::BoldReset;
	colors.dir           = isOriginal ? UI::Palette::BoldReset  : theme->muted;
	colors.iso           = isOriginal ? UI::Palette::Magenta    : theme->accent;
	colors.error_label   = isOriginal ? UI::Palette::Red        : theme->secondary;
	colors.error_path    = isOriginal ? UI::Palette::Yellow      : theme->warning;
	colors.success_label = isOriginal ? UI::Palette::BoldReset  : theme->muted;
	colors.success_path  = isOriginal ? UI::Palette::Green      : theme->primary;
	colors.dest_path     = isOriginal ? UI::Palette::Blue       : theme->accent;
	colors.abort         = isOriginal ? UI::Palette::Yellow      : theme->warning;
	colors.prompt_green  = isOriginal ? UI::Palette::Green      : theme->accent;
	colors.prompt_blue   = isOriginal ? UI::Palette::Blue       : theme->muted;

	return colors;

}

ConversionThemeStrings getConversionThemeStrings() {

	const MainTheme* theme = getActiveTheme();
	const bool isOriginal = (globalTheme == "original");

	return ConversionThemeStrings{
		.errLabel     = isOriginal ? UI::Palette::Red      : theme->secondary,
		.errPath      = isOriginal ? UI::Palette::Yellow   : theme->warning,
		.missingLabel = isOriginal ? UI::Palette::Purple   : theme->secondary,
		.okLabel      = isOriginal ? UI::Palette::BoldReset : theme->muted,
		.okPath       = isOriginal ? UI::Palette::Green    : theme->primary,
		.skipLabel    = isOriginal ? UI::Palette::Yellow   : theme->warning,
		.skipPath     = isOriginal ? UI::Palette::Green    : theme->primary
	};

}

WriteTheme getWriteTheme() {

	const MainTheme* t  = getActiveTheme();
	const bool orig     = (globalTheme == "original");

	WriteTheme wt;

	if (orig) {
		wt.errLabel     = UI::Palette::Red;
		wt.errPath      = UI::Palette::Yellow;
		wt.warnLabel    = UI::Palette::Yellow;
		wt.infoLabel    = UI::Palette::Green;
		wt.bold         = UI::Palette::BoldReset;

		wt.headerCol    = UI::Palette::Green;
		wt.indexCol     = UI::Palette::Yellow;
		wt.pathCol      = UI::Palette::BoldReset;
		wt.fileCol      = UI::Palette::Magenta;
		wt.sizeCol      = UI::Palette::Purple;
		wt.warnCol      = UI::Palette::Red;

		wt.colorSuccess = UI::Palette::Green;
		wt.colorFailure = UI::Palette::Red;
		wt.colorWarning = UI::Palette::Yellow;
		wt.colorStatus  = UI::Palette::BoldReset;
		wt.deviceCol    = UI::Palette::Yellow;
		wt.speedCol     = UI::Palette::BoldReset;

		wt.editorLabelCol     = "\001" + std::string(UI::Palette::Green)     + "\002";
		wt.editorPrimaryCol   = "\001" + std::string(UI::Palette::Blue)      + "\002";
		wt.editorHighlightCol = "\001" + std::string(UI::Palette::Yellow)    + "\002";
		wt.editorErrorCol     = "\001" + std::string(UI::Palette::Red)       + "\002";
		wt.editorResetCol     = "\001" + std::string(UI::Palette::BoldReset) + "\002";
	} else {
		wt.errLabel     = t->secondary;
		wt.errPath      = t->warning;
		wt.warnLabel    = t->warning;
		wt.infoLabel    = t->primary;
		wt.bold         = UI::Palette::BoldReset;

		wt.headerCol    = t->accent;
		wt.indexCol     = t->warning;
		wt.pathCol      = t->muted;
		wt.fileCol      = t->accent;
		wt.sizeCol      = t->highlight;
		wt.warnCol      = t->warning;

		wt.colorSuccess = t->primary;
		wt.colorFailure = t->secondary;
		wt.colorWarning = t->warning;
		wt.colorStatus  = t->muted;
		wt.deviceCol    = t->warning;
		wt.speedCol     = UI::Palette::BoldReset;

		wt.editorLabelCol     = "\001" + std::string(t->accent)               + "\002";
		wt.editorPrimaryCol   = "\001" + std::string(t->muted)                + "\002";
		wt.editorHighlightCol = "\001" + std::string(t->warning)              + "\002";
		wt.editorErrorCol     = "\001" + std::string(t->secondary)            + "\002";
		wt.editorResetCol     = "\001" + std::string(UI::Palette::BoldReset)  + "\002";
	}

	return wt;

}

// ============================================================
//  Database Operations
// ============================================================

VerboseAndDatabaseTheme getDatabaseTheme() {

	const MainTheme* t = getActiveTheme();
	const bool orig = (globalTheme == "original");

	VerboseAndDatabaseTheme dt;

	if (orig) {
		dt.green   = std::string(UI::Palette::Green);
		dt.blue    = std::string(UI::Palette::Blue);
		dt.orange  = std::string(UI::Palette::Orange);
		dt.yellow  = std::string(UI::Palette::Yellow);
		dt.red     = std::string(UI::Palette::Red);
		dt.purple  = std::string(UI::Palette::Purple);
		dt.bold    = std::string(UI::Palette::BoldReset);
		dt.magenta = "";
		dt.reset   = std::string(UI::Palette::BoldReset);
	} else {
		dt.green   = t->accent;
		dt.blue    = t->muted;
		dt.orange  = t->highlight;
		dt.yellow  = t->warning;
		dt.red     = t->secondary;
		dt.purple  = t->secondary;
		dt.bold    = t->muted;
		dt.magenta = "";
		dt.reset   = std::string(UI::Palette::BoldReset);
	}

	return dt;

}

SemanticUIColors resolveDatabaseTheme() {

	const MainTheme* theme = getActiveTheme();

	if (globalTheme == "original") {
		return {
			.label     = UI::Palette::Green,
			.accent    = UI::Palette::Blue,
			.warning   = UI::Palette::Yellow,
			.error     = UI::Palette::Red,
			.reset     = UI::Palette::BoldReset,
			.path      = UI::Palette::Orange,
			.highlight = UI::Palette::Green,
			.data      = UI::Palette::BoldReset,
			.str       = UI::Palette::Cyan
		};
	}
	return {
		.label     = theme->muted,
		.accent    = theme->accent,
		.warning   = theme->warning,
		.error     = theme->secondary,
		.reset     = UI::Palette::BoldReset,
		.path      = theme->warning,
		.highlight = theme->accent,
		.data      = theme->accent,
		.str       = UI::Palette::Cyan
	};

}

// ============================================================
//  Verbose / Logging Output
// ============================================================

VerboseAndDatabaseTheme getVerboseTheme() {

	const MainTheme* t = getActiveTheme();
	const bool orig = (globalTheme == "original");

	VerboseAndDatabaseTheme vt;

	if (orig) {
		vt.red     = std::string(UI::Palette::Red);
		vt.yellow  = std::string(UI::Palette::Yellow);
		vt.green   = std::string(UI::Palette::Green);
		vt.purple  = std::string(UI::Palette::Purple);
		vt.magenta = std::string(UI::Palette::Magenta);
		vt.blue    = std::string(UI::Palette::Blue);
		vt.orange  = std::string(UI::Palette::Orange);
		vt.bold    = std::string(UI::Palette::BoldReset);
		vt.reset   = std::string(UI::Palette::BoldReset);
	} else {
		vt.red     = t->secondary;
		vt.yellow  = t->warning;
		vt.green   = t->primary;
		vt.purple  = t->secondary;
		vt.magenta = t->highlight;
		vt.blue    = t->primary;
		vt.orange  = t->highlight;
		vt.bold    = t->muted;
		vt.reset   = std::string(UI::Palette::BoldReset);
	}

	return vt;

}

SemanticUIColors resolveVerboseTheme() {

	const MainTheme* theme = getActiveTheme();

	if (globalTheme == "original") {
		return {
			.label     = UI::Palette::BoldReset,
			.accent    = {},
			.warning   = UI::Palette::Yellow,
			.error     = UI::Palette::Red,
			.reset     = UI::Palette::BoldReset,
			.path      = UI::Palette::Green,
			.highlight = UI::Palette::Blue,
			.data      = {},
			.str       = {}
		};
	}
	return {
		.label     = theme->muted,
		.accent    = {},
		.warning   = theme->warning,
		.error     = theme->secondary,
		.reset     = UI::Palette::BoldReset,
		.path      = theme->primary,
		.highlight = theme->accent,
		.data      = {},
		.str       = {}
	};

}

// ============================================================
//  Configuration / Setup UI
// ============================================================

SemanticUIColors resolveOptionsTheme() {

	const MainTheme* theme = getActiveTheme();
	const bool isOrig = (globalTheme == "original");

	if (isOrig) {
		return {
			.label     = UI::Palette::BoldReset,
			.accent    = UI::Palette::Blue,
			.warning   = UI::Palette::Yellow,
			.error     = UI::Palette::Red,
			.reset     = UI::Palette::BoldReset,
			.path      = UI::Palette::Blue,
			.highlight = UI::Palette::Green,
			.data      = UI::Palette::BoldReset,
			.str       = UI::Palette::Brown
		};
	}
	return {
		.label     = theme->muted,
		.accent    = theme->accent,
		.warning   = theme->warning,
		.error     = theme->secondary,
		.reset     = UI::Palette::BoldReset,
		.path      = theme->muted,
		.highlight = theme->accent,
		.data      = theme->accent,
		.str       = theme->muted
	};

}

// ============================================================
//  High-Frequency Rendering
// ============================================================

ProgressBarColors resolveProgressTheme() {

	const MainTheme* theme = getActiveTheme();

	if (globalTheme == "original") {
		return {
			UI::Palette::Green.data(),
			UI::Palette::Red.data(),
			UI::Palette::Yellow.data(),
			UI::Palette::BoldReset.data(),
			UI::Palette::BoldReset.data()
		};
	}
	return {
		theme->primary.data(),
		theme->secondary.data(),
		theme->warning.data(),
		theme->muted.data(),
		UI::Palette::BoldReset.data()
	};

}
