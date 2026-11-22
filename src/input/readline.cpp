// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "isocmd/core/state.h"
#include "isocmd/input/readline.h"
#include "isocmd/ui/themes.h"
#include "isocmd/usb/write2usbUI.h"

/**
 * @file readline_completion.cpp
 * @brief Custom GNU Readline completion and pagination logic for CLI commands and device mapping.
 */

static int current_page = 0;
static char last_common_prefix[1024] = "";

/**
 * @brief List of available special commands for completion.
 */
const char* special_cmds[] = {
    "!clr", "!clr_paths", "!clr_filter", "*stats",
    NULL
};

/**
 * @brief Generates completion matches for special commands based on prefix.
 * @param text The partial text to match.
 * @param state Readline state (0 for first call).
 * @return Allocated string of the match or NULL.
 */
static char* command_generator(const char* text, int state) {
    static int list_index, len;
    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    const char* name;
    while ((name = special_cmds[list_index++])) {
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }
    return NULL;
}

/**
 * @brief Dispatches completion requests to the command generator if triggers are detected.
 *
 * Trigger characters are @c '!' and @c '*' at the start of the current word.
 * This entry point deliberately does @b not set
 * @c rl_attempted_completion_over; when no trigger is present it returns
 * @c nullptr (not an empty array) so Readline falls back to its default
 * filename completion. This is the opposite of
 * @c setting_value_completion_entry(), which suppresses the default.
 *
 * @param text The partial text.
 * @param start Start index in line buffer (unused).
 * @param end End index in line buffer (unused).
 * @return Array of matches or nullptr.
 */
char** my_special_completion_entry(const char* text, int start, int end) {
    (void)start;
    (void)end;

    if (text[0] == '!' || text[0] == '*') {
        return rl_completion_matches(text, command_generator);
    }

    return nullptr;
}

/**
 * @brief Rebinds Esc to @c clear_list_and_screen while a completion list is shown.
 *
 * A first Esc after a completion list clears/repaints, and a second Esc
 * exits. This applies to every prompt that uses @c my_rl_complete, including
 * the Settings Editor's value prompt. @c clear_list_and_screen drops the
 * repaint snapshot before the screen is redrawn, so the Settings Editor's
 * repaint callback cannot redraw the list that is being dismissed.
 */
static void install_esc_hijack() {
    rl_bind_keyseq("\\e", clear_list_and_screen);
}

/**
 * @brief Rebinds Esc back to @c exit_handler.
 */
static void restore_esc_default() {
    rl_bind_keyseq("\\e", exit_handler);
}

/**
 * @brief True while @c customListingsFunction is displaying a completion
 *        list for the current @c my_rl_complete() call.
 *
 * If Readline inserts a common prefix while also displaying matches, the
 * subsequent @c clear_screen_and_buffer() call restores the default Esc
 * binding and, because this flag is set, preserves the repaint snapshot.
 * @c my_rl_complete() uses this flag to re-install the Esc hijack so the
 * first Esc still clears the list and the second exits.
 */
static bool list_shown_during_completion = false;

//=============================================================================
// Completion list display (helpers for customListingsFunction and
// repaintCompletionListing)
//=============================================================================

namespace {

    enum class CompletionKind { Special = 0, SettingValue = 1, File = 2 };

    struct PageInfo {
        int total_pages;
        int start_index;
        int items_to_display;
    };

    struct DisplayItem {
        std::string display_text;   // truncated, colored string ready for printing
        int visual_length;          // length without ANSI codes
        const char* raw_match;      // original match
    };

    struct FormatContext {
        CompletionKind kind;
        const ReadlineColors& rc;
        const char* base_color;     // dynamic color used for setting values
        int base_len;               // length of the common directory part (file branch only)
    };

    struct ColumnLayout {
        int column_width;
        int num_columns;
        int total_column_width;
        int rows;
    };

    struct CompletionSnapshot {
        std::vector<std::string> matches;

        CompletionKind kind = CompletionKind::File;

        int num_matches = 0;
        int displayed_page = 0;
        int base_len = 0;
        int hook_cursor_row = 0;    // 1-based terminal row where Readline invoked the completion hook

        bool valid = false;

        void clear() {
            matches.clear();
            num_matches = 0;
            displayed_page = 0;
            base_len = 0;
            hook_cursor_row = 0;
            valid = false;
        }

        void capture(
            char** source,
            int count,
            CompletionKind completion_kind,
            int page,
            int base_length,
            int cursor_row) {

            clear();

            matches.reserve(count + 1);

            for (int i = 0; i <= count; ++i)
                matches.emplace_back(source[i] ? source[i] : "");

            kind = completion_kind;
            num_matches = count;
            displayed_page = page;
            base_len = base_length;
            hook_cursor_row = cursor_row;

            valid = true;
        }
    };

    CompletionSnapshot lastCompletion;

    constexpr int kColumnSpacing = 4;
    constexpr int kMaxColumns = 4;
    constexpr int kProbeWidth = 1000;   // "unlimited" width used for the measuring pass

    // ---------------------------------------------------------------------------
    // Completion kind / pagination
    // ---------------------------------------------------------------------------

    // Characters that mark a match as a special (command-style) entry for
    // display purposes. Note: '?' is recognized here for display/coloring only;
    // my_special_completion_entry() only generates matches for '!' and '*'.
    bool isSpecialPrefix(char c) {
        return c != '\0' && strchr("!?*", c) != nullptr;
    }

    const char* specialColor(char c, const ReadlineColors& rc) {
        if (!isSpecialPrefix(c)) return rc.reset;
        return (c == '!') ? rc.warning : rc.accent;
    }

    CompletionKind detectCompletionKind(char** matches, int num_matches) {
        if (num_matches > 0 && matches[1] && isSpecialPrefix(matches[1][0]))
            return CompletionKind::Special;
        if (g_activeSettingValues != nullptr && !g_activeSettingValues->empty())
            return CompletionKind::SettingValue;
        return CompletionKind::File;
    }

    void updatePaginationState(const char* current_prefix) {
        if (strcmp(last_common_prefix, current_prefix) == 0) return;
        current_page = 0;
        strncpy(last_common_prefix, current_prefix, sizeof(last_common_prefix) - 1);
        last_common_prefix[sizeof(last_common_prefix) - 1] = '\0';
    }

    bool needsPagination(int num_matches) {
        return GlobalState::ITEMS_PER_PAGE > 0 && (size_t)num_matches > GlobalState::ITEMS_PER_PAGE;
    }

    PageInfo computePageInfo(int num_matches, int page_index) {
        PageInfo page{1, 1, num_matches};

        if (GlobalState::ITEMS_PER_PAGE <= 0)
            return page;

        page.total_pages =
            ((size_t)num_matches + GlobalState::ITEMS_PER_PAGE - 1) /
            GlobalState::ITEMS_PER_PAGE;

        if (page_index >= page.total_pages)
            page_index = 0;

        page.start_index =
            page_index * (int)GlobalState::ITEMS_PER_PAGE + 1;

        int remaining = num_matches - (page.start_index - 1);

        page.items_to_display =
            std::min(remaining, (int)GlobalState::ITEMS_PER_PAGE);

        return page;
    }

    void advancePage(int num_matches, int total_pages) {
        if (!needsPagination(num_matches)) return;
        current_page++;
        if (current_page >= total_pages) current_page = 0;
    }

    // ---------------------------------------------------------------------------
    // Header / footer
    // ---------------------------------------------------------------------------

    void printHeader(
        CompletionKind kind,
        int total_pages,
        int num_matches,
        int page_index,
        const ReadlineColors& rc) {

        if (num_matches <= 1) return;

        static const char* const kLabels[] = {
            "CMD Completion Matches",
            "Value Completion Matches",
            "Tab Completion Matches"
        };

        const char* label = kLabels[(int)kind];

        if (total_pages > 1) {
            printf("\n%s%s [page %d/%d%s]:%s\n\n",
                   rc.label, label,
                   page_index + 1, total_pages,
                   rc.label, rc.reset);
        } else {
            printf("\n%s%s:%s\n\n",
                   rc.label, label, rc.reset);
        }
    }

    void printFooter(const PageInfo& page, int num_matches, const ReadlineColors& rc) {
        if (!needsPagination(num_matches)) return;
        printf("\n%s[%d/%d matches — press %sTab%s for next page]%s\n",
               rc.label, page.start_index + page.items_to_display - 1, num_matches,
               rc.hint, rc.label, rc.reset);
    }

    // ---------------------------------------------------------------------------
    // Item formatting
    // ---------------------------------------------------------------------------

    std::string smartTruncate(const char* str, int max_width) {
        if (!str || max_width <= 0)
            return {};

        std::string result(str);

        const size_t width = static_cast<size_t>(max_width);

        if (result.length() <= width)
            return result;

        if (max_width <= 3)
            return result.substr(0, width);

        // Try to preserve a reasonably short file extension.
        const size_t dot_pos = result.find_last_of('.');

        if (dot_pos != std::string::npos && dot_pos > 0) {
            const size_t ext_len = result.length() - dot_pos;

            if (ext_len + 4 <= width) {
                const size_t prefix_len = width - ext_len - 3;

                return result.substr(0, prefix_len) +
                       "..." +
                       result.substr(dot_pos);
            }
        }

        // General truncation, preserving both the beginning and the end.
        const int body_width = max_width - 3;
        const int prefix_len = (body_width + 1) / 2;
        const int suffix_len = body_width - prefix_len;

        return result.substr(0, prefix_len) +
               "..." +
               (suffix_len > 0
                    ? result.substr(result.length() - suffix_len)
                    : std::string{});
    }

    bool isDirectory(const char* path) {
        struct stat path_stat;
        return (stat(path, &path_stat) == 0) && S_ISDIR(path_stat.st_mode);
    }

    int computeBaseLen(const char* base_path) {
        const char* last_slash = strrchr(base_path, '/');
        return (last_slash != NULL) ? (int)(last_slash - base_path + 1) : 0;
    }

    DisplayItem makeItem(const char* raw, const char* text, const char* color,
                         const char* suffix, int width, const ReadlineColors& rc) {
        std::string truncated = smartTruncate(text, width);
        int vis_len = (int)truncated.length() + (int)strlen(suffix);
        std::string display = std::string(color) + truncated + suffix + rc.reset;
        return {display, vis_len, raw};
    }

    DisplayItem formatItem(const char* raw, const FormatContext& ctx, int width) {
        const ReadlineColors& rc = ctx.rc;

        if (ctx.kind == CompletionKind::Special)
            return makeItem(raw, raw, specialColor(raw[0], rc), "", width, rc);

        if (ctx.kind == CompletionKind::SettingValue)
            return makeItem(raw, raw, ctx.base_color, "", width, rc);

        const char* relative = raw + ctx.base_len;

        if (isDirectory(raw)) {
            const bool show_slash = width >= 2;
            const char* suffix = show_slash ? "/" : "";
            return makeItem(raw, relative, rc.dir, suffix, width - (show_slash ? 1 : 0), rc);
        }

        return makeItem(raw, relative, rc.file, "", width, rc);
    }

    std::vector<DisplayItem> formatPageItems(char** matches, const PageInfo& page, const FormatContext& ctx) {
        std::vector<DisplayItem> items;
        items.reserve(page.items_to_display);
        for (int i = 0; i < page.items_to_display; ++i)
            items.push_back(formatItem(
                matches[page.start_index + i],
                ctx,
                kProbeWidth));
        return items;
    }

    size_t maxVisualLength(const std::vector<DisplayItem>& items) {
        size_t max_len = 0;
        for (const auto& item : items)
            max_len = std::max(max_len, (size_t)item.visual_length);
        return max_len;
    }

    void refitItems(std::vector<DisplayItem>& items, const FormatContext& ctx, int column_width) {
        for (auto& item : items)
            item = formatItem(item.raw_match, ctx, column_width);
    }

    // ---------------------------------------------------------------------------
    // Layout / rendering
    // ---------------------------------------------------------------------------

    int fdColumns(int fd) {
        struct winsize w = {};
        return (ioctl(fd, TIOCGWINSZ, &w) == -1) ? 0 : w.ws_col;
    }

    int getTerminalWidth() {
        int cols = fdColumns(STDOUT_FILENO);
        if (cols == 0) cols = fdColumns(STDERR_FILENO);
        return (cols == 0) ? 80 : cols;
    }

    int fdRows(int fd) {
        struct winsize w = {};
        return (ioctl(fd, TIOCGWINSZ, &w) == -1) ? 0 : w.ws_row;
    }

    int getTerminalRows() {
        int rows = fdRows(STDOUT_FILENO);
        if (rows == 0) rows = fdRows(STDERR_FILENO);
        return (rows == 0) ? 24 : rows;
    }

    // Parses a completed ANSI Cursor Position Report and returns its 1-based row.
    // Returns 0 if the response is malformed.
    int parseCursorRow(const char* buf) {
        int parsed_row = 0, parsed_col = 0;
        char term_ch = 0;

        const char* esc = strstr(buf, "\033[");

        if (!esc ||
            sscanf(esc + 2, "%d;%d%c", &parsed_row, &parsed_col, &term_ch) != 3 ||
            term_ch != 'R' ||
            parsed_row <= 0 ||
            parsed_col <= 0) {
            return 0;
        }

        return parsed_row;
    }

    // Drains trailing CPR response bytes after a timeout.
    void drainCursorResponse(int tty_fd) {
        for (int i = 0; i < 5; ++i) {
            struct pollfd pfd{};
            pfd.fd = tty_fd;
            pfd.events = POLLIN;

            if (poll(&pfd, 1, 20) <= 0) return;

            char ch = 0;
            if (read(tty_fd, &ch, 1) <= 0 || ch == 'R') return;
        }
    }

    // Queries the 1-based cursor row via ANSI Cursor Position Report (ESC [ 6 n).
    // Returns 0 if the terminal is non-interactive, unsupported, or times out.
    int queryCursorRow() {
        if (!isatty(STDOUT_FILENO)) return 0;

        int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty_fd < 0) return 0;

        struct termios orig_termios{}, raw_termios{};
        bool have_termios = (tcgetattr(tty_fd, &orig_termios) == 0);

        if (have_termios) {
            raw_termios = orig_termios;
            raw_termios.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw_termios.c_cc[VMIN] = 0;
            raw_termios.c_cc[VTIME] = 0;
            tcsetattr(tty_fd, TCSANOW, &raw_termios);
        }

        fflush(stdout);

        const char req[] = "\033[6n";
        ssize_t written = write(tty_fd, req, sizeof(req) - 1);

        int row = 0;
        bool saw_cpr_terminator = false;

        if (written == static_cast<ssize_t>(sizeof(req) - 1)) {
            using clock = std::chrono::steady_clock;
            const auto deadline = clock::now() + std::chrono::milliseconds(100);

            char buf[64];
            size_t pos = 0;

            while (pos + 1 < sizeof(buf)) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - clock::now());

                if (remaining.count() <= 0) break;

                struct pollfd pfd{};
                pfd.fd = tty_fd;
                pfd.events = POLLIN;

                int pr = poll(&pfd, 1, static_cast<int>(remaining.count()));
                if (pr <= 0) break;

                if (read(tty_fd, buf + pos, 1) <= 0) break;

                if (buf[pos] == 'R') {
                    buf[pos + 1] = '\0';
                    saw_cpr_terminator = true;
                    row = parseCursorRow(buf);
                    break;
                }

                ++pos;
            }

            // If a CPR sequence started ("ESC [") but timed out before 'R',
            // drain any trailing bytes up to 'R' so a late tail doesn't leak.
            if (!saw_cpr_terminator &&
                pos > 0 &&
                strstr(buf, "\033[") != nullptr) {
                drainCursorResponse(tty_fd);
            }
        }

        if (have_termios) {
            tcsetattr(tty_fd, TCSANOW, &orig_termios);
        }

        close(tty_fd);
        return row;
    }

    // Computes the maximum number of grid rows that fit strictly below cursor_row
    // without exceeding term_rows. Returns -1 when unconstrained.
    int maxGridRowsBelowCursor(int cursor_row, int term_rows, int num_matches, bool show_footer) {
        if (cursor_row <= 0 || term_rows <= 0) return -1;

        const int lines_below_cursor = std::max(0, term_rows - cursor_row);
        int overhead = 1; // opening '\n' in renderCompletionListing
        if (num_matches > 1) overhead += 3; // printHeader: "\n<label>:\n\n"
        if (show_footer) overhead += 2;     // printFooter: "\n[...]\n"

        return std::max(0, lines_below_cursor - overhead);
    }

    ColumnLayout computeLayout(size_t max_item_length, int items_to_display, int max_grid_rows) {
        ColumnLayout layout{};

        const int terminal_width = std::max(1, getTerminalWidth());

        // Leave the final terminal column unused to avoid automatic wrapping
        // when the last printed character reaches the right edge.
        const int usable_width = std::max(1, terminal_width - 1);

        const int desired_width =
            std::max(8, static_cast<int>(max_item_length) + 2);

        // Never allow a completion column to exceed the available width.
        layout.column_width = std::min(desired_width, usable_width);

        const int fit = std::max(
            1,
            (usable_width + kColumnSpacing) /
                (layout.column_width + kColumnSpacing));

        layout.num_columns =
            (items_to_display <= 4)
                ? 1
                : std::min(fit, kMaxColumns);

        layout.total_column_width =
            layout.column_width + kColumnSpacing;

        layout.rows =
            (items_to_display + layout.num_columns - 1) /
            layout.num_columns;

        // Vertical constraint: never allow the grid to exceed the rows available
        // below the cursor position where Readline invoked the completion hook.
        if (max_grid_rows >= 0 && layout.rows > max_grid_rows) {
            layout.rows = max_grid_rows;
        }

        return layout;
    }

    void printCell(const DisplayItem& item, bool pad, int total_column_width) {
        printf("%s", item.display_text.c_str());
        if (pad) printf("%*s", std::max(0, total_column_width - item.visual_length), "");
    }

    class AutoWrapGuard {
    public:
        AutoWrapGuard() {
            // Prevent a mid-render terminal resize from causing automatic wrapping.
            printf("\033[?7l");
            fflush(stdout);
        }
        ~AutoWrapGuard() {
            // Restore the normal terminal auto-wrap mode.
            printf("\033[?7h");
            fflush(stdout);
        }
        AutoWrapGuard(const AutoWrapGuard&) = delete;
        AutoWrapGuard& operator=(const AutoWrapGuard&) = delete;
    };

    void renderGrid(const std::vector<DisplayItem>& items, const ColumnLayout& layout) {
        if (layout.rows <= 0) return;

        const int visible_count =
            std::min((int)items.size(), layout.rows * layout.num_columns);

        for (int row = 0; row < layout.rows; row++) {
            for (int col = 0; col < layout.num_columns; col++) {
                int page_offset = row + col * layout.rows;
                if (page_offset >= visible_count) continue;

                // Pad only when not in the last column and not the last visible item
                bool pad = col < layout.num_columns - 1 && page_offset < visible_count - 1;
                printCell(items[page_offset], pad, layout.total_column_width);
            }

            // Explicitly return to column 1 before advancing to the next row.
            printf("\033[0m\r\n");
        }
    }

    void renderCompletionListing(
        char** matches,
        int num_matches,
        CompletionKind kind,
        int displayed_page,
        int base_len,
        int hook_cursor_row) {

        ReadlineColors rc = resolveReadlineTheme();

        const PageInfo page =
            computePageInfo(num_matches, displayed_page);

        const int term_rows = getTerminalRows();
        const int effective_cursor_row =
            (hook_cursor_row > 0) ? std::min(hook_cursor_row, term_rows) : 0;
        const bool show_footer = needsPagination(num_matches);
        const int max_grid_rows =
            maxGridRowsBelowCursor(effective_cursor_row, term_rows, num_matches, show_footer);

        // If there are no rows below Readline's cursor, do not render.
        if (effective_cursor_row > 0 && term_rows <= effective_cursor_row) {
            fflush(stdout);
            return;
        }

        const FormatContext ctx{
            kind,
            rc,
            UI::Palette::BoldReset.c_str(),
            base_len
        };

        std::vector<DisplayItem> items =
            formatPageItems(matches, page, ctx);

        const ColumnLayout layout =
            computeLayout(maxVisualLength(items), page.items_to_display, max_grid_rows);

        refitItems(items, ctx, layout.column_width);

        {
            // Prevent automatic wrapping during rendering if the terminal is resized.
            AutoWrapGuard auto_wrap_guard;

            printf("\033[s\033[J\r\n");

            int lines_remaining =
                (effective_cursor_row > 0)
                    ? std::max(0, (term_rows - effective_cursor_row) - 1)
                    : -1;

            const bool can_print_header =
                (num_matches > 1) && (lines_remaining < 0 || lines_remaining >= 3);
            if (can_print_header) {
                printHeader(
                    kind,
                    page.total_pages,
                    num_matches,
                    displayed_page,
                    rc);
                if (lines_remaining >= 0) lines_remaining -= 3;
            }

            renderGrid(items, layout);

            if (lines_remaining >= 0) {
                lines_remaining = std::max(0, lines_remaining - layout.rows);
            }

            if (show_footer && (lines_remaining < 0 || lines_remaining >= 2)) {
                printFooter(page, num_matches, rc);
            }

            printf("\033[u");
            fflush(stdout);
        }
    }

} // namespace

/**
 * @brief Repaints the last completion listing after a terminal resize.
 *
 * Uses an owned snapshot of the last displayed completion matches.
 * Does not advance pagination or modify the current pagination state.
 *
 * The snapshot remains valid independently of subsequent input-buffer
 * modifications or cursor movement. It is invalidated by a new completion
 * attempt, an explicit screen clear, or a prompt-state reset.
 *
 * @note The listing saves/restores an absolute cursor position
 *       (@c ESC[s / @c ESC[u), so callers must invoke this with the cursor on the
 *       prompt's input line (see the repaint callbacks).
 */
void repaintCompletionListing() {
    if (!lastCompletion.valid)
        return;

    // Re-query the current cursor row on repaint if available, falling back
    // to the row captured when Readline originally invoked the hook.
    const int current_cursor_row = queryCursorRow();
    const int cursor_row =
        (current_cursor_row > 0) ? current_cursor_row : lastCompletion.hook_cursor_row;

    std::vector<char*> matches;
    matches.reserve(lastCompletion.matches.size());

    for (auto& match : lastCompletion.matches)
        matches.push_back(match.data());

    renderCompletionListing(
        matches.data(),
        lastCompletion.num_matches,
        lastCompletion.kind,
        lastCompletion.displayed_page,
        lastCompletion.base_len,
        cursor_row);
}

/**
 * @brief Custom hook for GNU Readline to display completions with pagination and color themes.
 *
 * @param matches  Array of completion matches in Readline format: matches[0] is the
 *                 common prefix of all completions (used for pagination state tracking,
 *                 not displayed), and matches[1..num_matches] are the actual completion
 *                 candidates. Note that matches[1] is interpreted as a filesystem path
 *                 prefix only in the file/directory completion branch; for special
 *                 command and settings-value completions it is simply the first
 *                 candidate string.
 * @param num_matches  Total number of completion candidates (excluding matches[0]).
 * @param max_length   Length of the longest match string as provided by Readline.
 *                     Unused by this implementation — column widths are computed
 *                     directly from the match array.
 */
void customListingsFunction(char **matches, int num_matches, int max_length) {
    (void)max_length;

    // Capture the cursor position right where Readline invoked the completion hook,
    // before any output is emitted, to enforce the vertical viewport constraint.
    const int hook_cursor_row = queryCursorRow();

    list_shown_during_completion = true;
    install_esc_hijack();

    const CompletionKind kind = detectCompletionKind(matches, num_matches);

    updatePaginationState(matches[0]);

    const int displayed_page = current_page;

    const int base_len =
        (kind == CompletionKind::File)
            ? computeBaseLen(matches[1])
            : 0;

    // Preserve the actual displayed matches and page for resize repainting.
    lastCompletion.capture(
        matches,
        num_matches,
        kind,
        displayed_page,
        base_len,
        hook_cursor_row);

    renderCompletionListing(
        matches,
        num_matches,
        kind,
        displayed_page,
        base_len,
        hook_cursor_row);

    // Preserve existing Tab-to-next-page behavior.
    const PageInfo page =
        computePageInfo(num_matches, displayed_page);

    advancePage(num_matches, page.total_pages);
}

CompleterData g_completerData = {nullptr, nullptr};

//=============================================================================
// Settings-Value Tab Completion
//=============================================================================

/**
 * @brief Generates matches for the active setting's valid values.
 *
 * Iterates over the candidate list in @c g_activeSettingValues, comparing each
 * entry against @p text by prefix. The append character is set to a single
 * space so accepted matches remain editable. Mirrors @c command_generator's
 * static-index contract (state == 0 restarts iteration).
 */
static char* setting_value_generator(const char* text, int state) {
    static size_t idx;
    static size_t len;
    if (!state) {
        idx = 0;
        len = strlen(text);
        rl_completion_append_character = ' ';
    }
    if (!g_activeSettingValues) return nullptr;
    while (idx < g_activeSettingValues->size()) {
        const std::string& v = (*g_activeSettingValues)[idx++];
        if (v.compare(0, len, text) == 0) {
            return strdup(v.c_str());
        }
    }
    return nullptr;
}

/**
 * @brief Completion entry for the Settings Editor.
 *
 * Mirrors @c my_special_completion_entry in shape, but differs in two
 * deliberate ways: it sets @c rl_attempted_completion_over = 1 (so Readline
 * does not fall back to filename completion on this prompt) and forces the
 * append character to a single space. When matches exist, Readline hands the
 * list to @c customListingsFunction (installed as
 * @c rl_completion_display_matches_hook by @c restoreReadline), so pagination,
 * colors, column layout, resize repaints and the Esc handling (first Esc
 * clears the list, second Esc exits) all behave exactly like the search prompt.
 *
 * @note The candidate list @c g_activeSettingValues is populated/cleared by
 *       the Settings Editor (see @c editSetting / the RAII guard in
 *       settingsEditor.cpp); this function only consumes it.
 */
char** setting_value_completion_entry(const char* text, int /*start*/, int /*end*/) {
    if (!g_activeSettingValues || g_activeSettingValues->empty()) return nullptr;
    rl_attempted_completion_over = 1;
    rl_completion_append_character = ' ';
    return rl_completion_matches(text, setting_value_generator);
}

/**
 * @brief Handles tab-completion for ISO images (numeric indices with '>' separator).
 * @param text The partial text entered by the user.
 * @param start Start index in the Readline line buffer.
 * @param full_line The complete text of the Readline line buffer.
 * @return Allocated array of string matches, or nullptr if no completions exist.
 */
static char** handle_iso_completion(const char* text, int start, const std::string& full_line) {
    if (!g_completerData.sortedIsos) {
        return nullptr;
    }

    std::vector<std::string> possible_completions;
    size_t list_index = 0;
    while (list_index < g_completerData.sortedIsos->size()) {
        const std::string opt = std::to_string(++list_index) + ">";
        if (opt.find(text) == 0) {
            possible_completions.push_back(opt);
        }
    }

    // Check if candidate completions are already present at the current word's position
    bool all_present = true;
    for (const auto& comp : possible_completions) {
        if (full_line.compare(start, comp.size(), comp) != 0) {
            all_present = false;
            break;
        }
    }

    if (all_present || possible_completions.empty()) {
        return nullptr;
    }

    return rl_completion_matches(text, [](const char* text, int state) -> char* {
        static size_t list_index;
        if (!state) {
            list_index = 0;
            rl_completion_append_character = '\0';
        }
        while (list_index < g_completerData.sortedIsos->size()) {
            const std::string opt = std::to_string(++list_index) + ">";
            if (opt.find(text) == 0)
                return strdup(opt.c_str());
        }
        return nullptr;
    });
}

/**
 * @brief Handles tab-completion for USB target devices following the '>' separator.
 * @param text The partial text entered by the user.
 * @param start Start index in the Readline line buffer.
 * @param full_line The complete text of the Readline line buffer.
 * @return Allocated array of string matches, or nullptr if no completions exist.
 */
static char** handle_device_completion(const char* text, int start, const std::string& full_line) {
    if (!g_completerData.usbDevices) {
        return nullptr;
    }

    std::string fullText(text);
    size_t pos = fullText.find_last_of('>');
    std::string prefix, deviceSubText;
    if (pos != std::string::npos) {
        prefix = fullText.substr(0, pos + 1);
        deviceSubText = fullText.substr(pos + 1);
    } else {
        deviceSubText = fullText;
    }

    std::vector<std::string> possible_device_completions;
    for (size_t i = 0; i < g_completerData.usbDevices->size(); i++) {
        const std::string& dev = (*g_completerData.usbDevices)[i];
        if (dev.find(deviceSubText) == 0) {
            std::string completion = prefix + dev;
            possible_device_completions.push_back(completion);
        }
    }

    // Compare against line at current word's start position
    bool all_present = true;
    for (const auto& comp : possible_device_completions) {
        if (full_line.compare(start, comp.size(), comp) != 0) {
            all_present = false;
            break;
        }
    }

    if (all_present || possible_device_completions.empty()) {
        return nullptr;
    }

    // Static variables to preserve exact lambda state/capture behavior
    static std::string s_prefix;
    static std::string s_deviceSubText;
    s_prefix = prefix;
    s_deviceSubText = deviceSubText;

    rl_completion_append_character = '\0';
    return rl_completion_matches(fullText.c_str(), [](const char* /*unused*/, int state) -> char* {
        static size_t list_index;
        if (!state) {
            list_index = 0;
        }
        while (list_index < g_completerData.usbDevices->size()) {
            const std::string& dev = (*g_completerData.usbDevices)[list_index++];
            if (dev.find(s_deviceSubText) == 0) {
                std::string completion = s_prefix + dev;
                return strdup(completion.c_str());
            }
        }
        return nullptr;
    });
}

/**
 * @brief Callback for Readline to handle ISO-to-USB mapping completions.
 *
 * Resets pagination state and the completion repaint snapshot inline (the same
 * resets @c resetReadlinePagination performs), suppresses Readline's default
 * filename completion, then dispatches to the ISO-index or USB-device handler
 * depending on whether the current word contains a '>' separator.
 *
 * @param text The partial text currently being completed.
 * @param start Start index of the current word in the line buffer.
 * @param end End index of the current word in the line buffer.
 * @return Array of matched completion strings or nullptr.
 */
char** completion_cb(const char* text, int start, int end) {
    current_page = 0;
    last_common_prefix[0] = '\0';
    lastCompletion.clear();

    rl_attempted_completion_over = 1;

    std::string current_word(rl_line_buffer + start, end - start);
    std::string full_line(rl_line_buffer);

    bool is_device_completion = (current_word.find('>') != std::string::npos);

    if (!is_device_completion) {
        return handle_iso_completion(text, start, full_line);
    } else {
        return handle_device_completion(text, start, full_line);
    }
}

/**
 * @brief Resets the completion-list pagination state and clears the saved repaint snapshot.
 *
 * Clears the current page counter, the cached common prefix used to
 * detect the start of a new completion session, and the saved completion
 * repaint snapshot (@c lastCompletion).
 *
 * This is the callable form of a reset that is also performed inline elsewhere
 * in this file: @c completion_cb resets all three at the start of each
 * completion attempt, @c my_rl_complete clears the snapshot at the start of each
 * attempt, and @c clear_screen_and_buffer resets the page and prefix (and the
 * snapshot, unless a list was just shown during completion). External callers,
 * such as the Settings Editor's per-setting loop, call this function so a
 * snapshot from a previous prompt can never be repainted on a different screen.
 */
void resetReadlinePagination() {
    current_page = 0;
    last_common_prefix[0] = '\0';
    lastCompletion.clear();
}

/**
 * @brief Dummy function to intercept and disable specific keypresses in Readline.
 * @return Always 0.
 */
int prevent_readline_keybindings(int, int) {
    return 0;
}

/**
 * @brief Clears the terminal screen, redraws the prompt, resets pagination state,
 *        and (usually) clears the saved completion repaint snapshot.
 *
 * If a Settings Editor body-repaint hook is installed
 * (@c SettingsEditorRepaint::g_fn), the editor's own draw lambda is invoked
 * instead of @c clearScrollBuffer so the header/value/description/footer are
 * repainted along with the prompt. Otherwise the scrollback is cleared. Readline
 * is then asked to redraw, the page counter and cached common prefix are reset,
 * and the default Esc binding is restored via @c restore_esc_default.
 *
 * The completion snapshot (@c lastCompletion) is cleared only when
 * @c list_shown_during_completion is false. When @c my_rl_complete calls this
 * function after Readline inserted a common prefix and also displayed a list,
 * the snapshot is deliberately preserved so the repaint hook can redraw the list
 * and later terminal resizes can repaint it. @c my_rl_complete re-installs the
 * Esc hijack right after this returns.
 *
 * @note The snapshot is cleared (when it is) only AFTER the repaint hook runs.
 *       A caller that wants a visible completion list dismissed (Esc, or Ctrl+L
 *       in the Settings Editor) must drop the snapshot first; use
 *       @c clear_list_and_screen for that.
 *
 * When @c RetainAndRestoreReadlineBuffer::g_rl_complete_mode is active, the
 * current input line is captured, Readline is forced to exit its loop, and the
 * completion mode flag is cleared so a subsequent loop iteration can restore
 * the buffer cleanly.
 *
 * @param ignore1 Unused parameter (typically 0).
 * @param ignore2 Unused parameter (typically 0).
 * @return Always 0.
 */
int clear_screen_and_buffer(int, int) {
    if (SettingsEditorRepaint::g_fn) {
        SettingsEditorRepaint::g_fn();
        std::cout << "\n";
    } else {
        clearScrollBuffer();
    }
    fflush(stdout);
    rl_forced_update_display();

    current_page = 0;
    last_common_prefix[0] = '\0';

    // Preserve the snapshot when my_rl_complete() is refreshing a listing
    // after Readline inserted a common prefix. It is needed for future
    // terminal-resize repaints.
    //
    // Ordinary screen clears invalidate the snapshot. Explicit dismissal
    // clears it before reaching this function via clear_list_and_screen().
    if (!list_shown_during_completion)
        lastCompletion.clear();

    // Always restore the default Esc binding when clearing. If a completion
    // list is currently visible, my_rl_complete() re-installs the hijack
    // right after this returns (tracked by list_shown_during_completion), so
    // the first Esc still clears the list and the second exits.
    restore_esc_default();

    char *new_text = rl_copy_text(0, rl_end);

    if (RetainAndRestoreReadlineBuffer::g_rl_complete_mode == 1) {
        RetainAndRestoreReadlineBuffer::g_rl_pending_text = new_text;
        rl_on_new_line();
        rl_replace_line("", 0);
        rl_done = 1;
        RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;
    }

    free(new_text);
    return 0;
}

/**
 * @brief Clears the screen and dismisses any visible completion list.
 *
 * Drops the repaint snapshot first, then delegates to
 * @c clear_screen_and_buffer. Without this, the Settings Editor's repaint
 * callback (invoked by @c clear_screen_and_buffer via
 * @c SettingsEditorRepaint::g_fn) would call @c repaintCompletionListing()
 * while the snapshot is still valid and redraw the very list being dismissed.
 *
 * Bound to Esc while a list is shown (see @c install_esc_hijack) and to
 * Ctrl+L in the Settings Editor's value prompt. The search prompt keeps
 * @c clear_screen_and_buffer on Ctrl+L, since its repaint path never goes
 * through @c g_fn.
 *
 * @param a Unused numeric argument, forwarded.
 * @param b Unused invoking key, forwarded.
 * @return Always 0.
 */
int clear_list_and_screen(int a, int b) {
    lastCompletion.clear();
    return clear_screen_and_buffer(a, b);
}

/**
 * @brief TAB-completion handler that extends @c rl_complete with directory
 * slash handling, completion-list state management, and screen repaint.
 *
 * Delegates to @c rl_complete_internal('?') on double-TAB (list matches)
 * or @c rl_complete_internal('!') on single TAB (insert/complete). At the start
 * of each attempt it clears the completion repaint snapshot and restores the
 * default Esc binding. After completion, if the input changed and the result is
 * a directory, it appends a trailing slash.
 *
 * It then repaints via @c clear_screen_and_buffer (which uses the Settings
 * Editor's repaint hook when one is installed, and clears the scroll buffer
 * otherwise) in two cases: when the input changed, or when a previously
 * displayed listing needs dismissing because this attempt did not display a new
 * one, even if the input is unchanged. The repaint also resets the page counter
 * and cached common prefix.
 *
 * When a completion list is displayed, Esc is rebound to
 * @c clear_list_and_screen (see @c install_esc_hijack): the first Esc
 * clears/repaints the list and restores the default binding, and a second Esc
 * exits. This applies to every prompt using this handler, including the
 * Settings Editor value prompt. If a repaint follows a displayed list (e.g. a
 * common prefix was inserted), the hijack is re-installed afterwards.
 *
 * @note When @c RetainAndRestoreReadlineBuffer::g_rl_complete_mode is 1, the
 *       repaint path (@c clear_screen_and_buffer) stashes the current input in
 *       @c g_rl_pending_text, clears the buffer and sets @c rl_done so the next
 *       loop iteration can restore the text. This function does not handle that
 *       mode itself.
 *
 * @param ignore       Numeric argument (unused).
 * @param invoking_key Triggering key (unused).
 * @return Return value of @c rl_complete_internal.
 */
int my_rl_complete(int ignore, int invoking_key)
{
    (void)ignore;
    (void)invoking_key;

    const bool had_previous_listing = lastCompletion.valid;

    // Start a fresh completion attempt.
    list_shown_during_completion = false;
    lastCompletion.clear();
    restore_esc_default();

    rl_completion_append_character = '\0';

    const int old_point = rl_point;
    char *old_text = rl_copy_text(0, rl_end);

    int ret;
    if (rl_last_func == my_rl_complete)
        ret = rl_complete_internal('?');
    else
        ret = rl_complete_internal('!');

    char *new_text = rl_copy_text(0, rl_end);

    const bool input_changed =
        rl_point != old_point || strcmp(old_text, new_text) != 0;

    // Redraw when completion changes the input, or when a previous
    // listing was dismissed by an attempt that produced no new listing.
    if (input_changed ||
        (had_previous_listing && !list_shown_during_completion)) {

        if (input_changed) {
            struct stat st;
            if (stat(new_text, &st) == 0 && S_ISDIR(st.st_mode)) {
                const size_t len = strlen(new_text);
                if (len > 0 && new_text[len - 1] != '/') {
                    rl_point = static_cast<int>(len);
                    rl_insert_text("/");
                }
            }
        }

        clear_screen_and_buffer(0, 0);

        // The redraw restores the default Esc binding. Reinstate the
        // completion-list binding only if this attempt displayed a list.
        if (list_shown_during_completion)
            install_esc_hijack();
    }

    list_shown_during_completion = false;

    free(old_text);
    free(new_text);
    return ret;
}

/**
 * @brief Restores readline key bindings and re-inserts any pending buffer text.
 *
 * Rebinds readline keys and, if a pending input line was saved in
 * RetainAndRestoreReadlineBuffer::g_rl_pending_text, installs a startup hook
 * that re-inserts it into the readline buffer before the next prompt is
 * displayed. If no pending text exists, the startup hook is cleared.
 *
 * Key bindings set:
 * - `Ctrl+L` (`\f`): Clears/repaints the screen via @p clear_screen_and_buffer.
 *   The input buffer is only cleared (and stashed for restoration) when
 *   @c g_rl_complete_mode is active.
 * - `Tab`    (`\t`): Invokes custom completion via @p my_rl_complete.
 *
 * @note The startup hook consumes @p g_rl_pending_text exactly once,
 *       clearing it after insertion to prevent repeated re-insertion.
 *
 * @see RetainAndRestoreReadlineBuffer::g_rl_pending_text
 * @see clear_screen_and_buffer()
 * @see my_rl_complete()
 */
void RestoreReadlineBuffer() {
    rl_bind_key('\f', clear_screen_and_buffer);
    rl_bind_key('\t', my_rl_complete);

    if (!RetainAndRestoreReadlineBuffer::g_rl_pending_text.empty()) {
        rl_startup_hook = []() -> int {
            if (!RetainAndRestoreReadlineBuffer::g_rl_pending_text.empty()) {
                rl_insert_text(RetainAndRestoreReadlineBuffer::g_rl_pending_text.c_str());
                rl_point = rl_end;
                RetainAndRestoreReadlineBuffer::g_rl_pending_text = "";
            }
            return 0;
        };
    } else {
        rl_startup_hook = nullptr;
    }
}

/**
 * @brief Arms a one-shot readline startup hook that restores stashed input.
 *
 * If @c RetainAndRestoreReadlineBuffer::g_rl_pending_text is non-empty,
 * installs an @c rl_startup_hook that re-inserts it into the buffer before
 * the next prompt is displayed, places the cursor at the end of the text,
 * then clears both the stash and the hook so the text is restored exactly once.
 * If the stash is empty, the function does nothing.
 *
 * Call immediately before @c readline(). Unlike @c RestoreReadlineBuffer(),
 * it does not rebind any keys, so prompts that disable Tab completion and
 * Ctrl+L keep that behavior.
 *
 * @note The stash is filled by @c pgup_handler and @c pgdn_handler, and is
 *       cleared by @c ReadlineKeybindingGuard on scope exit.
 *
 * @see RetainAndRestoreReadlineBuffer::g_rl_pending_text
 * @see RestoreReadlineBuffer()
 */
void armPendingInputRestore() {
    if (RetainAndRestoreReadlineBuffer::g_rl_pending_text.empty()) return;
    rl_startup_hook = []() -> int {
        rl_insert_text(RetainAndRestoreReadlineBuffer::g_rl_pending_text.c_str());
        rl_point = rl_end;
        RetainAndRestoreReadlineBuffer::g_rl_pending_text.clear();
        rl_startup_hook = nullptr;   // one-shot
        return 0;
    };
}
