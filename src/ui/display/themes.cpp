// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.ui.display.themes;

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

    RL_BoldAlt =
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

ReadlineAndPromptTheme getPromptTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };

    ReadlineAndPromptTheme pt;

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

ReadlineAndPromptTheme getFilterTheme(const std::string& operationColor, bool includeIso) {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };

    ReadlineAndPromptTheme ft;

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

ReadlineColors resolveReadlineTheme() {
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

        wt.rl_labelCol     = "\001" + std::string(UI::Palette::Green)     + "\002";
        wt.rl_primaryCol   = "\001" + std::string(UI::Palette::Blue)      + "\002";
        wt.rl_highlightCol = "\001" + std::string(UI::Palette::Yellow)    + "\002";
        wt.rl_errorCol     = "\001" + std::string(UI::Palette::Red)       + "\002";
        wt.rl_resetCol     = "\001" + std::string(UI::Palette::BoldReset) + "\002";
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

        wt.rl_labelCol     = "\001" + std::string(t->accent)               + "\002";
        wt.rl_primaryCol   = "\001" + std::string(t->muted)                + "\002";
        wt.rl_highlightCol = "\001" + std::string(t->warning)              + "\002";
        wt.rl_errorCol     = "\001" + std::string(t->secondary)            + "\002";
        wt.rl_resetCol     = "\001" + std::string(UI::Palette::BoldReset)  + "\002";
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
