// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <wchar.h>

#include <readline/readline.h>

import std;
import isocmd.ui.display.readlineResize;
import isocmd.ui.cli.termSize;

namespace {

	// Render terminal cells rather than bytes. Escape controls in the editable
	// buffer so bracketed pastes cannot create extra rows or inject ANSI codes.
	struct InputCell {
		std::size_t offset, length, width, column;
		std::string text;
	};

	InputCell inputCell(std::string_view input, std::size_t pos,
	                    std::size_t column, std::mbstate_t& state) {

		const auto c = static_cast<unsigned char>(input[pos]);
		if (c < 32 || c == 127) {
			state = {};
			return {pos, 1, 2, column, {'^', static_cast<char>(c ^ 64)}};
		}
		wchar_t wc{};
		const auto len = std::mbrtowc(&wc, input.data() + pos, input.size() - pos, &state);
		if (len == static_cast<std::size_t>(-1) || len == static_cast<std::size_t>(-2) || !len) {
			state = {};
			return {pos, 1, 1, column, "?"};
		}
		const int width = ::wcwidth(wc);
		if (width < 0) return {pos, len, 1, column, "?"};
		return {pos, len, static_cast<std::size_t>(width), column,
		        std::string(input.substr(pos, len))};

	}

	struct InputLayout {
		std::vector<InputCell> cells;
		std::size_t cursor = 0;
		std::size_t width = 0;
	};

	InputLayout measureInput(std::string_view input, std::size_t point) {

		InputLayout layout;
		std::mbstate_t state{};
		for (std::size_t pos = 0; pos < input.size();) {
			auto cell = inputCell(input, pos, layout.width, state);
			pos += cell.length;
			layout.width += cell.width;
			if (pos <= point) layout.cursor = layout.cells.size() + 1;
			layout.cells.push_back(std::move(cell));
		}
		return layout;

	}

	// Strip Readline's nonprinting delimiters but keep the ANSI styling. If
	// the prompt alone is too wide, shorten it to leave space for the input.
	std::pair<std::string, std::size_t> fitPrompt(std::string_view prompt, std::size_t budget) {

		std::string out;
		std::size_t width = 0;
		std::mbstate_t state{};
		for (std::size_t pos = 0; pos < prompt.size();) {
			if (prompt[pos] == '\001') {
				const auto end = prompt.find('\002', pos + 1);
				if (end == std::string_view::npos) break;
				out.append(prompt.substr(pos + 1, end - pos - 1));
				pos = end + 1;
				continue;
			}
			if (prompt[pos] == '\002') { ++pos; continue; }
			auto cell = inputCell(prompt, pos, width, state);
			if (cell.width > budget - width) {
				out += "\033[0m";
				break;
			}
			out += cell.text;
			width += cell.width;
			pos += cell.length;
		}
		return {std::move(out), width};

	}

	std::size_t columnAt(const InputLayout& layout, std::size_t index) {
		return index < layout.cells.size() ? layout.cells[index].column : layout.width;
	}

	struct InputWindow { std::size_t first, last; bool left, right; };

	// Avoid a visually empty edge after the overflow marker when clipping
	// starts on a space. Never skip the cursor or change the Readline buffer.
	std::size_t skipClippedSpaces(const InputLayout& layout, std::size_t first) {

		if (!first) return first;
		auto next = first;
		while (next < layout.cursor && layout.cells[next].text == " ") ++next;
		if (next < layout.cells.size() && layout.cells[next].text != " ") return next;
		return first;

	}

	InputWindow inputWindow(const InputLayout& layout, std::size_t budget, std::size_t previous) {

		if (layout.width < budget) return {0, layout.cells.size(), false, false};
		std::size_t first = std::min(previous, layout.cursor);
		const auto cursorCol = columnAt(layout, layout.cursor);
		// Reserve space for the cursor and for both overflow indicators.
		while (first < layout.cursor && cursorCol - columnAt(layout, first) + 2 >= budget)
			++first;
		// Recover more of the prefix when widening or returning towards the start.
		while (first && layout.width - columnAt(layout, first - 1) + 2 < budget)
			--first;
		first = skipClippedSpaces(layout, first);
		const bool left = first != 0;
		const auto room = budget - static_cast<std::size_t>(left);
		std::size_t last = first;
		const auto startCol = columnAt(layout, first);
		while (last < layout.cells.size() && columnAt(layout, last + 1) - startCol < room)
			++last;
		return {first, last, left, last < layout.cells.size()};

	}

	void appendWindow(std::string& out, const InputLayout& layout, const InputWindow& window) {

		const bool active = rl_mark_active_p() != 0;
		const auto lo = static_cast<std::size_t>(std::min(rl_mark, rl_point));
		const auto hi = static_cast<std::size_t>(std::max(rl_mark, rl_point));
		bool marked = false;
		for (std::size_t i = window.first; i < window.last; ++i) {
			const auto& cell = layout.cells[i];
			const bool highlight = active && cell.offset >= lo && cell.offset < hi;
			if (highlight != marked) out += highlight ? "\033[7m" : "\033[27m";
			marked = highlight;
			out += cell.text;
		}
		if (marked) out += "\033[27m";
		if (window.right) out += "»";

	}

	struct FixedPrompt {
		rl_voidfunc_t* previous = rl_redisplay_function;
		std::size_t first = 0;
		bool initial = true;
		bool enabled = false;

		FixedPrompt();
		~FixedPrompt();
		void draw();
		RlLine read(const std::string& prompt);
	};

	FixedPrompt* activePrompt = nullptr;

	void fixedPromptRedisplay() {
		if (activePrompt) activePrompt->draw();
	}

	FixedPrompt::FixedPrompt() {
		// Keep the live Filter preview hook in charge of list repainting.
		// It delegates the actual input row to redisplayPrompt().
		if (!::isatty(STDOUT_FILENO)) return;
		enabled = true;
		activePrompt = this;
		if (previous == rl_redisplay) rl_redisplay_function = fixedPromptRedisplay;
	}

	FixedPrompt::~FixedPrompt() {
		if (!enabled) return;
		rl_redisplay_function = previous;
		activePrompt = nullptr;
	}

	RlLine FixedPrompt::read(const std::string& prompt) {

		RlLine line(readline(prompt.c_str()), &std::free);
		// Native final redisplay has no rendered row to advance from when a
		// custom redisplay hook is installed. EOF already emits its newline.
		if (enabled && line) {
			FILE* stream = rl_outstream ? rl_outstream : stdout;
			std::fputc('\n', stream);
			std::fflush(stream);
		}
		return line;

	}

	// Emit any multiline header only on the initial redisplay.
	std::string_view displayPromptTail(FILE* stream, bool& initial) {

		const std::string_view prompt(rl_display_prompt ? rl_display_prompt : "");
		const auto split = prompt.rfind('\n');
		const auto tail = split == std::string_view::npos ? 0 : split + 1;
		if (initial && tail && !rl_already_prompted) {
			for (const char c : prompt.substr(0, tail))
				if (c != '\001' && c != '\002') std::fputc(c, stream);
		}
		initial = false;
		return prompt.substr(tail);

	}

	std::string renderInputLine(const std::string& label, std::size_t labelWidth,
	                            const InputLayout& layout, const InputWindow& window) {

		std::string out = "\r" + label;
		if (window.left) out += "«";
		appendWindow(out, layout, window);
		out += "\033[K\r";
		const auto cursor = labelWidth + static_cast<std::size_t>(window.left) +
		    columnAt(layout, layout.cursor) - columnAt(layout, window.first);
		if (cursor) out += "\033[" + std::to_string(cursor) + "C";
		return out;

	}

	void FixedPrompt::draw() {

		const auto size = terminalSize();
		const auto cols = size.cols ? size.cols : 80;
		FILE* stream = rl_outstream ? rl_outstream : stdout;
		if (cols < 4) {
			std::fputs("\r\033[K", stream);
			std::fflush(stream);
			return;
		}
		// Never write in the rightmost terminal column (autowrap boundary).
		const auto usable = cols - 1;
		const auto prompt = displayPromptTail(stream, initial);
		const auto [label, labelWidth] = fitPrompt(prompt, usable > 4 ? usable - 4 : 0);
		const std::string_view input(rl_line_buffer ? rl_line_buffer : "", static_cast<std::size_t>(rl_end));
		const auto layout = measureInput(input, static_cast<std::size_t>(rl_point));
		const auto window = inputWindow(layout, usable - labelWidth, first);
		first = window.first;
		const auto out = renderInputLine(label, labelWidth, layout, window);
		std::fwrite(out.data(), 1, out.size(), stream);
		std::fflush(stream);

	}

} // namespace

void redisplayPrompt() {

	if (activePrompt) activePrompt->draw();
	else rl_redisplay();

}

/**
 * @brief Wraps @c readline() with an optional @ref ListResizeWatcher and
 *        temporary Tab-completion control.
 *
 * Runs @p repaint (on the calling thread, from Readline's idle hook) whenever
 * the terminal size changes while the prompt waits. The watcher is stopped
 * and the previous @c rl_event_hook restored before this returns.
 *
 * Readline's own SIGWINCH handling (@c rl_catch_sigwinch) is disabled for the
 * watcher's lifetime and restored before this returns. The watcher already
 * redraws on resize; leaving Readline's built-in SIGWINCH handler enabled
 * makes it call @c rl_resize_terminal() -> @c rl_forced_update_display() ->
 * @c rl_redisplay_function() on the same event, racing the watcher's redraw
 * and potentially painting the prompt twice. The previous value is restored
 * on every exit path, including exceptions from the watcher constructor.
 *
 * The ordinary prompt stays fixed while its editable input scrolls horizontally.
 * The full Readline buffer is retained. The live Filter preview retains its
 * own redisplay hook and delegates the input row to the same renderer.
 *
 * Optionally disables Tab completion for the duration of the Readline call.
 * When enabled, Tab does not invoke completion or display matching entries.
 * The previous Tab key binding and @c rl_attempted_completion_function are
 * saved and automatically restored afterward, leaving other Readline prompts
 * unaffected.
 *
 * If @p repaint is empty, no resize watcher is installed, but Tab-completion
 * control is still applied when requested.
 *
 * @param prompt               Prompt passed to @c readline() (may contain
 *                             \001/\002 markers).
 * @param repaint              Repaint callback (see @ref ListResizeWatcher
 *                             for the contract). Empty means no resize watcher.
 * @param disableTabCompletion Set to true to temporarily disable Tab completion;
 *                             defaults to false.
 *
 * @return The line read (freed by the returned pointer), or null on EOF
 *         (Ctrl+D).
 */
RlLine readlineWithResize(
    const std::string& prompt,
    ListResizeWatcher::Repaint repaint,
    bool disableTabCompletion) {

	// Initialize first so ~/.inputrc cannot override the application's one-row
	// prompt layout. Reapply on entry in case Readline settings were reloaded.
	rl_initialize();
	rl_variable_bind("horizontal-scroll-mode", "on");

	// Use the actual terminal width, including changes between prompts.
	const TermSize size = terminalSize();
	if (size.rows && size.cols) {
		rl_set_screen_size(static_cast<int>(size.rows), static_cast<int>(size.cols));
	}

	FixedPrompt fixedPrompt;

	// Save and restore the Tab binding and completion callbacks.
	struct TabCompletionGuard {
		Keymap keymap;
		rl_command_func_t* tabBinding;
		rl_completion_func_t* attemptedCompletion;

		TabCompletionGuard()
		    : keymap(rl_get_keymap()),
		      tabBinding(rl_function_of_keyseq("\t", keymap, nullptr)),
		      attemptedCompletion(rl_attempted_completion_function) {}

		~TabCompletionGuard() {

			rl_attempted_completion_function = attemptedCompletion;
			rl_bind_key_in_map('\t', tabBinding, keymap);

		}

		TabCompletionGuard(const TabCompletionGuard&) = delete;
		TabCompletionGuard& operator=(const TabCompletionGuard&) = delete;
	};

	std::unique_ptr<TabCompletionGuard> tabGuard;

	if (disableTabCompletion) {
		tabGuard = std::make_unique<TabCompletionGuard>();

		rl_attempted_completion_function = nullptr;
		rl_bind_key('\t', [](int, int) -> int {

			return 0;

		});
	}

	if (!repaint) {
		return fixedPrompt.read(prompt);
	}

	struct SigwinchGuard {
		int saved;

		SigwinchGuard() : saved(rl_catch_sigwinch) {

			rl_catch_sigwinch = 0;

		}

		~SigwinchGuard() {

			rl_catch_sigwinch = saved;

		}

		SigwinchGuard(const SigwinchGuard&) = delete;
		SigwinchGuard& operator=(const SigwinchGuard&) = delete;
	} guard;

	ListResizeWatcher watcher(std::move(repaint));
	return fixedPrompt.read(prompt);

}

// ------------------------------------------------------------------------- //
// ListResizeWatcher                                                         //
// ------------------------------------------------------------------------- //

/**
 * @brief State for the SIGWINCH-driven resize watcher.
 *
 * The signal handler only sets an async-signal-safe flag (and forwards to the
 * previously installed handler, e.g. Readline's own). Terminal output and
 * Readline operations happen in @ref tick on the main thread, from Readline's
 * event hook, where Readline's line-buffer and cursor state are safe to touch.
 */
struct ListResizeWatcher::Impl {
	explicit Impl(Repaint r) : repaint(std::move(r)), last(terminalSize()) {}

	Repaint           repaint;
	TermSize          last;                     // last dimensions we repainted for (main thread only)
	rl_hook_func_t* prevHook = nullptr;     // hook installed before ours (chained)

	/// Runs from Readline's event hook on the main thread. Repaints the list
	/// after a resize, updates Readline's screen dimensions, and restores the prompt.
	void tick();
};

namespace {

	/// Atomically set from the signal handler, cleared on the main thread.
	std::atomic<bool> g_winch{false};

	/// Handler that was installed before ours; we forward to it so Readline's own
	/// SIGWINCH handling (rl_catch_sigwinch) keeps working.
	struct sigaction g_prevWinch {};
	bool             g_winchInstalled = false;

	/// Active watcher used by the Readline event hook. Accessed only on the
	/// thread running Readline; set during construction and cleared by stop().
	ListResizeWatcher::Impl* g_activeListWatcher = nullptr;

	extern "C" void onWinch(int sig, siginfo_t* info, void* ctx) {

		g_winch.store(true, std::memory_order_relaxed);
		if (g_prevWinch.sa_handler == SIG_DFL || g_prevWinch.sa_handler == SIG_IGN) return;
		if (g_prevWinch.sa_flags & SA_SIGINFO) {
			if (g_prevWinch.sa_sigaction) g_prevWinch.sa_sigaction(sig, info, ctx);
		} else {
			g_prevWinch.sa_handler(sig);
		}

	}

	/// Handles pending resize repainting, then invokes the hook that was installed
	/// before this watcher so other Readline event processing remains active.
	int listResizeHook() {

		ListResizeWatcher::Impl* w = g_activeListWatcher;
		if (!w) return 0;
		w->tick();
		return w->prevHook ? w->prevHook() : 0;

	}

} // namespace

void ListResizeWatcher::Impl::tick() {

	// Atomically check if true and reset to false in a single instruction.
	// If a signal arrives during processing, exchange() will leave/set it to true
	// so a subsequent tick catches it.
	if (!g_winch.exchange(false, std::memory_order_relaxed)) return;

	const TermSize sz = terminalSize();
	if (sz.cols == 0 || sz.rows == 0) return;
	if (sz.cols == last.cols && sz.rows == last.rows) return;
	last = sz;

	// Keep Readline's horizontal viewport in sync with the dimensions used by the list renderer.
	rl_set_screen_size(static_cast<int>(sz.rows), static_cast<int>(sz.cols));

	// The repaint callback performs the normal list display step. Prevent
	// exceptions from propagating through Readline's C call stack.
	try { repaint(); } catch (...) {}

	// Leave a blank line after the list, then let Readline redraw the prompt
	// and the current input, including its cursor position.
	std::cout << '\n' << std::flush;
	// Re-expand the prompt for the new width. rl_set_prompt() frees rl_prompt,
	// so pass it a copy. Only the last line of a multi-line prompt is
	// redrawn by Readline, which matches repaint() re-printing the header.
	if (rl_prompt) {
		const std::string p = rl_prompt;
		rl_set_prompt(p.c_str());
	}
	rl_on_new_line();
	redisplayPrompt();

}

ListResizeWatcher::ListResizeWatcher(Repaint repaint)
    : impl_(std::make_unique<Impl>(std::move(repaint))) {

	// Only one watcher may be active at a time: hook and signal state are process-global.
	assert(g_activeListWatcher == nullptr && "only one ListResizeWatcher may be active");

	impl_->prevHook = rl_event_hook;
	if (impl_->prevHook == listResizeHook) impl_->prevHook = nullptr;   // never chain to ourselves
	g_activeListWatcher = impl_.get();
	rl_event_hook = listResizeHook;

	struct sigaction sa {};
	sa.sa_sigaction = onWinch;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	g_winchInstalled = (sigaction(SIGWINCH, &sa, &g_prevWinch) == 0);

}

ListResizeWatcher::~ListResizeWatcher() {

	stop();

}

void ListResizeWatcher::stop() {

	if (!impl_ || g_activeListWatcher != impl_.get()) return;

	if (g_winchInstalled) {
		sigaction(SIGWINCH, &g_prevWinch, nullptr);
		g_winchInstalled = false;
	}

	g_activeListWatcher = nullptr;
	// Only undo our own installation; leave it alone if someone replaced the hook since.
	if (rl_event_hook == listResizeHook) rl_event_hook = impl_->prevHook;

}
