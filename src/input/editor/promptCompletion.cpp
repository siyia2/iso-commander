// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

import std;
import isocmd.input.lineEditor;
import isocmd.core.uiState;
import isocmd.input.inputHandling;
import isocmd.input.terminalControl;
import isocmd.input.promptCompletion;
import isocmd.ui.display.themes;
import isocmd.write2usb.util.writeTypes;

/**
 * @brief Retains pending editor input across prompt redraws and restoration.
 */
namespace RetainAndRestoreLineEditorBuffer {
	std::string g_editorPendingText = "";
}

/**
 * @brief Clears retained editor input on scope exit.
 *
 * Prevents pending text from leaking into subsequent input sessions.
 */
PendingLineEditorTextClearGuard::~PendingLineEditorTextClearGuard() {

	RetainAndRestoreLineEditorBuffer::g_editorPendingText.clear();

}

/**
 * @brief Manages the repaint callback for the interactive settings editor.
 */
namespace SettingsEditorRepaint {
	std::function<void()> g_fn;

	/**
	 * @brief Registers or replaces the settings editor repaint callback.
	 * @param fn Callback to invoke when repainting is required.
	 */
	void set(std::function<void()> fn) {

		g_fn = std::move(fn);

	}

	/**
	 * @brief Retrieves the currently registered repaint callback.
	 * @return Constant reference to the callback, which may be empty.
	 */
	const std::function<void()>& get() {

		return g_fn;

	}
}

/**
 * @file promptCompletion.cpp
 * @brief Custom line-editor completion and pagination logic for CLI commands and device mapping.
 */

static int current_page = 0;
static char last_common_prefix[1024] = "";

/// List of available special commands for completion.
const char* special_cmds[] = {
	"!clr", "!clr_paths", "!clr_filter", "*stats",
	NULL
};

/**
 * @brief Collects matching special commands when a trigger is present.
 *
 * Trigger characters are @c '!' and @c '*' at the start of the current word.
 * This entry point deliberately does @b not set
 * @c editorCompletionHandled; when no trigger is present it returns
 * an empty vector so the line editor falls back to its default
 * filename completion. This is the opposite of
 * @c setting_value_completion_entry(), which suppresses the default.
 *
 * @param text The partial text.
 * @param start Start index in line buffer (unused).
 * @param end End index in line buffer (unused).
 * @return Vector of matches (empty if no match).
 */
std::vector<std::string> my_special_completion_entry(std::string_view text, int start, int end) {

	(void)start;
	(void)end;

	if (text.empty() || (text.front() != '!' && text.front() != '*')) return {};
	std::vector<std::string> candidates;
	for (const char* name : special_cmds) {
		if (name && std::string_view(name).starts_with(text)) candidates.emplace_back(name);
	}
	return editorCompletionMatches(std::move(candidates));

}

/**
 * @brief Rebinds Esc to @c clear_list_and_screen while a completion list is shown.
 *
 * A first Esc after a completion list clears/repaints, and a second Esc
 * exits. This applies to every prompt that uses @c completePrompt, including
 * the Settings Editor's value prompt. @c clear_list_and_screen drops the
 * repaint snapshot before the screen is redrawn, so the Settings Editor's
 * repaint callback cannot redraw the list that is being dismissed.
 */
static void install_esc_hijack() {

	editorBindKeySequence("\033", clear_list_and_screen);

}

/// Rebinds Esc back to @c exit_handler.
static void restore_esc_default() {

	editorBindKeySequence("\033", exit_handler);

}

/**
 * @brief True while @c customListingsFunction is displaying a completion
 *        list for the current @c completePrompt() call.
 *
 * If the line editor inserts a common prefix while also displaying matches, the
 * subsequent @c clear_screen_and_buffer() call restores the default Esc
 * binding and, because this flag is set, preserves the repaint snapshot.
 * @c completePrompt() uses this flag to re-install the Esc hijack so the
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
		const LineEditorColors& rc;
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
		int hook_cursor_row = 0;    // 1-based terminal row where the line editor invoked the completion hook

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
		    const std::vector<std::string>& source,
		    int count,
		    CompletionKind completion_kind,
		    int page,
		    int base_length,
		    int cursor_row) {

			clear();

			matches = source;

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

	const char* specialColor(char c, const LineEditorColors& rc) {

		if (!isSpecialPrefix(c)) return rc.reset;
		return (c == '!') ? rc.warning : rc.accent;

	}

	CompletionKind detectCompletionKind(const std::vector<std::string>& matches, int num_matches) {

		if (num_matches > 0 && !matches[1].empty() && isSpecialPrefix(matches[1][0]))
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
	    const LineEditorColors& rc) {

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

	void printFooter(const PageInfo& page, int num_matches, const LineEditorColors& rc) {

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
	                     const char* suffix, int width, const LineEditorColors& rc) {

		std::string truncated = smartTruncate(text, width);
		int vis_len = (int)truncated.length() + (int)strlen(suffix);
		std::string display = std::string(color) + truncated + suffix + rc.reset;
		return {display, vis_len, raw};

	}

	DisplayItem formatItem(const char* raw, const FormatContext& ctx, int width) {

		const LineEditorColors& rc = ctx.rc;

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

	std::vector<DisplayItem> formatPageItems(const std::vector<std::string>& matches, const PageInfo& page, const FormatContext& ctx) {

		std::vector<DisplayItem> items;
		items.reserve(page.items_to_display);
		for (int i = 0; i < page.items_to_display; ++i)
		    items.push_back(formatItem(
		        matches[page.start_index + i].c_str(),
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

	// The native editor owns input and preserves any interleaved typeahead.
	int queryCursorRow() {

		return editorQueryCursorRow();

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
		// below the cursor position where the line editor invoked the completion hook.
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
	    const std::vector<std::string>& matches,
	    int num_matches,
	    CompletionKind kind,
	    int displayed_page,
	    int base_len,
	    int hook_cursor_row) {

		LineEditorColors rc = resolveLineEditorTheme();

		const PageInfo page =
		    computePageInfo(num_matches, displayed_page);

		const int term_rows = getTerminalRows();
		const int effective_cursor_row =
		    (hook_cursor_row > 0) ? std::min(hook_cursor_row, term_rows) : 0;
		const bool show_footer = needsPagination(num_matches);
		const int max_grid_rows =
		    maxGridRowsBelowCursor(effective_cursor_row, term_rows, num_matches, show_footer);

		// If there are no rows below the line editor's cursor, do not render.
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
	// to the row captured when the line editor originally invoked the hook.
	const int current_cursor_row = queryCursorRow();
	const int cursor_row =
	    (current_cursor_row > 0) ? current_cursor_row : lastCompletion.hook_cursor_row;

	renderCompletionListing(
	    lastCompletion.matches,
	    lastCompletion.num_matches,
	    lastCompletion.kind,
	    lastCompletion.displayed_page,
	    lastCompletion.base_len,
	    cursor_row);

}

/**
 * @brief Custom native-editor hook to display completions with pagination and color themes.
 *
 * @param matches  Vector of completion matches in line-editor format: matches[0] is the
 *                 common prefix of all completions (used for pagination state tracking,
 *                 not displayed), and matches[1..num_matches] are the actual completion
 *                 candidates. Note that matches[1] is interpreted as a filesystem path
 *                 prefix only in the file/directory completion branch; for special
 *                 command and settings-value completions it is simply the first
 *                 candidate string.
 * @param num_matches  Total number of completion candidates (excluding matches[0]).
 * @param max_length   Length of the longest match string as provided by the line editor.
 *                     Unused by this implementation — column widths are computed
 *                     directly from the match vector.
 */
void customListingsFunction(const std::vector<std::string>& matches, int num_matches, int max_length) {

	(void)max_length;

	// Capture the cursor position right where the line editor invoked the completion hook,
	// before any output is emitted, to enforce the vertical viewport constraint.
	const int hook_cursor_row = queryCursorRow();

	list_shown_during_completion = true;
	install_esc_hijack();

	const CompletionKind kind = detectCompletionKind(matches, num_matches);

	updatePaginationState(matches[0].c_str());

	const int displayed_page = current_page;

	const int base_len =
	    (kind == CompletionKind::File)
	        ? computeBaseLen(matches[1].c_str())
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
 * @brief Completion entry for the Settings Editor.
 *
 * Mirrors @c my_special_completion_entry in shape, but differs in two
 * deliberate ways: it sets @c editorCompletionHandled = 1 (so the line editor
 * does not fall back to filename completion on this prompt) and forces the
 * append character to a single space. When matches exist, the line editor hands the
 * list to @c customListingsFunction (installed as
 * @c editorCompletionDisplayHook by @c restoreLineEditor), so pagination,
 * colors, column layout, resize repaints and the Esc handling (first Esc
 * clears the list, second Esc exits) all behave exactly like the search prompt.
 *
 * @note The candidate list @c g_activeSettingValues is populated/cleared by
 *       the Settings Editor (see @c editSetting / the scope guard in
 *       settingsEditor.cpp); this function only consumes it.
 */
std::vector<std::string> setting_value_completion_entry(std::string_view text, int /*start*/, int /*end*/) {

	if (!g_activeSettingValues || g_activeSettingValues->empty()) return {};
	editorCompletionHandled = 1;
	editorCompletionAppendCharacter = ' ';
	std::vector<std::string> candidates;
	for (const auto& value : *g_activeSettingValues)
		if (value.starts_with(text)) candidates.push_back(value);
	return editorCompletionMatches(std::move(candidates));

}

/// Completed mappings outside the token currently being edited.
struct MappingAssignments {
	std::vector<bool> mapped;
	std::vector<std::string> used;
	bool valid = true; ///< All nonempty tokens outside the current token are valid.
};

/// Removes the same surrounding whitespace accepted by the mapping parser.
static std::string_view trimMappingPart(std::string_view part) {

	constexpr std::string_view whitespace = " \t\n\r\f\v";
	const auto first = part.find_first_not_of(whitespace);
	if (first == std::string_view::npos) return {};
	return part.substr(first, part.find_last_not_of(whitespace) - first + 1);

}

/**
 * @brief Records a complete mapping to a discovered device, ignoring invalid or duplicate pairs.
 * @param token Semicolon-delimited mapping token.
 * @param assignments Assigned ISO indices and device paths.
 * @return True if the mapping was recorded.
 */
static bool recordMapping(std::string_view token, MappingAssignments& assignments) {

	const auto sep = token.find('>');
	if (sep == std::string_view::npos) return false;
	const auto indexText = trimMappingPart(token.substr(0, sep));
	if (indexText.empty()) return false;
	std::size_t index = 0;
	const auto [end, ec] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
	if (ec != std::errc{} || end != indexText.data() + indexText.size() || index == 0 || index > assignments.mapped.size()) return false;

	std::string device(trimMappingPart(token.substr(sep + 1)));
	if (!device.starts_with("/dev/")) return false;
	const auto& devices = *g_completerData.usbDevices;
	if (std::find(devices.begin(), devices.end(), device) == devices.end()) {
		std::error_code error;
		device = std::filesystem::canonical(device, error).string();
		if (error || std::find(devices.begin(), devices.end(), device) == devices.end()) return false;
	}
	if (std::find(assignments.used.begin(), assignments.used.end(), device) != assignments.used.end()) return false;
	assignments.mapped[index - 1] = true;
	assignments.used.push_back(std::move(device));
	return true;

}

/**
 * @brief Collects assignments on either side of the current completion token.
 * @param line Full editor buffer.
 * @param start Byte offset of the current token.
 * @return Existing valid assignments, excluding the token being edited.
 */
static MappingAssignments collectMappingAssignments(std::string_view line, std::size_t start) {

	MappingAssignments assignments;
	if (!g_completerData.sortedIsos || !g_completerData.usbDevices) return assignments;
	assignments.mapped.resize(g_completerData.sortedIsos->size());
	for (std::size_t pos = 0; pos < line.size();) {
		const auto sep = line.find(';', pos);
		const auto finish = sep == std::string_view::npos ? line.size() : sep;
		if (start < pos || start > finish) {
			const auto token = trimMappingPart(line.substr(pos, finish - pos));
			if (!token.empty() && !recordMapping(token, assignments)) assignments.valid = false;
		}
		if (sep == std::string_view::npos) break;
		pos = sep + 1;
	}
	return assignments;

}

/**
 * @brief Appends the only remaining ISO/device pair after a complete final token.
 * @param text Current mapping token, preserved verbatim in the replacement.
 * @param assignments Completed mappings outside the current token.
 * @return A unique replacement including the separator, or no match if ambiguous.
 */
static std::vector<std::string> appendRemainingMapping(std::string_view text, MappingAssignments assignments) {

	if (!assignments.valid || !g_completerData.usbDevices || !recordMapping(text, assignments)) return {};
	const auto& devices = *g_completerData.usbDevices;
	if (std::count(assignments.mapped.begin(), assignments.mapped.end(), false) != 1 ||
	    assignments.used.size() + 1 != devices.size()) return {};

	const auto iso = std::find(assignments.mapped.begin(), assignments.mapped.end(), false);
	const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto& path) {
		return std::find(assignments.used.begin(), assignments.used.end(), path) == assignments.used.end();
	});
	if (device == devices.end()) return {};

	editorCompletionAppendCharacter = '\0';
	return {std::string(text) + ";" + std::to_string(iso - assignments.mapped.begin() + 1) + ">" + *device};

}

/**
 * @brief Handles tab-completion for ISO images (numeric indices with '>' separator).
 * @param text The partial text entered by the user.
 * @param start Start index in the line-editor buffer.
 * @param full_line The complete text of the line-editor buffer.
 * @param assignments Completed mappings outside the current token.
 * @return Vector of matches (empty if no match).
 */
static std::vector<std::string> handle_iso_completion(std::string_view text, int start, const std::string& full_line,
                                                      const MappingAssignments& assignments) {

	if (!g_completerData.sortedIsos) {
		return {};
	}

	std::vector<std::string> possible_completions;
	// Only implicit (empty-token) completion prioritizes unmapped ISOs.
	const bool preferUnmapped = text.empty() && std::find(assignments.mapped.begin(), assignments.mapped.end(), false) != assignments.mapped.end();
	for (size_t i = 0; i < g_completerData.sortedIsos->size(); ++i) {
		if (preferUnmapped && assignments.mapped[i]) continue;
		const std::string opt = std::to_string(i + 1) + ">";
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
		return {};
	}

	editorCompletionAppendCharacter = '\0';
	return editorCompletionMatches(std::move(possible_completions));

}

/**
 * @brief Handles tab-completion for USB target devices following the '>' separator.
 * @param text The partial text entered by the user.
 * @param start Start index in the line-editor buffer.
 * @param full_line The complete text of the line-editor buffer.
 * @param assignments Completed mappings outside the current token.
 * @return Vector of matches (empty if no match).
 */
static std::vector<std::string> handle_device_completion(std::string_view text, int start, const std::string& full_line,
                                                         const MappingAssignments& assignments) {

	if (!g_completerData.usbDevices) {
		return {};
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
		if (std::find(assignments.used.begin(), assignments.used.end(), dev) != assignments.used.end()) continue;
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
		return {};
	}

	editorCompletionAppendCharacter = '\0';
	return editorCompletionMatches(std::move(possible_device_completions));

}

/**
 * @brief Callback for the line editor to handle ISO-to-USB mapping completions.
 *
 * Resets pagination state and the completion repaint snapshot inline (the same
 * resets @c resetLineEditorPagination performs), suppresses the line editor's default
 * filename completion, then dispatches to the ISO-index or USB-device handler
 * depending on whether the current word contains a '>' separator. A unique
 * ISO-index match continues directly into device completion in the same Tab.
 * Empty tokens prefer unmapped ISOs, and devices already assigned elsewhere
 * are excluded. At the end of a complete mapping, Tab appends a separator
 * and the final pair when exactly one ISO and device remain unassigned.
 * Explicit indices may still be mapped to multiple devices.
 *
 * @param text The partial text currently being completed.
 * @param start Start index of the current word in the line buffer.
 * @param end End index of the current word in the line buffer.
 * @return Vector of matches (empty if no match).
 */
std::vector<std::string> completion_cb(std::string_view text, int start, int end) {

	current_page = 0;
	last_common_prefix[0] = '\0';
	lastCompletion.clear();

	editorCompletionHandled = 1;

	std::string current_word(editorLineBuffer + start, end - start);
	std::string full_line(editorLineBuffer);

	const auto assignments = collectMappingAssignments(full_line, start);
	if (text.empty() && g_completerData.usbDevices && !assignments.used.empty() &&
	    assignments.used.size() == g_completerData.usbDevices->size()) return {};

	bool is_device_completion = (current_word.find('>') != std::string::npos);

	if (is_device_completion) {
		if (static_cast<std::size_t>(end) == full_line.size()) {
			auto remaining = appendRemainingMapping(text, assignments);
			if (!remaining.empty()) return remaining;
		}
		return handle_device_completion(text, start, full_line, assignments);
	}

	auto matches = handle_iso_completion(text, start, full_line, assignments);
	// Once the ISO index is unambiguous, complete the device in the same Tab.
	// Keep the index-only match when there are no available devices.
	if (matches.size() == 1) {
		auto devices = handle_device_completion(matches.front(), start, full_line, assignments);
		if (!devices.empty()) return devices;
	}
	return matches;

}

/**
 * @brief Completes USB mappings on every Tab, including consecutive presses.
 * Mapping prompts do not display completion listings.
 */
int completeMappingPrompt(int, int) {

	editorCompletionAppendCharacter = '\0';
	return editorCompleteInternal('\t');

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
 * completion attempt, @c completePrompt clears the snapshot at the start of each
 * attempt, and @c clear_screen_and_buffer resets the page and prefix (and the
 * snapshot, unless a list was just shown during completion). External callers,
 * such as the Settings Editor's per-setting loop, call this function so a
 * snapshot from a previous prompt can never be repainted on a different screen.
 */
void resetLineEditorPagination() {

	current_page = 0;
	last_common_prefix[0] = '\0';
	lastCompletion.clear();

}

/**
 * @brief Dummy function to intercept and disable specific keypresses in the line editor.
 * @return Always 0.
 */
int ignoreEditorKeybinding(int, int) {

	return 0;

}

/**
 * @brief Clears the terminal screen, redraws the prompt, resets pagination state,
 *        and (usually) clears the saved completion repaint snapshot.
 *
 * If a Settings Editor body-repaint hook is installed
 * (@c SettingsEditorRepaint::g_fn), the editor's own draw lambda is invoked
 * instead of @c clearScrollBuffer so the header/value/description/footer are
 * repainted along with the prompt. Otherwise the scrollback is cleared. Line editor
 * is then asked to redraw, the page counter and cached common prefix are reset,
 * and the default Esc binding is restored via @c restore_esc_default.
 *
 * The completion snapshot (@c lastCompletion) is cleared only when
 * @c list_shown_during_completion is false. When @c completePrompt calls this
 * function after the line editor inserted a common prefix and also displayed a list,
 * the snapshot is deliberately preserved so the repaint hook can redraw the list
 * and later terminal resizes can repaint it. @c completePrompt re-installs the
 * Esc hijack right after this returns.
 *
 * @note The snapshot is cleared (when it is) only AFTER the repaint hook runs.
 *       A caller that wants a visible completion list dismissed (Esc, or Ctrl+L
 *       in the Settings Editor) must drop the snapshot first; use
 *       @c clear_list_and_screen for that.
 *
 * When @c RetainAndRestoreLineEditorBuffer::g_editorCompletionMode is active, the
 * current input line is captured, the line editor is forced to exit its loop, and the
 * completion mode flag is cleared so a subsequent loop iteration can restore
 * the buffer cleanly.
 *
 * Both key-handler parameters are unused.
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
	editorForcedUpdateDisplay();

	current_page = 0;
	last_common_prefix[0] = '\0';

	// Preserve the snapshot when completePrompt() is refreshing a listing
	// after the line editor inserted a common prefix. It is needed for future
	// terminal-resize repaints.
	//
	// Ordinary screen clears invalidate the snapshot. Explicit dismissal
	// clears it before reaching this function via clear_list_and_screen().
	if (!list_shown_during_completion)
	    lastCompletion.clear();

	// Always restore the default Esc binding when clearing. If a completion
	// list is currently visible, completePrompt() re-installs the hijack
	// right after this returns (tracked by list_shown_during_completion), so
	// the first Esc still clears the list and the second exits.
	restore_esc_default();

	const std::string new_text = editorCopyText(0, editorBufferEnd);

	if (RetainAndRestoreLineEditorBuffer::g_editorCompletionMode == 1) {
		RetainAndRestoreLineEditorBuffer::g_editorPendingText = new_text;

		editorReplaceLine("");
		editorDone = 1;
		RetainAndRestoreLineEditorBuffer::g_editorCompletionMode = 0;
	}

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
 * @brief TAB-completion handler that extends @c editorComplete with directory
 * slash handling, completion-list state management, and screen repaint.
 *
 * Delegates to @c editorCompleteInternal('?') on double-TAB (list matches)
 * or @c editorCompleteInternal('!') on single TAB (insert/complete). At the start
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
 * @note When @c RetainAndRestoreLineEditorBuffer::g_editorCompletionMode is 1, the
 *       repaint path (@c clear_screen_and_buffer) stashes the current input in
 *       @c g_editorPendingText, clears the buffer and sets @c editorDone so the next
 *       loop iteration can restore the text. This function does not handle that
 *       mode itself.
 *
 * @param ignore       Numeric argument (unused).
 * @param invoking_key Triggering key (unused).
 * @return Return value of @c editorCompleteInternal.
 */
int completePrompt(int ignore, int invoking_key)
{

	(void)ignore;
	(void)invoking_key;

	const bool had_previous_listing = lastCompletion.valid;

	// Start a fresh completion attempt.
	list_shown_during_completion = false;
	lastCompletion.clear();
	restore_esc_default();

	editorCompletionAppendCharacter = '\0';

	const int old_point = editorCursor;
	const std::string old_text = editorCopyText(0, editorBufferEnd);

	int ret;
	if (editorLastCommand == completePrompt)
	    ret = editorCompleteInternal('?');
	else
	    ret = editorCompleteInternal('!');

	const std::string new_text = editorCopyText(0, editorBufferEnd);

	const bool input_changed =
	    editorCursor != old_point || old_text != new_text;

	// Redraw when completion changes the input, or when a previous
	// listing was dismissed by an attempt that produced no new listing.
	if (input_changed ||
	    (had_previous_listing && !list_shown_during_completion)) {

		if (input_changed) {
			struct stat st;
			if (stat(new_text.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
				const size_t len = new_text.size();
				if (len > 0 && new_text[len - 1] != '/') {
					editorCursor = static_cast<int>(len);
					editorInsertText("/");
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

	return ret;

}

/**
 * @brief Restores line editor key bindings and re-inserts any pending buffer text.
 *
 * Rebinds line editor keys and, if a pending input line was saved in
 * RetainAndRestoreLineEditorBuffer::g_editorPendingText, installs a startup hook
 * that re-inserts it into the line editor buffer before the next prompt is
 * displayed. If no pending text exists, the startup hook is cleared.
 *
 * Key bindings set:
 * - `Ctrl+L` (`\f`): Clears/repaints the screen via @p clear_screen_and_buffer.
 *   The input buffer is only cleared (and stashed for restoration) when
 *   @c g_editorCompletionMode is active.
 * - `Tab`    (`\t`): Invokes custom completion via @p completePrompt.
 *
 * @note The startup hook consumes @p g_editorPendingText exactly once,
 *       clearing it after insertion to prevent repeated re-insertion.
 *
 * @see RetainAndRestoreLineEditorBuffer::g_editorPendingText
 * @see clear_screen_and_buffer()
 * @see completePrompt()
 */
void RestoreLineEditorBuffer() {

	editorBindKey('\f', clear_screen_and_buffer);
	editorBindKey('\t', completePrompt);

	if (!RetainAndRestoreLineEditorBuffer::g_editorPendingText.empty()) {
		editorStartupHook = []() -> int {

			if (!RetainAndRestoreLineEditorBuffer::g_editorPendingText.empty()) {
				editorInsertText(RetainAndRestoreLineEditorBuffer::g_editorPendingText);
				editorCursor = editorBufferEnd;
				RetainAndRestoreLineEditorBuffer::g_editorPendingText = "";
			}
			return 0;

		};
	} else {
		editorStartupHook = nullptr;
	}

}

/**
 * @brief Arms a one-shot line editor startup hook that restores stashed input.
 *
 * If @c RetainAndRestoreLineEditorBuffer::g_editorPendingText is non-empty,
 * installs an @c editorStartupHook that re-inserts it into the buffer before
 * the next prompt is displayed, places the cursor at the end of the text,
 * then clears both the stash and the hook so the text is restored exactly once.
 * If the stash is empty, the function does nothing.
 *
 * Call immediately before @c editLine(). Unlike @c RestoreLineEditorBuffer(),
 * it does not rebind any keys, so prompts that disable Tab completion and
 * Ctrl+L keep that behavior.
 *
 * @note The stash is filled by @c pgup_handler and @c pgdn_handler, and is
 *       cleared by @c LineEditorKeybindingGuard on scope exit.
 *
 * @see RetainAndRestoreLineEditorBuffer::g_editorPendingText
 * @see RestoreLineEditorBuffer()
 */
void armPendingInputRestore() {

	if (RetainAndRestoreLineEditorBuffer::g_editorPendingText.empty()) return;
	editorStartupHook = []() -> int {

		editorInsertText(RetainAndRestoreLineEditorBuffer::g_editorPendingText);
		editorCursor = editorBufferEnd;
		RetainAndRestoreLineEditorBuffer::g_editorPendingText.clear();
		editorStartupHook = nullptr;   // one-shot
		return 0;

	};

}
