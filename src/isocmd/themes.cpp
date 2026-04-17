// SPDX-License-Identifier: GPL-3.0-or-later

#include "../themes.h"
#include "../headers.h"

/**
 * @file themes.cpp
 * @brief Theme resolution and color palette management for ISO Commander UI.
 *
 * Implements theme-aware color getter functions that adapt between the original
 * hardcoded color scheme and user-selected custom themes (classic, neon, ocean,
 * sunset, forest, midnight, mono, retro, crimson, dracula, tokyo).
 *
 * Provides context-specific color palettes and semantic resolvers for:
 * - List displays (PrintListTheme)
 * - File operations (CpMvRmColors)
 * - Write/export UI (WriteTheme)
 * - Database operations (VerboseAndDatabaseTheme / SemanticUIColors)
 * - Interactive prompts (ReadlineAndPromptTheme)
 * - Verbose logging (VerboseAndDatabaseTheme / SemanticUIColors)
 * - Configuration menus (SemanticUIColors)
 * - CLI tab-completion (ReadlineColors)
 * - High-frequency UI (ProgressBarColors)
 *
 * Merged structs (see headers.h):
 * - VerboseAndDatabaseTheme: replaces VerboseTheme + DatabaseTheme (std::string named-color bags)
 * - ReadlineAndPromptTheme:  replaces PromptTheme + FilterTheme (readline-wrapped std::string)
 * - SemanticUIColors:        replaces SetupColors + VerboseMountColors + DatabaseSwitchesColors
 *
 * Data structures are optimized based on use-case:
 * - std::string_view: Used for general UI logic and structured bindings.
 * - const char*:      Used for high-frequency rendering and C-style library hooks (Readline).
 *
 * Readline-wrapped color variants (\001/\002) are provided for interactive
 * prompts to ensure correct line editing behavior.
 *
 * @see getActiveTheme()
 * @see globalTheme
 */


// ============================================================
//  Interactive Prompts & Input
// ============================================================

/**
 * @brief Creates and returns a ReadlineAndPromptTheme for readline-powered interactive prompts.
 *
 * Generates ANSI color codes wrapped with readline delimiter markers (\001/\002)
 * to ensure correct width calculation during line editing.
 *
 * Merged from: getPromptTheme()
 * Call site usage:
 * - .accent    — ISO path / active input color  (was pt.iso)
 * - .primary   — neutral/muted base text        (was pt.muted)
 * - .filter    — filter bar highlight
 * - .highlight — orange emphasis
 * - .reset     — boldAlt terminal reset
 * - .iso       — unused / empty for prompt-only callers
 *
 * @return ReadlineAndPromptTheme Populated with readline-wrapped color codes.
 *
 * @note \001/\002 wrapping prevents readline from counting ANSI codes as display
 *       characters, avoiding line-wrapping glitches.
 */
ReadlineAndPromptTheme getPromptTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };

    ReadlineAndPromptTheme pt;

    if (orig) {
        pt.accent    = wrap(originalColors::green);
        pt.primary   = wrap(originalColors::blue);
        pt.filter    = wrap(originalColors::cyan);
        pt.highlight = wrap(originalColors::orange);
        pt.reset     = wrap(originalColors::boldAlt);
        pt.iso       = wrap(originalColors::green);
    } else {
        pt.accent    = wrap(originalColors::green);
        pt.primary   = wrap(t->muted);
        pt.filter    = wrap(t->accent);
        pt.highlight = wrap(originalColors::orange);
        pt.reset     = wrap(originalColors::boldAlt);
        pt.iso       = wrap(originalColors::green);
    }

    return pt;
}

/**
 * @brief Creates and returns a ReadlineAndPromptTheme for readline-powered filter/prompt UI.
 *
 * Generates ANSI color codes wrapped with readline delimiter markers (\001/\002)
 * for use in interactive filter bars and search interfaces. Supports optional
 * ISO path coloring and custom operation-specific highlight colors.
 *
 * Merged from: getFilterTheme()
 * Call site usage:
 * - .primary   — neutral base / muted label text
 * - .filter    — filter bar accent color
 * - .highlight — operation-specific or theme highlight
 * - .reset     — boldAlt terminal reset
 * - .iso       — ISO path color (empty string when includeIso is false)
 * - .accent    — unused / empty for filter-only callers
 *
 * @param operationColor Optional custom color for highlight text (raw ANSI string).
 * @param includeIso     When true, enables ISO path coloring via .iso field.
 * @return ReadlineAndPromptTheme Populated with readline-wrapped color codes.
 *
 * @note \001/\002 wrapping prevents readline from counting ANSI codes as display
 *       characters, avoiding line-wrapping issues in filter prompts.
 */
ReadlineAndPromptTheme getFilterTheme(const std::string& operationColor, bool includeIso) {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };

    ReadlineAndPromptTheme ft;

    if (orig) {
        ft.primary   = wrap(originalColors::blue);
        ft.filter    = wrap(originalColors::cyan);
        ft.highlight = wrap(originalColors::orange);
        ft.reset     = wrap(originalColors::boldAlt);
        ft.iso       = includeIso ? wrap(originalColors::green) : "";
        ft.accent    = "";
    } else {
        ft.primary   = wrap(t->muted);
        ft.filter    = wrap(t->accent);
        ft.highlight = operationColor.empty() ? wrap(t->highlight) : wrap(operationColor);
        ft.reset     = wrap(originalColors::boldAlt);
        ft.iso       = includeIso ? wrap(t->accent) : "";
        ft.accent    = "";
    }

    return ft;
}

/**
 * @brief Resolves semantic colors for Readline tab-completion listings.
 *
 * Provides raw @c const @c char* pointers specifically optimized for the custom
 * Readline listing hook. Ensures compatibility with @c printf-based formatting
 * used in completion displays.
 *
 * @return A @ref ReadlineColors struct containing:
 * - @b label: Muted color for completion headers and pagination info.
 * - @b hint:  Highlight color for interactive hints (e.g., "Ctrl+l").
 * - @b dir:   Distinct color for directory entries.
 * - @b file:  Standard color for regular file entries.
 * - @b reset: Terminal reset/boldAlt sequence.
 */
ReadlineColors resolveReadlineTheme() {
    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    if (isOrig) {
        return {
            originalColors::brown.data(),
            originalColors::yellow.data(),
            originalColors::blue.data(),
            originalColors::resetPlain.data(),
            originalColors::boldAlt.data()
        };
    }

    return {
        theme->muted.data(),
        theme->accent.data(),
        theme->accent.data(),
        originalColors::resetPlain.data(),
        originalColors::boldAlt.data()
    };
}


// ============================================================
//  List & Navigation Display
// ============================================================

/**
 * @brief Resolves the color palette for list rendering based on the active theme mode.
 *
 * @param isOriginal Boolean flag indicating if the legacy 'original' theme is active.
 * @param theme      Pointer to the current active MainTheme (ignored if isOriginal is true).
 *
 * @return PrintListTheme A struct containing std::string_view color codes for:
 * - UI accents, headers, and numbers
 * - File type indicators (ISO, Image, Mounted)
 * - Decorative elements (Squares, Directories, and alternating row indices)
 */
PrintListTheme getListColors(bool isOriginal, const MainTheme* theme) {
    if (isOriginal) {
        return {
            originalColors::darkCyan, originalColors::brown,   originalColors::yellow,
            originalColors::magenta,  originalColors::orange,  originalColors::blue,
            originalColors::dimGray,  originalColors::red,     originalColors::green,
            originalColors::boldAlt
        };
    }
    return {
        theme->accent,           theme->muted,      theme->warning,
        theme->accent,           theme->highlight,  theme->secondary,
        originalColors::dimGray, theme->secondary,  theme->accent,
        theme->muted
    };
}


// ============================================================
//  File Operations
// ============================================================

/**
 * @brief Retrieves the color scheme for Copy/Move/Remove operations.
 *
 * @return CpMvRmColors A structure containing all color string_views for cp/mv/rm operations.
 *
 * @see CpMvRmColors
 * @see getActiveTheme()
 */
CpMvRmColors getCpMvRmColors() {
    const MainTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    CpMvRmColors colors;
    colors.arrow         = originalColors::boldAlt;
    colors.dir           = isOriginal ? originalColors::boldAlt : theme->muted;
    colors.iso           = isOriginal ? originalColors::magenta : theme->accent;
    colors.error_label   = isOriginal ? originalColors::red     : theme->secondary;
    colors.error_path    = isOriginal ? originalColors::yellow  : theme->warning;
    colors.success_label = isOriginal ? originalColors::boldAlt : theme->muted;
    colors.success_path  = isOriginal ? originalColors::green   : theme->primary;
    colors.dest_path     = isOriginal ? originalColors::blue    : theme->accent;
    colors.abort         = isOriginal ? originalColors::yellow  : theme->warning;
    colors.prompt_green  = isOriginal ? originalColors::green   : theme->accent;
    colors.prompt_blue   = isOriginal ? originalColors::blue    : theme->secondary;

    return colors;
}

/**
 * @brief Gets themed color strings for conversion output messages.
 *
 * @return ConversionThemeStrings Struct containing themed string views for errors,
 *         success messages, skipped files, and their respective paths.
 */
ConversionThemeStrings getConversionThemeStrings() {
    const MainTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    return ConversionThemeStrings{
        .errLabel     = isOriginal ? originalColors::red     : theme->secondary,
        .errPath      = isOriginal ? originalColors::yellow  : theme->warning,
        .missingLabel = isOriginal ? originalColors::purple  : theme->secondary,
        .okLabel      = isOriginal ? originalColors::boldAlt : theme->muted,
        .okPath       = isOriginal ? originalColors::green   : theme->primary,
        .skipLabel    = isOriginal ? originalColors::yellow  : theme->warning,
        .skipPath     = isOriginal ? originalColors::green   : theme->primary
    };
}

/**
 * @brief Builds and returns the resolved WriteTheme for the current global theme.
 *
 * @return Fully populated WriteTheme.
 */
WriteTheme getWriteTheme() {
    const MainTheme* t  = getActiveTheme();
    const bool orig     = (globalTheme == "original");

    WriteTheme wt;

    if (orig) {
        wt.errLabel     = originalColors::red;
        wt.errPath      = originalColors::yellow;
        wt.warnLabel    = originalColors::yellow;
        wt.infoLabel    = originalColors::green;
        wt.bold         = originalColors::boldAlt;

        wt.headerCol    = originalColors::green;
        wt.indexCol     = originalColors::yellow;
        wt.pathCol      = originalColors::boldAlt;
        wt.fileCol      = originalColors::magenta;
        wt.sizeCol      = originalColors::purple;
        wt.warnCol      = originalColors::red;

        wt.colorSuccess = originalColors::green;
        wt.colorFailure = originalColors::red;
        wt.colorWarning = originalColors::yellow;
        wt.colorStatus  = originalColors::boldAlt;
        wt.deviceCol    = originalColors::yellow;
        wt.speedCol     = originalColors::boldAlt;

        wt.rl_labelCol     = "\001" + std::string(originalColors::green)   + "\002";
        wt.rl_primaryCol   = "\001" + std::string(originalColors::blue)    + "\002";
        wt.rl_highlightCol = "\001" + std::string(originalColors::yellow)  + "\002";
        wt.rl_errorCol     = "\001" + std::string(originalColors::red)     + "\002";
        wt.rl_resetCol     = "\001" + std::string(originalColors::boldAlt) + "\002";
    } else {
        wt.errLabel     = t->secondary;
        wt.errPath      = t->warning;
        wt.warnLabel    = t->warning;
        wt.infoLabel    = t->primary;
        wt.bold         = originalColors::boldAlt;

        wt.headerCol    = t->accent;
        wt.indexCol     = t->secondary;
        wt.pathCol      = t->muted;
        wt.fileCol      = t->accent;
        wt.sizeCol      = t->highlight;
        wt.warnCol      = t->warning;

        wt.colorSuccess = t->primary;
        wt.colorFailure = t->secondary;
        wt.colorWarning = t->warning;
        wt.colorStatus  = t->muted;
        wt.deviceCol    = t->secondary;
        wt.speedCol     = originalColors::boldAlt;

        wt.rl_labelCol     = "\001" + std::string(t->accent)               + "\002";
        wt.rl_primaryCol   = "\001" + std::string(t->muted)                + "\002";
        wt.rl_highlightCol = "\001" + std::string(t->secondary)            + "\002";
        wt.rl_errorCol     = "\001" + std::string(t->secondary)            + "\002";
        wt.rl_resetCol     = "\001" + std::string(originalColors::boldAlt) + "\002";
    }

    return wt;
}


// ============================================================
//  Database Operations
// ============================================================

/**
 * @brief Creates and returns a VerboseAndDatabaseTheme for database operation displays.
 *
 * Merged from: getDatabaseTheme()
 * The .magenta field is unused by database callers — it is zero-initialized
 * (empty string) and can be safely ignored at database call sites.
 *
 * Color semantics:
 * - green:   Success / INSERT operations
 * - blue:    Information / SELECT queries
 * - orange:  Highlights / UPDATE modifications
 * - yellow:  Warnings / notices
 * - red:     Errors / DELETE operations
 * - purple:  Metadata / schema information
 * - bold:    Emphasis / keywords
 * - reset:   Terminal reset sequence
 *
 * @return VerboseAndDatabaseTheme Populated theme structure.
 *
 * @note reset always uses originalColors::boldAlt for consistent termination.
 */
VerboseAndDatabaseTheme getDatabaseTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    VerboseAndDatabaseTheme dt;

    if (orig) {
        dt.green   = std::string(originalColors::green);
        dt.blue    = std::string(originalColors::blue);
        dt.orange  = std::string(originalColors::orange);
        dt.yellow  = std::string(originalColors::yellow);
        dt.red     = std::string(originalColors::red);
        dt.purple  = std::string(originalColors::purple);
        dt.bold    = std::string(originalColors::boldAlt);
        dt.magenta = "";
        dt.reset   = std::string(originalColors::boldAlt);
    } else {
        dt.green   = t->accent;
        dt.blue    = t->muted;
        dt.orange  = t->highlight;
        dt.yellow  = t->warning;
        dt.red     = t->secondary;
        dt.purple  = t->secondary;
        dt.bold    = t->muted;
        dt.magenta = "";
        dt.reset   = originalColors::boldAlt;
    }

    return dt;
}

/**
 * @brief Resolves semantic colors for database statistics and configuration state displays.
 *
 * Merged from: resolveDatabaseTheme() → now returns SemanticUIColors.
 * Call site field mapping:
 * - .accent    ← header  (section titles)
 * - .label     ← label   (category descriptions)
 * - .data      ← data    (numeric values / file paths)
 * - .warning   ← warning (buffered/cached entry counts)
 * - .error     ← error   (Disabled / Access Denied states)
 * - .str       ← string  (string-type value color)
 * - .reset     ← reset
 * - .highlight and .path are unused by database switch callers.
 *
 * @return SemanticUIColors Populated with database-switch color roles.
 */
SemanticUIColors resolveDatabaseTheme() {
    const MainTheme* theme = getActiveTheme();

    if (globalTheme == "original") {
        return {
            .label     = originalColors::green,
            .accent    = originalColors::blue,
            .warning   = originalColors::orange,
            .error     = originalColors::red,
            .reset     = UI::Palette::BoldReset,
            .path      = {},
            .highlight = {},
            .data      = originalColors::boldAlt,
            .str       = originalColors::cyan
        };
    }
    return {
        .label     = theme->muted,
        .accent    = theme->accent,
        .warning   = theme->warning,
        .error     = theme->secondary,
        .reset     = UI::Palette::BoldReset,
        .path      = {},
        .highlight = {},
        .data      = theme->accent,
        .str       = originalColors::cyan
    };
}


// ============================================================
//  Verbose / Logging Output
// ============================================================

/**
 * @brief Creates and returns a VerboseAndDatabaseTheme for detailed debug/logging output.
 *
 * Merged from: getVerboseTheme()
 * All nine fields are populated. Database callers only use a subset and leave
 * .magenta empty — verbose callers use all fields including .magenta.
 *
 * Color semantics:
 * - red:     Errors, critical failures
 * - yellow:  Warnings, deprecated features
 * - green:   Success, completed operations
 * - purple:  Debug info, stack traces
 * - magenta: Highlights, attention points  ← verbose-only field
 * - blue:    Informational messages, progress
 * - orange:  Cautions, non-critical issues
 * - bold:    Emphasis, important keywords
 * - reset:   Terminal reset sequence
 *
 * @return VerboseAndDatabaseTheme Populated theme structure.
 *
 * @note reset always uses originalColors::boldAlt for consistent termination.
 */
VerboseAndDatabaseTheme getVerboseTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");

    VerboseAndDatabaseTheme vt;

    if (orig) {
        vt.red     = std::string(originalColors::red);
        vt.yellow  = std::string(originalColors::yellow);
        vt.green   = std::string(originalColors::green);
        vt.purple  = std::string(originalColors::purple);
        vt.magenta = std::string(originalColors::magenta);
        vt.blue    = std::string(originalColors::blue);
        vt.orange  = std::string(originalColors::orange);
        vt.bold    = std::string(originalColors::boldAlt);
        vt.reset   = std::string(originalColors::boldAlt);
    } else {
        vt.red     = t->secondary;
        vt.yellow  = t->warning;
        vt.green   = t->primary;
        vt.purple  = t->secondary;
        vt.magenta = t->highlight;
        vt.blue    = t->primary;
        vt.orange  = t->highlight;
        vt.bold    = t->muted;
        vt.reset   = originalColors::boldAlt;
    }

    return vt;
}

/**
 * @brief Resolves semantic colors for mount and unmount verbose logging.
 *
 * Merged from: resolveVerboseTheme() → now returns SemanticUIColors.
 * Call site field mapping:
 * - .label     ← label     (static prefixes: "ISO:", "Unmounted:")
 * - .path      ← path      (file and directory paths)
 * - .highlight ← highlight (destination mount points)
 * - .warning   ← warning   (skipped states / path-specific warnings)
 * - .error     ← error     (failure prefixes)
 * - .reset     ← reset
 * - .accent, .data, .str are unused by verbose mount callers.
 *
 * @return SemanticUIColors Populated with verbose mount color roles.
 */
SemanticUIColors resolveVerboseTheme() {
    const MainTheme* theme = getActiveTheme();

    if (globalTheme == "original") {
        return {
            .label     = originalColors::boldAlt,
            .accent    = {},
            .warning   = originalColors::yellow,
            .error     = originalColors::red,
            .reset     = originalColors::boldAlt,
            .path      = originalColors::green,
            .highlight = originalColors::blue,
            .data      = {},
            .str       = {}
        };
    }
    return {
        .label     = theme->muted,
        .accent    = {},
        .warning   = theme->warning,
        .error     = theme->secondary,
        .reset     = originalColors::boldAlt,
        .path      = theme->primary,
        .highlight = theme->accent,
        .data      = {},
        .str       = {}
    };
}


// ============================================================
//  Configuration / Setup UI
// ============================================================

/**
 * @brief Resolves semantic UI colors for the options/setup menu.
 *
 * Merged from: resolveOptionsTheme() → now returns SemanticUIColors.
 * Call site field mapping:
 * - .label   ← label   (standard bold text)
 * - .accent  ← accent  (success / enable / positive highlight)
 * - .warning ← warning (value / numeric highlight)
 * - .error   ← error   (error / disable / negative highlight)
 * - .reset   ← boldReset
 * - .highlight ← (config header)
 * - .path, .data, .str are unused by setup callers.
 *
 * @return SemanticUIColors Populated with setup/options color roles.
 */
SemanticUIColors resolveOptionsTheme() {
    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    if (isOrig) {
        return {
            .label     = originalColors::boldAlt,
            .accent    = originalColors::green,
            .warning   = originalColors::yellow,
            .error     = originalColors::red,
            .reset     = UI::Palette::BoldReset,
            .path      = {},
            .highlight = originalColors::blue,
            .data      = {},
            .str       = {}
        };
    }
    return {
        .label     = theme->muted,
        .accent    = theme->accent,
        .warning   = theme->warning,
        .error     = theme->secondary,
        .reset     = UI::Palette::BoldReset,
        .path      = {},
        .highlight = theme->accent,
        .data      = {},
        .str       = {}
    };
}


// ============================================================
//  High-Frequency Rendering
// ============================================================

/**
 * @brief Resolves semantic colors for high-frequency progress bar rendering.
 *
 * Uses raw @c const @c char* pointers to avoid std::string allocations during
 * the high-frequency update loop (typically 100ms intervals).
 *
 * @return A @ref ProgressBarColors struct containing:
 * - @b success: Color for completed bar segments.
 * - @b failure: Color for failed state indicators.
 * - @b warning: Color for partial success or interruptions.
 * - @b status:  Color for brackets and metadata labels.
 * - @b reset:   Terminal reset sequence.
 */
ProgressBarColors resolveProgressTheme() {
    const MainTheme* theme = getActiveTheme();

    if (globalTheme == "original") {
        return {
            originalColors::green.data(),
            originalColors::red.data(),
            originalColors::yellow.data(),
            originalColors::boldAlt.data(),
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
