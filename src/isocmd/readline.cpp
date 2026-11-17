// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

// C / System Headers
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../inputHandling.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../write2usbUI.h"

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
char* command_generator(const char* text, int state) {
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
 * @param start Start index in line buffer.
 * @param end End index in line buffer.
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
 * @brief Rebinds Esc to @c clear_screen_and_buffer while a completion list is shown.
 *
 * A first Esc after a completion list clears/repaints, and a second Esc
 * exits. This applies to every prompt that uses @c my_rl_complete, including
 * the Settings Editor's value prompt.
 */
static void install_esc_hijack() {
    rl_bind_keyseq("\\e", clear_screen_and_buffer);
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
 * binding; @c my_rl_complete() uses this flag to re-install the hijack so
 * the first Esc still clears the list and the second exits.
 */
static bool list_shown_during_completion = false;

//=============================================================================
// Completion list display (helpers for customListingsFunction)
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

constexpr int kColumnSpacing = 4;
constexpr int kMaxColumns = 4;
constexpr int kProbeWidth = 1000;   // "unlimited" width used for the measuring pass

// ---------------------------------------------------------------------------
// Completion kind / pagination
// ---------------------------------------------------------------------------

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

PageInfo computePageInfo(int num_matches) {
    PageInfo page{1, 1, num_matches};
    if (GlobalState::ITEMS_PER_PAGE <= 0) return page;

    page.total_pages = ((size_t)num_matches + GlobalState::ITEMS_PER_PAGE - 1) / GlobalState::ITEMS_PER_PAGE;
    if (current_page >= page.total_pages) current_page = 0;
    page.start_index = current_page * (int)GlobalState::ITEMS_PER_PAGE + 1;
    int remaining = num_matches - (page.start_index - 1);
    page.items_to_display = std::min(remaining, (int)GlobalState::ITEMS_PER_PAGE);
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

void printHeader(CompletionKind kind, int total_pages, int num_matches, const ReadlineColors& rc) {
    if (num_matches <= 1) return;

    static const char* const kLabels[] = {
        "CMD Completion Matches",     // Special
        "Value Completion Matches",   // SettingValue
        "Tab Completion Matches"      // File
    };
    const char* label = kLabels[(int)kind];

    if (total_pages > 1) {
        printf("\n%s%s [page %d/%d%s]:%s\n\n",
               rc.label, label, current_page + 1, total_pages, rc.label, rc.reset);
    } else {
        printf("\n%s%s:%s\n\n", rc.label, label, rc.reset);
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
    std::string result(str);
    size_t len = result.length();
    if (len <= (size_t)max_width) return result;

    // Try to preserve extension for files (harmless for other kinds)
    size_t dot_pos = result.find_last_of('.');
    if (dot_pos != std::string::npos && dot_pos > 0 && len - dot_pos <= 10) {
        std::string ext = result.substr(dot_pos);
        int prefix_len = std::max(3, max_width - (int)ext.length() - 3);
        if (prefix_len >= 3) return result.substr(0, prefix_len) + "..." + ext;
    }
    int prefix_len = (max_width - 3) / 2;
    int suffix_len = max_width - 3 - prefix_len;
    return result.substr(0, prefix_len) + "..." + result.substr(len - suffix_len, suffix_len);
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

// `width` is the maximum text width; directories get `dir_shrink` fewer
// characters because of their trailing '/'.
DisplayItem formatItem(const char* raw, const FormatContext& ctx, int width, int dir_shrink) {
    const ReadlineColors& rc = ctx.rc;

    if (ctx.kind == CompletionKind::Special)
        return makeItem(raw, raw, specialColor(raw[0], rc), "", width, rc);

    if (ctx.kind == CompletionKind::SettingValue)
        return makeItem(raw, raw, ctx.base_color, "", width, rc);

    const char* relative = raw + ctx.base_len;
    if (isDirectory(raw))
        return makeItem(raw, relative, rc.dir, "/", width - dir_shrink, rc);
    return makeItem(raw, relative, rc.file, "", width, rc);
}

std::vector<DisplayItem> formatPageItems(char** matches, const PageInfo& page, const FormatContext& ctx) {
    std::vector<DisplayItem> items;
    items.reserve(page.items_to_display);
    for (int i = 0; i < page.items_to_display; ++i)
        items.push_back(formatItem(matches[page.start_index + i], ctx, kProbeWidth, 0));
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
        item = formatItem(item.raw_match, ctx, column_width, 1);
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

ColumnLayout computeLayout(size_t max_item_length, int items_to_display) {
    ColumnLayout layout;
    layout.column_width = std::max(8, (int)max_item_length + 2);

    int fit = std::max(1, (getTerminalWidth() + kColumnSpacing) / (layout.column_width + kColumnSpacing));
    layout.num_columns = (items_to_display <= 4) ? 1 : std::min(fit, kMaxColumns);
    layout.total_column_width = layout.column_width + kColumnSpacing;
    layout.rows = (items_to_display + layout.num_columns - 1) / layout.num_columns;
    return layout;
}

void printCell(const DisplayItem& item, bool pad, int total_column_width) {
    printf("%s", item.display_text.c_str());
    if (pad) printf("%*s", std::max(0, total_column_width - item.visual_length), "");
}

void renderGrid(const std::vector<DisplayItem>& items, const ColumnLayout& layout) {
    const int count = (int)items.size();
    for (int row = 0; row < layout.rows; row++) {
        for (int col = 0; col < layout.num_columns; col++) {
            int page_offset = row + col * layout.rows;
            if (page_offset >= count) continue;
            // Pad only when not in the last column and not the last item
            bool pad = col < layout.num_columns - 1 && page_offset < count - 1;
            printCell(items[page_offset], pad, layout.total_column_width);
        }
        printf("\033[0m\n");
    }
}

} // namespace

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

    // While a list is visible, the first Esc clears/repaints it and the
    // second Esc exits (same behaviour on every prompt, Settings Editor included).
    list_shown_during_completion = true;
    install_esc_hijack();

    ReadlineColors rc = resolveReadlineTheme();
    const CompletionKind kind = detectCompletionKind(matches, num_matches);

    updatePaginationState(matches[0]);

    printf("\033[s\033[J\n"); // Save cursor and clear

    const PageInfo page = computePageInfo(num_matches);
    printHeader(kind, page.total_pages, num_matches, rc);
    advancePage(num_matches, page.total_pages);

    const FormatContext ctx{
        kind,
        rc,
        UI::Palette::BoldReset.c_str(),
        (kind == CompletionKind::File) ? computeBaseLen(matches[1]) : 0
    };

    // Measure with an effectively unlimited width, lay out columns, then re-fit.
    std::vector<DisplayItem> items = formatPageItems(matches, page, ctx);
    const ColumnLayout layout = computeLayout(maxVisualLength(items), page.items_to_display);
    refitItems(items, ctx, layout.column_width);

    renderGrid(items, layout);
    printFooter(page, num_matches, rc);

    printf("\033[u");
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
 * list to @c customListingsFunction (already installed as
 * @c rl_completion_display_matches_hook), so pagination, colors, column
 * layout, resize repaints and the Esc handling (first Esc clears the list,
 * second Esc exits) all behave exactly like the search prompt.
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
 * @brief Callback for Readline to handle ISO-to-USB mapping completions.
 * @param text The partial text.
 * @param start Start index in line buffer.
 * @param end End index in line buffer.
 * @return Array of matches or nullptr.
 */
char** completion_cb(const char* text, int start, int end) {
    current_page = 0;
    last_common_prefix[0] = '\0';

    rl_attempted_completion_over = 1;
    char** matches = nullptr;
    std::string current_word(rl_line_buffer + start, end - start);

    bool is_device_completion = (current_word.find('>') != std::string::npos);

    if (!is_device_completion) {
        if (g_completerData.sortedIsos) {
            std::vector<std::string> possible_completions;
            size_t list_index = 0;
            while (list_index < g_completerData.sortedIsos->size()) {
                const std::string opt = std::to_string(++list_index) + ">";
                if (opt.find(text) == 0)
                    possible_completions.push_back(opt);
            }

            // Check if the candidate completions are already present at the
            // *current word's position* in the line (not just anywhere in
            // the line, which could falsely match an earlier clause that
            // happens to reuse the same numeric index).
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_completions) {
                if (full_line.compare(start, comp.size(), comp) != 0) {
                    all_present = false;
                    break;
                }
            }

            if (!all_present && !possible_completions.empty()) {
                matches = rl_completion_matches(text, [](const char* text, int state) -> char* {
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
                    return (char*)nullptr;
                });
            }
        }
    }
    else {
        if (g_completerData.usbDevices) {
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

            // Compare against the line at the current word's
            // start position rather than searching the whole line, so an
            // identical prefix used in an earlier clause doesn't suppress
            // completion for the current one.
            bool all_present = true;
            std::string full_line(rl_line_buffer);
            for (const auto& comp : possible_device_completions) {
                if (full_line.compare(start, comp.size(), comp) != 0) {
                    all_present = false;
                    break;
                }
            }

            if (!all_present && !possible_device_completions.empty()) {
                static std::string s_prefix;
                static std::string s_deviceSubText;
                s_prefix = prefix;
                s_deviceSubText = deviceSubText;

                rl_completion_append_character = '\0';
                matches = rl_completion_matches(fullText.c_str(), [](const char* /*unused*/, int state) -> char* {
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
                    return (char*)nullptr;
                });
            }
        }
    }
    return matches;
}

/**
 * @brief Resets the completion-list pagination state.
 *
 * Clears the current page counter and the cached common prefix used to
 * detect the start of a new completion session. Called by
 * @c completion_cb at the start of each completion attempt (inline reset),
 * and after a screen wipe via @c clear_screen_and_buffer and
 * @c my_rl_complete. Does not touch any Settings Editor flag — none exists in
 * this translation unit.
 */
void resetReadlinePagination() {
    current_page = 0;
    last_common_prefix[0] = '\0';
}

/**
 * @brief Re-enables custom completion hooks and history navigation keybindings.
 */
void restoreReadline() {
    rl_completion_display_matches_hook = customListingsFunction;
    rl_attempted_completion_function = nullptr;
    rl_bind_keyseq("\033[A", rl_get_previous_history);
    rl_bind_keyseq("\033[B", rl_get_next_history);
    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
}

/**
 * @brief Disables standard completion and history navigation for confirmation prompts.
 */
void disableReadlineForConfirmation() {
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    rl_bind_keyseq("\033[A", prevent_readline_keybindings);
    rl_bind_keyseq("\033[B", prevent_readline_keybindings);
    rl_bind_keyseq("\\e[5~", prevent_readline_keybindings);
    rl_bind_keyseq("\\e[6~", prevent_readline_keybindings);
}

/**
 * @brief Dummy function to intercept and disable specific keypresses in Readline.
 * @return Always 0.
 */
int prevent_readline_keybindings(int, int) {
    return 0;
}


/**
 * @brief Clears the terminal screen, scrollback buffer, and resets pagination/UI state.
 *
 * If a Settings Editor body-repaint hook is installed
 * (@c SettingsEditorRepaint::g_fn), the editor's own draw lambda is invoked
 * instead of @c clearScrollBuffer so the header/value/description/footer are
 * repainted along with the prompt. Otherwise the scrollback is cleared and
 * Readline is asked to redraw. Pagination state is then reset.
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
 * @brief TAB-completion handler that extends @c rl_complete with pagination reset
 * and custom directory formatting.
 *
 * Delegates to @c rl_complete_internal('?') on double-TAB (list matches)
 * or @c rl_complete_internal('!') on single TAB (insert/complete). After
 * completion, if the result is a directory, it ensures a trailing slash is
 * appended. If the cursor position or line buffer changed, it clears the
 * scroll buffer.
 *
 * When a completion list is displayed, Esc is rebound to
 * @c clear_screen_and_buffer (see @c install_esc_hijack): the first Esc
 * clears/repaints the list and restores the default binding, and a second Esc
 * exits. This applies to every prompt using this handler, including the
 * Settings Editor value prompt.
 *
 * When @c RetainAndRestoreReadlineBuffer::g_rl_complete_mode is 1, it saves the
 * completed text to @c g_rl_pending_text, clears the input buffer, and sets
 * @c rl_done to exit readline, allowing the next loop iteration to restore the
 * text.
 *
 * @param ignore      Numeric argument (unused).
 * @param invoking_key Triggering key (unused).
 * @return Return value of @c rl_complete_internal.
 */
int my_rl_complete(int ignore, int invoking_key)
{
    (void)ignore;
    (void)invoking_key;

    // Reset tracking for this keystroke.
    list_shown_during_completion = false;

    rl_completion_append_character = '\0';

    int old_point = rl_point;
    char *old_text = rl_copy_text(0, rl_end);

    int ret;
    if (rl_last_func == my_rl_complete)
        ret = rl_complete_internal('?');
    else
        ret = rl_complete_internal('!');

    char *new_text = rl_copy_text(0, rl_end);

    if (rl_point != old_point || strcmp(old_text, new_text) != 0) {

        struct stat st;
        if (stat(new_text, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t len = strlen(new_text);
            if (len > 0 && new_text[len - 1] != '/') {
                rl_point = len;
                rl_insert_text("/");
            }
        }

        clear_screen_and_buffer(0, 0);

        // clear_screen_and_buffer() restored the default Esc binding. If a
        // completion list was just displayed, re-install the hijack so the
        // first Esc clears the list and the second exits.
        if (list_shown_during_completion) {
            install_esc_hijack();
        }

        if (RetainAndRestoreReadlineBuffer::g_rl_complete_mode == 1) {
            RetainAndRestoreReadlineBuffer::g_rl_pending_text = new_text;
            rl_on_new_line();
            rl_replace_line("", 0);
            rl_done = 1;
            RetainAndRestoreReadlineBuffer::g_rl_complete_mode = 0;
        }
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
 * - `Ctrl+L` (`\f`): Clears the screen and current input buffer.
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
    // Ctrl+L now calls clear_screen_and_buffer directly (no static-captured
    // line that was reused on every later press).
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
