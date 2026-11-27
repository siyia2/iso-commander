// SPDX-License-Identifier: GPL-3.0-or-later

// Keeps the current list and prompt aligned when the terminal is resized.

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <wchar.h>


import std;
import isocmd.input.lineEditor;
import isocmd.ui.display.lineEditorResize;
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

	// Strip the line editor's nonprinting delimiters but keep the ANSI styling. If
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
	// starts on a space. Never skip the cursor or change the line-editor buffer.
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

		const bool active = editorSelectionActive() != 0;
		const auto lo = static_cast<std::size_t>(std::min(editorSelectionMark, editorCursor));
		const auto hi = static_cast<std::size_t>(std::max(editorSelectionMark, editorCursor));
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
		EditorRedisplay* previous = editorRedisplayHook;
		std::size_t first = 0;
		bool initial = true;
		bool enabled = false;

		FixedPrompt();
		~FixedPrompt();
		void draw();
		EditorLine read(const std::string& prompt);
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
		if (previous == editorRedisplay) editorRedisplayHook = fixedPromptRedisplay;
	}

	FixedPrompt::~FixedPrompt() {
		if (!enabled) return;
		editorRedisplayHook = previous;
		activePrompt = nullptr;
	}

	EditorLine FixedPrompt::read(const std::string& prompt) {

		EditorLine line = editLine(prompt);
		// The native backend leaves the successful-submit newline to this
		// renderer. EOF already emits its newline.
		if (enabled && line) {
			FILE* stream = editorOutputStream ? editorOutputStream : stdout;
			std::fputc('\n', stream);
			std::fflush(stream);
		}
		return line;

	}

	// Emit any multiline header only on the initial redisplay.
	std::string_view displayPromptTail(FILE* stream, bool& initial) {

		const std::string_view prompt(editorDisplayPrompt ? editorDisplayPrompt : "");
		const auto split = prompt.rfind('\n');
		const auto tail = split == std::string_view::npos ? 0 : split + 1;
		if (initial && tail) {
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
		FILE* stream = editorOutputStream ? editorOutputStream : stdout;
		if (cols < 4) {
			std::fputs("\r\033[K", stream);
			std::fflush(stream);
			return;
		}
		// Never write in the rightmost terminal column (autowrap boundary).
		const auto usable = cols - 1;
		const auto prompt = displayPromptTail(stream, initial);
		const auto [label, labelWidth] = fitPrompt(prompt, usable > 4 ? usable - 4 : 0);
		const std::string_view input(editorLineBuffer ? editorLineBuffer : "", static_cast<std::size_t>(editorBufferEnd));
		const auto layout = measureInput(input, static_cast<std::size_t>(editorCursor));
		const auto window = inputWindow(layout, usable - labelWidth, first);
		first = window.first;
		const auto out = renderInputLine(label, labelWidth, layout, window);
		std::fwrite(out.data(), 1, out.size(), stream);
		std::fflush(stream);

	}

} // namespace

void redisplayPrompt() {

	if (activePrompt) activePrompt->draw();
	else editorRedisplay();

}

/**
 * @brief Wraps @c editLine() with an optional @ref ListResizeWatcher and
 *        temporary Tab-completion control.
 *
 * Runs @p repaint (on the calling thread, from the line editor's idle hook) whenever
 * the terminal size changes while the prompt waits. The watcher is stopped
 * and the previous @c editorEventHook restored before this returns.
 *
 * The native editor's fallback terminal-size polling (@c editorCatchSigwinch)
 * is disabled for the watcher's lifetime to avoid duplicate prompt redraws.
 * The previous value is restored on every exit path, including exceptions
 * from the watcher constructor.
 *
 * The ordinary prompt stays fixed while its editable input scrolls horizontally.
 * The full line-editor buffer is retained. The live Filter preview retains its
 * own redisplay hook and delegates the input row to the same renderer.
 *
 * Optionally disables Tab completion for the duration of the Line editor call.
 * When enabled, Tab does not invoke completion or display matching entries.
 * The previous Tab key binding and @c editorCompletionHook are
 * saved and automatically restored afterward, leaving other line-editor prompts
 * unaffected.
 *
 * If @p repaint is empty, no resize watcher is installed, but Tab-completion
 * control is still applied when requested.
 *
 * @param prompt               Prompt passed to @c editLine() (may contain
 *                             \001/\002 markers).
 * @param repaint              Repaint callback (see @ref ListResizeWatcher
 *                             for the contract). Empty means no resize watcher.
 * @param disableTabCompletion Set to true to temporarily disable Tab completion;
 *                             defaults to false.
 *
 * @return The line read (freed by the returned pointer), or null on EOF
 *         (Ctrl+D).
 */
EditorLine editLineWithResize(
    const std::string& prompt,
    ListResizeWatcher::Repaint repaint,
    bool disableTabCompletion) {

	// Use the actual terminal width, including changes between prompts.
	const TermSize size = terminalSize();
	if (size.rows && size.cols) {
		editorSetScreenSize(static_cast<int>(size.rows), static_cast<int>(size.cols));
	}

	FixedPrompt fixedPrompt;

	// Save and restore the Tab binding and completion callbacks.
	struct TabCompletionGuard {
		EditorCommand* tabBinding;
		EditorCompletion* attemptedCompletion;

		TabCompletionGuard()
		    : tabBinding(editorKeySequenceHandler("\t")),
		      attemptedCompletion(editorCompletionHook) {}

		~TabCompletionGuard() {

			editorCompletionHook = attemptedCompletion;
			editorBindKey('\t', tabBinding);

		}

		TabCompletionGuard(const TabCompletionGuard&) = delete;
		TabCompletionGuard& operator=(const TabCompletionGuard&) = delete;
	};

	std::unique_ptr<TabCompletionGuard> tabGuard;

	if (disableTabCompletion) {
		tabGuard = std::make_unique<TabCompletionGuard>();

		editorCompletionHook = nullptr;
		editorBindKey('\t', [](int, int) -> int {

			return 0;

		});
	}

	if (!repaint) {
		return fixedPrompt.read(prompt);
	}

	struct SigwinchGuard {
		int saved;

		SigwinchGuard() : saved(editorCatchSigwinch) {

			editorCatchSigwinch = 0;

		}

		~SigwinchGuard() {

			editorCatchSigwinch = saved;

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
 * previously installed handler, if one was installed). Terminal output and
 * Line editor operations happen in @ref tick on the main thread, from the line editor's
 * event hook, where the line editor's line-buffer and cursor state are safe to touch.
 */
struct ListResizeWatcher::Impl {
	explicit Impl(Repaint r) : repaint(std::move(r)), last(terminalSize()) {}

	Repaint           repaint;
	TermSize          last;                     // last dimensions we repainted for (main thread only)
	EditorHook* prevHook = nullptr;     // hook installed before ours (chained)

	/// Runs from the line editor's event hook on the main thread. Repaints the list
	/// after a resize, updates the line editor's screen dimensions, and restores the prompt.
	void tick();
};

namespace {

	/// Atomically set from the signal handler, cleared on the main thread.
	std::atomic<bool> g_winch{false};

	/// Handler installed before ours; preserve its behavior while watching resizes.
	struct sigaction g_prevWinch {};
	bool             g_winchInstalled = false;

	/// Active watcher used by the Line editor event hook. Accessed only on the
	/// thread running Line editor; set during construction and cleared by stop().
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
	/// before this watcher so other Line editor event processing remains active.
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

	// Keep the line editor's horizontal viewport in sync with the dimensions used by the list renderer.
	editorSetScreenSize(static_cast<int>(sz.rows), static_cast<int>(sz.cols));

	// The repaint callback performs the normal list display step. Prevent
	// exceptions from propagating through the editor event loop.
	try { repaint(); } catch (...) {}

	// Leave a blank line after the list, then let Line editor redraw the prompt
	// and the current input, including its cursor position.
	std::cout << '\n' << std::flush;
	// FixedPrompt redraws only the final line, after repaint() prints the header.

	redisplayPrompt();

}

ListResizeWatcher::ListResizeWatcher(Repaint repaint)
    : impl_(std::make_unique<Impl>(std::move(repaint))) {

	// Only one watcher may be active at a time: hook and signal state are process-global.
	assert(g_activeListWatcher == nullptr && "only one ListResizeWatcher may be active");

	impl_->prevHook = editorEventHook;
	if (impl_->prevHook == listResizeHook) impl_->prevHook = nullptr;   // never chain to ourselves
	g_activeListWatcher = impl_.get();
	editorEventHook = listResizeHook;

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
	if (editorEventHook == listResizeHook) editorEventHook = impl_->prevHook;

}
