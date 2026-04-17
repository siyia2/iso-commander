// SPDX-License-Identifier: GPL-3.0-or-later

#include "../themes.h"
#include "../headers.h"

/**
 * @file themes.cpp
 * @brief Theme resolution and color palette management for ISO Commander UI.
 * * Implements theme-aware color getter functions that adapt between the original
 * hardcoded color scheme and user-selected custom themes (classic, neon, ocean,
 * sunset, forest, midnight, mono, retro, crimson, dracula, tokyo).
 * * Provides context-specific color palettes and semantic resolvers for:
 * - List displays (PrintListTheme)
 * - File operations (CpMvRmColors) 
 * - Write/export UI (WriteTheme)
 * - Database operations (DatabaseTheme)
 * - Interactive prompts (PromptTheme, FilterTheme)
 * - Verbose logging (VerboseMountColors/resolveVerboseTheme)
 * - Configuration menus (SetupColors/resolveTheme)
 * - CLI tab-completion (ReadlineColors/resolveReadlineTheme)
 * - High-frequency UI (ProgressBarColors/resolveProgressTheme)
 * * Data structures are optimized based on use-case:
 * - std::string_view: Used for general UI logic and structured bindings.
 * - const char*: Used for high-frequency rendering and C-style library hooks (Readline).
 * * Readline-wrapped color variants (\001/\002) are provided for interactive
 * prompts to ensure correct line editing behavior.
 * * @see getActiveTheme()
 * @see globalTheme
 */


// ============================================================
//  Interactive Prompts & Input
// ============================================================

/**
 * @brief Creates and returns a PromptTheme for readline-powered interactive prompts
 * 
 * Generates ANSI color codes wrapped with readline delimiter markers (\001/\002)
 * to ensure correct width calculation during line editing.
 * 
 * @return PromptTheme Populated theme structure with readline-wrapped color codes
 * 
 * Color mapping:
 * - Original theme: green for ISO, blue for muted, cyan for filter, orange for highlight
 * - Custom theme: theme's accent for ISO/filter, theme's muted for muted
 * - Highlight and reset always use original colors (orange/boldAlt) for consistency
 * 
 * @note Wrapping with \001 and \002 prevents readline from counting ANSI codes
 *       as display characters, avoiding line wrapping glitches
 */
PromptTheme getPromptTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");
    
    // Helper to wrap raw ANSI strings for readline
    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };
    
    PromptTheme pt;
    
    if (orig) {
        pt.iso       = wrap(originalColors::green);
        pt.muted     = wrap(originalColors::blue);
        pt.filter    = wrap(originalColors::cyan);
        pt.highlight = wrap(originalColors::orange);
        pt.reset     = wrap(originalColors::boldAlt);
    } else {
        pt.iso       = wrap(originalColors::green);
        pt.muted     = wrap(t->muted);
        pt.filter    = wrap(t->accent);
        pt.highlight = wrap(originalColors::orange);
        pt.reset     = wrap(originalColors::boldAlt);
    }
    
    return pt;
}

/**
 * @brief Creates and returns a FilterTheme for readline-powered filter/prompt UI
 * 
 * Generates ANSI color codes wrapped with readline delimiter markers (\001/\002)
 * for use in interactive filter bars and search interfaces. Supports optional
 * ISO path coloring and custom operation-specific highlight colors.
 * 
 * @param operationColor Optional custom color for highlight text (e.g., "red", "green")
 * @param includeIso     When true, enables ISO path coloring in the theme
 * @return FilterTheme   Populated theme structure with readline-wrapped color codes
 * 
 * Color mapping:
 * - Original theme: blue primary, cyan filter, orange highlight, green ISO
 * - Custom theme: muted primary, accent filter, theme highlight (or custom operationColor)
 * - Reset always uses originalColors::boldAlt for consistency
 * 
 * @note Wrapping with \001 and \002 prevents readline from counting ANSI codes
 *       as display characters, avoiding line wrapping issues in filter prompts
 * @note ISO coloring is conditionally included based on includeIso parameter
 *       (typically enabled for path selection interfaces)
 */
FilterTheme getFilterTheme(const std::string& operationColor, bool includeIso) {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");
    
    // Helper to wrap raw ANSI strings for readline
    auto wrap = [](std::string_view s) -> std::string {
        return "\001" + std::string(s) + "\002";
    };
    
    FilterTheme ft;
    
    if (orig) {
        ft.primary   = wrap(originalColors::blue);
        ft.filter    = wrap(originalColors::cyan);
        ft.highlight = wrap(originalColors::orange);
        ft.reset     = wrap(originalColors::boldAlt);
        ft.iso       = includeIso ? wrap(originalColors::green) : "";
    } else {
        ft.primary   = wrap(t->muted);
        ft.filter    = wrap(t->accent);
        ft.highlight = operationColor.empty() ? wrap(t->highlight) : wrap(operationColor);
        ft.reset     = wrap(originalColors::boldAlt);
        ft.iso       = includeIso ? wrap(t->accent) : "";
    }
    
    return ft;
}

/**
 * @brief Resolves semantic colors for Readline tab-completion listings.
 *
 * This function provides raw @c const @c char* pointers to theme colors, specifically 
 * optimized for the custom Readline listing hook. It ensures compatibility with 
 * @c printf-based formatting used in completion displays and maintains visual 
 * consistency between the CLI prompt and file listings.
 *
 * @return A @ref ReadlineColors struct containing:
 * - @b label: Muted color for the completion headers and pagination info.
 * - @b hint: Highlight color for interactive hints (e.g., "Ctrl+l").
 * - @b dir: Distinct color for directory entries in the listing.
 * - @b file: Standard color for regular file entries.
 * - @b reset: The terminal reset/boldAlt sequence for output cleanup.
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
        theme->accent.data(), // Directories use accent in modern themes
        originalColors::resetPlain.data(),
        originalColors::boldAlt.data()
    };
}


// ============================================================
//  List & Navigation Display
// ============================================================

/**
 * @brief Resolves the color palette for list rendering based on the active theme mode.
 * * This function abstracts the color selection logic, mapping either the legacy 
 * hardcoded "original" colors or the dynamic properties of the provided ListTheme
 * to a standardized set of UI components used by the list printer.
 * * @param isOriginal Boolean flag indicating if the legacy 'original' theme is active.
 * @param theme Pointer to the current active ListTheme configuration (ignored if isOriginal is true).
 * * @return PrintListTheme A struct containing std::string_view color codes for:
 * - UI accents, headers, and numbers
 * - File type indicators (ISO, Image, Mounted)
 * - Decorative elements (Squares, Directories, and alternating row indices)
 */
PrintListTheme getListColors(bool isOriginal, const MainTheme* theme) {
    if (isOriginal) {
        return {
            originalColors::darkCyan, originalColors::brown, originalColors::yellow,
            originalColors::magenta,  originalColors::orange, originalColors::blue,
            originalColors::dimGray,   originalColors::red,    originalColors::green,
            originalColors::boldAlt
        };
    }
    return {
        theme->accent,    theme->muted,    theme->warning,
        theme->accent,    theme->highlight, theme->secondary,
        originalColors::dimGray, theme->secondary, theme->accent,
        theme->muted
    };
}


// ============================================================
//  File Operations
// ============================================================

/**
 * @brief Retrieves the color scheme for Copy/Move/Remove operations.
 * 
 * This function generates a color palette specifically tailored for file operations
 * (copy, move, remove) in the ISO Commander. The colors adapt to either the
 * original classic theme or the currently active user-selected theme.
 * 
 * The color mapping includes:
 * - Arrow prefix indicators for list entries
 * - Directory and ISO filename highlighting
 * - Error messages with labeled and path components
 * - Success confirmations for completed operations
 * - Destination path visualization
 * - Abort/cancellation notifications
 * - Interactive prompt elements (green/blue accents)
 * 
 * @return CpMvRmColors A structure containing all color string_views for cp/mv/rm operations.
 *         The returned colors are either from the original palette (if globalTheme == "original")
 *         or from the active theme's mapped colors.
 * 
 * @note The function is theme-aware and automatically adjusts based on the current
 *       globalTheme setting ("original" vs custom themes).
 * 
 * @see CpMvRmColors
 * @see getActiveTheme()
 * @see globalTheme
 */
CpMvRmColors getCpMvRmColors() {
    const MainTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");
    
    CpMvRmColors colors;
    colors.arrow = originalColors::boldAlt;
    colors.dir   = isOriginal ? originalColors::boldAlt : theme->muted;
    colors.iso   = isOriginal ? originalColors::magenta : theme->accent;
    colors.error_label = isOriginal ? originalColors::red     : theme->secondary;
    colors.error_path  = isOriginal ? originalColors::yellow  : theme->warning;
    colors.success_label = isOriginal ? originalColors::boldAlt : theme->muted;
    colors.success_path  = isOriginal ? originalColors::green   : theme->primary;
    colors.dest_path     = isOriginal ? originalColors::blue    : theme->accent;
    colors.abort         = isOriginal ? originalColors::yellow  : theme->warning;
    colors.prompt_green  = isOriginal ? originalColors::green   : theme->accent;
    colors.prompt_blue   = isOriginal ? originalColors::blue    : theme->secondary;
    
    return colors;
}

/**
 * @brief Gets themed color strings for conversion output messages
 * 
 * @return ConversionThemeStrings Struct containing themed string views for errors,
 *         success messages, skipped files, and their respective paths
 * 
 * @note Colors are selected based on whether original theme or custom active theme is used
 */
ConversionThemeStrings getConversionThemeStrings() {
    const MainTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");
    
    return ConversionThemeStrings{
        .errLabel     = isOriginal ? originalColors::red      : theme->secondary,
        .errPath      = isOriginal ? originalColors::yellow   : theme->warning,
        .missingLabel = isOriginal ? originalColors::purple   : theme->secondary,
        .okLabel      = isOriginal ? originalColors::boldAlt  : theme->muted,
        .okPath       = isOriginal ? originalColors::green    : theme->primary,
        .skipLabel    = isOriginal ? originalColors::yellow   : theme->warning,
        .skipPath     = isOriginal ? originalColors::green    : theme->primary
    };
}

/**
 * @brief Builds and returns the resolved WriteTheme for the current global theme.
 *
 * Call once at the top of any write-UI function instead of repeating
 * getActiveTheme() + isOriginal checks inline.
 *
 * @return Fully populated WriteTheme.
 */
WriteTheme getWriteTheme() {
    const MainTheme* t   = getActiveTheme();
    const bool orig      = (globalTheme == "original");

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
        
        // No static storage needed - string_view is perfect!
        wt.rl_labelCol     = "\001" + std::string(originalColors::green) + "\002";
        wt.rl_primaryCol   = "\001" + std::string(originalColors::blue) + "\002";
        wt.rl_highlightCol = "\001" + std::string(originalColors::yellow) + "\002";
        wt.rl_errorCol     = "\001" + std::string(originalColors::red) + "\002";
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
        
        wt.rl_labelCol     = "\001" + std::string(t->accent) + "\002";
        wt.rl_primaryCol   = "\001" + std::string(t->muted) + "\002";
        wt.rl_highlightCol = "\001" + std::string(t->secondary) + "\002";
        wt.rl_errorCol     = "\001" + std::string(t->secondary) + "\002";
        wt.rl_resetCol     = "\001" + std::string(originalColors::boldAlt) + "\002";
    }

    return wt;
}


// ============================================================
//  Database Operations
// ============================================================

/**
 * @brief Creates and returns a DatabaseTheme for database operation displays
 * 
 * Configures color scheme for database-related UI elements including query results,
 * record operations, and status messages. Maps semantic database operations to
 * appropriate theme colors.
 * 
 * @return DatabaseTheme Populated theme structure with raw ANSI color codes
 * 
 * Color semantics:
 * - Green:   Success/INSERT operations
 * - Blue:    Information/SELECT queries
 * - Orange:  Highlights/UPDATE modifications
 * - Yellow:  Warnings/notices
 * - Red:     Errors/DELETE operations
 * - Purple:  Metadata/schema information
 * - Bold:    Emphasis/keywords
 * - Color:   Default database text color
 * 
 * Color mapping:
 * - Original theme: Uses hardcoded originalColors (green/blue/orange/yellow/red/purple)
 * - Custom theme: Maps to theme's accent (green), muted (blue), highlight (orange),
 *                 warning (yellow), secondary (red/purple)
 * 
 * @note reset always uses originalColors::boldAlt for consistent termination
 * @note getskin() provides the default text color for the current skin
 */
DatabaseTheme getDatabaseTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");
    
    DatabaseTheme dt;
    
    if (orig) {
        dt.green   = std::string(originalColors::green);
        dt.blue    = std::string(originalColors::blue);
        dt.orange  = std::string(originalColors::orange);
        dt.yellow  = std::string(originalColors::yellow);
        dt.red     = std::string(originalColors::red);
        dt.purple  = std::string(originalColors::purple);
        dt.bold    = std::string(originalColors::boldAlt);
        dt.reset   = std::string(originalColors::boldAlt);
    } else {
        dt.green   = t->accent;
        dt.blue    = t->muted;
        dt.orange  = t->highlight;
        dt.yellow  = t->warning;
        dt.red     = t->secondary;
        dt.purple  = t->secondary;
        dt.bold    = t->muted;
        dt.reset   = originalColors::boldAlt;
    }
    
    return dt;
}

/**
 * @brief Resolves semantic colors for database statistics and configuration state displays.
 * * This resolver maps theme colors to specific roles used in the "database switches"
 * and statistics screens. It provides a distinct visual hierarchy for headers, 
 * data values, and system status messages (Enabled/Disabled).
 * * @return A @ref DatabaseSwitchesColors struct containing:
 * - @b header: Highlight color for section titles (e.g., Blue/Accent).
 * - @b label: Muted color for category descriptions (e.g., Green/Muted).
 * - @b data: High-contrast color for numeric values and file paths.
 * - @b warning: Secondary highlight for buffered/cached entry counts.
 * - @b status: Positive feedback color for "Enabled" or "Cleared" states.
 * - @b error: Critical feedback color for "Disabled" or "Access Denied" states.
 * - @b reset: Standard terminal reset sequence to clear formatting.
 */
DatabaseSwitchesColors resolveDatabaseTheme() {
    const MainTheme* theme = getActiveTheme();
    if (globalTheme == "original") {
        return {
            originalColors::blue,    // header
            originalColors::green,   // label
            originalColors::boldAlt, // data
            originalColors::orange,  // warning
            originalColors::green,   // status
            originalColors::red,     // error
            originalColors::cyan, //string
            UI::Palette::BoldReset
        };
    }
    return {
        theme->accent,    // header
        theme->muted,     // label
        theme->accent,    // data
        theme->warning,   // warning
        theme->accent,    // status
        theme->secondary, // error
        originalColors::cyan, //string
        UI::Palette::BoldReset
    };
}


// ============================================================
//  Verbose / Logging Output
// ============================================================

/**
 * @brief Creates and returns a VerboseTheme for detailed debug/logging output
 * 
 * Configures color scheme for verbose logging and diagnostic messages,
 * providing semantic color coding for different log levels and message types.
 * 
 * @return VerboseTheme Populated theme structure with raw ANSI color codes
 * 
 * Color semantics:
 * - Red:     Errors, critical failures
 * - Yellow:  Warnings, deprecated features
 * - Green:   Success, completed operations
 * - Purple:  Debug info, stack traces
 * - Magenta: Highlights, attention points
 * - Blue:    Informational messages, progress
 * - Orange:  Cautions, non-critical issues
 * - Bold:    Emphasis, important keywords
 * - Color:   Default verbose text color
 * 
 * Color mapping:
 * - Original theme: Uses hardcoded originalColors with distinct semantic meanings
 * - Custom theme: Maps to theme's semantic colors:
 *   - secondary for errors/purple (critical/debug)
 *   - warning for warnings
 *   - primary for success/info
 *   - highlight for emphasis/magenta/orange
 *   - muted for bold emphasis
 * 
 * @note reset always uses originalColors::boldAlt for consistent termination
 * @note getskin() provides the default text color for the current skin
 */
VerboseTheme getVerboseTheme() {
    const MainTheme* t = getActiveTheme();
    const bool orig = (globalTheme == "original");
    
    VerboseTheme vt;
    
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
 * @brief Resolves semantic colors for both mount and unmount verbose logging.
 *
 * This resolver provides a specialized mapping for high-detail strings used in 
 * @ref VerbosityFormatter (mounts) and @ref VerboseMessageFormatter (unmounts). 
 * It ensures visual consistency across all disk operations by differentiating 
 * between static labels, filesystem paths, and action highlights.
 *
 * @details The mapping logic accounts for the legacy "original" theme by providing 
 * hardcoded ANSI overrides, while dynamic themes are mapped to their semantic 
 * equivalents (e.g., @c path maps to @c theme->primary).
 *
 * @return A @ref VerboseMountColors struct containing:
 * - @b label: Neutral text for prefixes (e.g., "ISO:", "Unmounted:").
 * - @b path: High-visibility color for file and directory paths.
 * - @b highlight: High-contrast color for destination mount points.
 * - @b warning: Distinct color for skipped states or path-specific warnings.
 * - @b error: Critical color for "Failed to mnt" or "Failed to unmount" prefixes.
 * - @b reset: The terminal default/reset sequence (usually boldAlt).
 */
VerboseMountColors resolveVerboseTheme() {
    const MainTheme* theme = getActiveTheme();
    if (globalTheme == "original") {
        return {
            originalColors::boldAlt, // label
            originalColors::green,   // path
            originalColors::blue,    // highlight
            originalColors::yellow,  // warning
            originalColors::red,     // error
            originalColors::boldAlt  // reset
        };
    }
    return {
        theme->muted,      // label
        theme->primary,    // path (usually primary/green)
        theme->accent,     // highlight (usually accent/blue)
        theme->warning,    // warning
        theme->secondary,  // error
        originalColors::boldAlt
    };
}


// ============================================================
//  Configuration / Setup UI
// ============================================================

/**
 * @brief Resolves semantic UI colors based on the active global theme.
 * * This function acts as a centralized mapper that translates internal theme 
 * states (e.g., "original" vs. custom themes) into a unified @ref SetupColors 
 * structure. It maps specific visual roles (labels, accents, warnings, errors) 
 * to the appropriate ANSI escape codes or hex values.
 * * @note Centralizing this lookup eliminates repetitive conditional checks 
 * within individual command handlers and UI components.
 * * @return A @ref SetupColors struct containing std::string_view references 
 * to the resolved theme colors.
 */
SetupColors resolveOptionsTheme() {
    const MainTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    if (isOrig) {
        return {
            originalColors::boldAlt,   ///< label: Standard bold white/reset
            originalColors::green,  ///< accent: Vibrant Kelly Green
            originalColors::yellow, ///< warning: Pure Yellow
            originalColors::red,     ///< error: Bright Red
            UI::Palette::BoldReset
        };
    }

    return {
        theme->muted,               ///< Neutral label / muted text
        theme->accent,              ///< Success / enable / positive highlight
        theme->warning,             ///< Value / numeric highlight
        theme->secondary,           ///< Error / disable / negative highlight
        UI::Palette::BoldReset
    };
}


// ============================================================
//  High-Frequency Rendering
// ============================================================

/**
 * @brief Resolves semantic colors specifically for high-frequency progress bar rendering.
 *
 * This resolver maps global theme states to a @ref ProgressBarColors struct using raw 
 * @c const @c char* pointers. By avoiding @c std::string allocations and centralized 
 * ternary logic, it ensures minimal CPU overhead during the high-frequency update 
 * loop (typically 100ms intervals).
 *
 * @return A @ref ProgressBarColors struct containing:
 * - @b success: Color for completed bar segments (e.g., Green/Primary).
 * - @b failure: Color for failed state indicators (e.g., Red/Secondary).
 * - @b warning: Color for partial success or interruptions (e.g., Yellow/Warning).
 * - @b status: Color for brackets and metadata labels (e.g., Muted/BoldAlt).
 * - @b reset: The terminal reset sequence for clean line endings.
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
