// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <readline/readline.h>

import std;
import isocmd.core.cancellation;
import isocmd.core.uiState;
import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.proc.process;
import isocmd.ui.bar.progressBarSizeWatcher;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;
import isocmd.ui.display.readlineResize;

namespace {

	// ============================================================================
	// Progress-specific Readline resize hook
	// ============================================================================

	/// Repaint callback used by the readline event hook while the verbose prompt
	/// waits. rl_event_hook is a plain function pointer and cannot carry state,
	/// so the active callback is reached through this pointer (set only around
	/// that one readline() call).
	std::function<int()>* g_progressResizeHook = nullptr;

	/// rl_event_hook trampoline: forwards to the active callback, if any.
	int progressResizeHookTrampoline() {

		return g_progressResizeHook ? (*g_progressResizeHook)() : 0;

	}

	// ============================================================================
	// Progress-bar terminal input handling
	// ============================================================================

	/**
	 * @brief Terminal progress bar with input blocking and themed output.
	 */

	struct termios oldt {};
	int oldf = 0;

	/**
	 * @brief Disables terminal canonical mode and echoing to block user input
	 *        during progress updates.
	 *
	 * @param oldt Pointer to store original terminal settings.
	 * @param oldf Pointer to store original file descriptor flags.
	 */
	void disableInputForProgressBar(struct termios* oldt, int* oldf) {

		struct termios newt;

		tcgetattr(STDIN_FILENO, oldt);
		newt = *oldt;
		newt.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &newt);

		*oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
		fcntl(STDIN_FILENO, F_SETFL, *oldf | O_NONBLOCK);

	}

	/**
	 * @brief Restores terminal settings to their original state.
	 *
	 * @param oldt Pointer to original terminal settings.
	 * @param oldf Original file descriptor flags.
	 */
	void restoreInput(struct termios* oldt, int oldf) {

		tcsetattr(STDIN_FILENO, TCSANOW, oldt);
		fcntl(STDIN_FILENO, F_SETFL, oldf);

	}

	// ============================================================================
	// Progress-bar formatting and rendering helpers
	// ============================================================================

	constexpr std::size_t kMinBarWidth = 10;
	constexpr int kMinSafeCols = 8;

	bool sameSize(const TermSize& a, const TermSize& b) {

		return a.cols == b.cols && a.rows == b.rows;

	}

	std::size_t numDigits(std::size_t n) {

		std::size_t d = 1;

		while (n >= 10) {
			n /= 10;
			++d;
		}

		return d;

	}

	std::string formatSize(double bytes) {

		std::stringstream ss;

		const char* units[] = {" B", " KB", " MB", " GB"};
		int unit = 0;
		double size = bytes;

		while (size >= 1024 && unit < 3) {
			size /= 1024;
			unit++;
		}

		ss << std::fixed << std::setprecision(2) << size << units[unit];
		return ss.str();

	}

	std::string formatFixed(double value, int precision) {

		std::ostringstream o;
		o << std::fixed << std::setprecision(precision) << value;
		return o.str();

	}

	/**
	 * @brief Finds the end of a CSI escape sequence.
	 *
	 * If s[i] starts a CSI escape sequence, returns the index just past it;
	 * otherwise returns i.
	 */
	std::size_t escapeSequenceEnd(const std::string& s, std::size_t i) {

		if (static_cast<unsigned char>(s[i]) != 0x1B ||
		    i + 1 >= s.size() || s[i + 1] != '[') {
			return i;
		}

		std::size_t j = i + 2;

		while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) {
			++j;
		}

		if (j < s.size()) {
			++j;
		}

		return j;

	}

	/**
	 * @brief Returns the byte length of the UTF-8 character starting at s[i].
	 */
	std::size_t utf8CharLength(const std::string& s, std::size_t i) {

		std::size_t len = 1;

		while (i + len < s.size() &&
		       (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) {
			++len;
		}

		return len;

	}

	/**
	 * @brief Clips one line to maxCols visible columns, keeping ANSI escapes intact.
	 */
	std::string clipToWidth(const std::string& s, std::size_t maxCols) {

		std::string out;
		out.reserve(s.size());

		std::size_t w = 0;

		for (std::size_t i = 0; i < s.size();) {
			const std::size_t escEnd = escapeSequenceEnd(s, i);

			if (escEnd != i) {
				out.append(s, i, escEnd - i);
				i = escEnd;
				continue;
			}

			const std::size_t len = utf8CharLength(s, i);

			if (w >= maxCols) {
				break;
			}

			out.append(s, i, len);
			i += len;
			++w;
		}

		return out;

	}

	/**
	 * @brief Clips every line of a multi-line text.
	 */
	std::string clipText(const std::string& text, std::size_t maxCols) {

		std::string out;
		std::size_t start = 0;

		while (true) {
			const std::size_t nl = text.find('\n', start);

			if (nl == std::string::npos) {
				out += clipToWidth(text.substr(start), maxCols);
				break;
			}

			out += clipToWidth(text.substr(start, nl - start), maxCols);
			out += '\n';
			start = nl + 1;
		}

		return out;

	}

	/**
	 * @brief Returns the configured processing bar width for an operation.
	 */
	int processingBarWidthFor(const std::string& operation) {

		if (operation.find("mount") != std::string::npos ||
		    operation.find("umount") != std::string::npos) {
			return 46;
		}

		if (operation.find("2iso") != std::string::npos) {
			return 47;
		}

		return 41;

	}

	// ============================================================================
	// Progress-bar session
	// ============================================================================

	/**
	 * @brief State and drawing logic behind displayProgressBarWithSize().
	 *
	 * Construction themes the output, blocks terminal input and starts the size
	 * watcher (in that order); run() renders until the operation completes, then
	 * prints the final report and asks the verbose question.
	 */
	class ProgressBarSession {
	public:
		ProgressBarSession(
		    std::atomic<std::size_t>* completedBytes,
		    std::size_t totalBytes,
		    std::atomic<std::size_t>* completedTasks,
		    std::atomic<std::size_t>* failedTasks,
		    std::size_t totalTasks,
		    std::atomic<bool>* isComplete,
		    bool* verbose,
		    const std::string& operation,
		    const std::string& header)
		    : pc_(resolveProgressTheme()),
		      completedBytes_(completedBytes),
		      totalBytes_(totalBytes),
		      completedTasks_(completedTasks),
		      failedTasks_(failedTasks),
		      totalTasks_(totalTasks),
		      isComplete_(isComplete),
		      verbose_(verbose),
		      operation_(operation),
		      header_(header),
		      processingBarWidth_(processingBarWidthFor(operation)),
		      bytesTrackingEnabled_(completedBytes != nullptr),
		      totalBytesFormatted_(
		          completedBytes != nullptr
		              ? formatSize(static_cast<double>(totalBytes))
		              : std::string()),
		      curSize_(watcher_.get()),
		      lastSize_(curSize_) {}

		void run() {

			std::cout << "\033[?25l" << std::flush;

			struct pollfd pfds[2] = {
				{STDIN_FILENO, POLLIN, 0},
				{watcher_.wakeFd(), POLLIN, 0},
			};

			bool finished = false;

			while (!finished) {
				waitForEvents(pfds);
				finished = paintFrame();

				if (finished) {
					finish();
				}
			}

			std::cout << '\n';

		}

	private:
		struct InputBlocker {
			InputBlocker() {

				disableInputForProgressBar(&oldt, &oldf);

			}
		};

		struct Counters {
			std::size_t completed;
			std::size_t failed;
			std::size_t bytes;
		};

		struct LineContext {
			std::size_t completedTasks = 0;
			std::string timeStr;
			std::string pctStr;
			std::size_t usableCols = 1;
			int barWidth = 0;
			int progressPos = 0;
			bool useFinalLayout = false;
			bool cancelled = false;
			double elapsed = 0.0;
		};

		// ---- main loop ------------------------------------------------------

		static void drainStdin() {

			char ch;

			while (read(STDIN_FILENO, &ch, 1) > 0) {
			}

		}

		void waitForEvents(struct pollfd* pfds) {

			if (poll(pfds, 2, 100) > 0) {
				if (pfds[0].revents & POLLIN) {
					drainStdin();
				}

				if (pfds[1].revents & POLLIN) {
					watcher_.drainWake();
				}
			}

		}

		/**
		 * @brief Draws one frame, repainting the header after a resize.
		 *
		 * @return true when the operation is complete.
		 */
		bool paintFrame() {

			curSize_ = watcher_.get();

			const bool resized = hasDrawn_ && !sameSize(curSize_, lastSize_);

			std::string frame;

			if (kUseSynchronizedOutput) {
				frame.append(kSyncBegin);
			}

			if (resized) {
				frame += "\033[H\033[2J\033[3J";
				frame += resizeHeaderText();
			}

			frame += render(false, false, false);

			lastSize_ = curSize_;
			hasDrawn_ = true;

			const bool done = isComplete_->load(std::memory_order_acquire);

			if (bytesTrackingEnabled_ && !done && lastRenderedLines_ > 1) {
				frame += "\033[";
				frame += std::to_string(lastRenderedLines_ - 1);
				frame += "A";
			}

			if (kUseSynchronizedOutput) {
				frame.append(kSyncEnd);
			}

			std::cout << frame << std::flush;

			return done;

		}

		std::size_t usableCols() const {

			return curSize_.cols > 1 ? curSize_.cols - 1 : 1;

		}

		std::string resizeHeaderText() const {

			return header_.empty()
			    ? std::string("\n")
			    : clipText(header_, usableCols());

		}

		// ---- rendering ------------------------------------------------------

		Counters readCounters() const {

			if (countersFrozen_) {
				return {
					frozenCompletedTasks_,
					frozenFailedTasks_,
					frozenCompletedBytes_
				};
			}

			return {
				completedTasks_->load(std::memory_order_acquire),
				failedTasks_->load(std::memory_order_acquire),
				bytesTrackingEnabled_
				    ? completedBytes_->load(std::memory_order_acquire)
				    : 0
			};

		}

		double elapsedSeconds() const {

			if (frozenElapsedSeconds_ >= 0.0) {
				return frozenElapsedSeconds_;
			}

			return std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::high_resolution_clock::now() - startTime_).count() / 1000.0;

		}

		double overallProgress(
		    const Counters& c,
		    bool forceFull,
		    bool cancelled) const {

			const double tasksProgress = cancelled
			    ? static_cast<double>(c.completed) / totalTasks_
			    : static_cast<double>(c.completed + c.failed) / totalTasks_;

			double overall = tasksProgress;

			if (bytesTrackingEnabled_) {
				const double bytesProgress =
				    static_cast<double>(c.bytes) / totalBytes_;

				overall = cancelled
				    ? bytesProgress
				    : std::max(bytesProgress, tasksProgress);
			}

			if (forceFull && !cancelled) {
				overall = 1.0;
			}

			return overall;

		}

		void updateSpeedCache(std::size_t completedBytesValue, double elapsed) {

			if (!bytesTrackingEnabled_ ||
			    completedBytesValue == lastCompletedBytes_) {
				return;
			}

			cachedCompletedBytesFormatted_ =
			    formatSize(static_cast<double>(completedBytesValue));

			const double speed = elapsed > 0.0
			    ? static_cast<double>(completedBytesValue) / elapsed
			    : 0.0;

			cachedSpeedFormatted_ = formatSize(speed) + "/s";
			lastCompletedBytes_ = completedBytesValue;

		}

		/**
		 * @brief Returns the widest bar the bytes/speed lines allow.
		 */
		std::size_t bytesLinesMaxBar(std::size_t usable) const {

			const std::size_t sizeWidth =
			    std::max<std::size_t>(totalBytesFormatted_.size(), 10);

			const std::size_t line2Text = 11 + sizeWidth + 1 + sizeWidth;
			const std::size_t line3Text = 7 + 10 + 2 + 6;
			const std::size_t textW = std::max(line2Text, line3Text);

			return usable > 3 + textW ? usable - 3 - textW : 0;

		}

		int barWidthFor(
		    std::size_t timeLen,
		    std::size_t usable,
		    bool useFinalLayout) const {

			const std::size_t taskDigits = numDigits(totalTasks_);
			const std::size_t timeWidth = std::max<std::size_t>(timeLen, 5);

			const std::size_t line1Fixed =
			    1 + 2 + 3 + 3 + taskDigits + 1 + taskDigits + 8 + timeWidth + 1;

			std::size_t maxBar = usable > line1Fixed
			    ? usable - line1Fixed
			    : 0;

			if (bytesTrackingEnabled_) {
				maxBar = std::min(maxBar, bytesLinesMaxBar(usable));
			}

			const std::size_t configuredBar = static_cast<std::size_t>(
			    useFinalLayout ? finalBarWidth_ : processingBarWidth_);

			return static_cast<int>(
			    std::min(configuredBar, std::max(kMinBarWidth, maxBar)));

		}

		std::string buildBarLine(const LineContext& ctx) const {

			std::ostringstream l1;

			l1 << color << "[" << color;

			for (int i = 0; i < ctx.barWidth; ++i) {
				l1 << (
				    i < ctx.progressPos
				        ? "="
				        : (i == ctx.progressPos && !ctx.useFinalLayout ? ">" : " ")
				);

				if (i == ctx.progressPos && !ctx.useFinalLayout) {
					l1 << color;
				}
			}

			l1 << color << "] " << ctx.pctStr
			   << "% (" << ctx.completedTasks << "/" << totalTasks_ << ") Time: "
			   << ctx.timeStr << "s";

			return l1.str();

		}

		static std::string indentedLine(
		    const std::string& text,
		    const LineContext& ctx) {

			std::size_t pad = static_cast<std::size_t>(ctx.barWidth) + 3;

			if (pad + text.size() > ctx.usableCols) {
				pad = ctx.usableCols > text.size()
				    ? ctx.usableCols - text.size()
				    : 0;
			}

			return std::string(pad, ' ') + text;

		}

		std::string buildProcessedLine(const LineContext& ctx) const {

			const std::string processed =
			    "Processed: " +
			    ((ctx.useFinalLayout && !ctx.cancelled)
			        ? totalBytesFormatted_
			        : cachedCompletedBytesFormatted_) +
			    "/" + totalBytesFormatted_;

			return indentedLine(processed, ctx);

		}

		std::string buildSpeedLine(const LineContext& ctx) const {

			std::string speedLine = "Speed: ";

			if (ctx.useFinalLayout && !ctx.cancelled) {
				const double avgSpeed = ctx.elapsed > 0.0
				    ? static_cast<double>(totalBytes_) / ctx.elapsed
				    : 0.0;

				speedLine += formatSize(avgSpeed) + "/s (avg)";
			} else {
				speedLine += cachedSpeedFormatted_;
			}

			return indentedLine(speedLine, ctx);

		}

		std::string render(
		    bool forceFull,
		    bool useFinalLayout,
		    bool cancelled) {

			const Counters c = readCounters();
			const double overall = overallProgress(c, forceFull, cancelled);
			const double elapsed = elapsedSeconds();

			updateSpeedCache(c.bytes, elapsed);

			LineContext ctx;

			ctx.completedTasks = c.completed;
			ctx.elapsed = elapsed;
			ctx.timeStr = formatFixed(elapsed, 1);
			ctx.pctStr = formatFixed(overall * 100.0, 0);
			ctx.usableCols = usableCols();
			ctx.useFinalLayout = useFinalLayout;
			ctx.cancelled = cancelled;
			ctx.barWidth = barWidthFor(
			    ctx.timeStr.size(),
			    ctx.usableCols,
			    useFinalLayout);
			ctx.progressPos = static_cast<int>(ctx.barWidth * overall);

			std::vector<std::string> lines;
			lines.reserve(3);

			lines.push_back(buildBarLine(ctx));

			if (bytesTrackingEnabled_) {
				lines.push_back(buildProcessedLine(ctx));
				lines.push_back(buildSpeedLine(ctx));
			}

			std::string out;

			for (std::size_t i = 0; i < lines.size(); ++i) {
				if (i > 0) {
					out += '\n';
				}

				out += "\r\033[2K";
				out += clipToWidth(lines[i], ctx.usableCols);
			}

			lastRenderedLines_ = static_cast<int>(lines.size());

			return out;

		}

		// ---- completion -----------------------------------------------------

		/**
		 * @brief Re-reads the terminal size.
		 *
		 * @return true if it changed since the last painted frame.
		 */
		bool refreshSizeAtEnd() {

			const TermSize nowSize = watcher_.refresh();

			const bool resizedAtEnd = !sameSize(nowSize, lastSize_);

			curSize_ = nowSize;
			lastSize_ = nowSize;

			return resizedAtEnd;

		}

		void freezeFinalState() {

			frozenElapsedSeconds_ =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        std::chrono::high_resolution_clock::now() - startTime_
			    ).count() / 1000.0;

			frozenCompletedTasks_ =
			    completedTasks_->load(std::memory_order_acquire);

			frozenFailedTasks_ =
			    failedTasks_->load(std::memory_order_acquire);

			frozenCompletedBytes_ = bytesTrackingEnabled_
			    ? completedBytes_->load(std::memory_order_acquire)
			    : 0;

			countersFrozen_ = true;

			finalCancelled_ =
			    GlobalState::g_operationCancelled.load(std::memory_order_acquire);

			finalSnapTo100_ = (!finalCancelled_ && frozenFailedTasks_ == 0);

		}

		void appendStatusLabel(std::ostream& s) const {

			const bool anyCompleted = frozenCompletedTasks_ > 0;

			if (finalCancelled_) {
				s << pc_.warning << "INTERRUPTED";
			} else if (frozenFailedTasks_ > 0) {
				s << (anyCompleted ? pc_.warning : pc_.failure)
				  << (anyCompleted ? "PARTIAL" : "FAILED");
			} else {
				s << pc_.success << "COMPLETED";
			}

		}

		std::string statusAndBar() {

			std::ostringstream s;

			s << "\r\033[2K" << color << " Status: " << operation_ << color
			  << " → ";

			appendStatusLabel(s);

			s << pc_.reset << '\n';
			s << render(finalSnapTo100_, true, finalCancelled_);

			return s.str();

		}

		void printFinalReport(bool resizedAtEnd) {

			std::ostringstream fin;

			if (kUseSynchronizedOutput) {
				fin << kSyncBegin;
			}

			if (resizedAtEnd) {
				fin << "\033[H\033[2J\033[3J"
				    << resizeHeaderText()
				    << "\033[1A";
			} else {
				fin << (
				    bytesTrackingEnabled_
				        ? "\033[1J\033[3A"
				        : "\033[1J\033[1A"
				);
			}

			fin << statusAndBar();

			if (kUseSynchronizedOutput) {
				fin << kSyncEnd;
			}

			std::cout << fin.str() << std::flush;

		}

		void finish() {

			signal(SIGINT, SIG_IGN);

			const bool resizedAtEnd = refreshSizeAtEnd();

			freezeFinalState();
			printFinalReport(resizedAtEnd);

			disableReadlineForConfirmation();

			std::cout << "\n\n";

			restoreInput(&oldt, oldf);
			reset_custom_keybindingsForSelect();

			askVerbose();

			watcher_.stop();

			restoreReadline();

		}

		// ---- verbose prompt -------------------------------------------------

		void askVerbose() {

			if (!GlobalState::isNonInteractive.load()) {
				*verbose_ = promptVerbose();
			} else {
				autoVerbose();
			}

		}

		void autoVerbose() {

			disableInputForProgressBar(&oldt, &oldf);

			std::this_thread::sleep_for(std::chrono::milliseconds(500));

			restoreInput(&oldt, oldf);

			std::cout << "\033[?25h" << std::flush;

			*verbose_ = true;

		}

		std::string makePrompt(std::string_view text) const {

			std::string prompt("\001");
			prompt
			    .append(color)
			    .append("\002")
			    .append(text)
			    .append("\001")
			    .append(pc_.reset)
			    .append("\002");
			return prompt;

		}

		std::string promptFor(int cols) const {

			using namespace std::string_view_literals;

			static constexpr auto fullText = "Display verbose output? (y/n): "sv;
			static constexpr auto shortText = "Verbose? (y/n): "sv;
			static constexpr auto tinyText = "y/n: "sv;

			if (cols >= static_cast<int>(fullText.size()) + 2) {
				return makePrompt(fullText);
			}

			if (cols >= static_cast<int>(shortText.size()) + 2) {
				return makePrompt(shortText);
			}

			return makePrompt(tinyText);

		}

		/**
		 * @brief Asks "Display verbose output?" and returns true for y/Y.
		 */
		bool promptVerbose() {

			std::cout << "\033[?25h" << std::flush;

			const TermSize startSize = watcher_.get();

			lastSize_ = startSize;
			curSize_ = startSize;

			std::string res;

			if (startSize.cols < kMinSafeCols) {
				const auto input = readlineWithResize(promptFor(static_cast<int>(startSize.cols)));
				if (input) res = input.get();
			} else {
				res = readAnswerWithResizeHook(startSize);
			}

			return res == "y" || res == "Y";

		}

		std::string readAnswerWithResizeHook(const TermSize& startSize) {

			std::string prompt = promptFor(static_cast<int>(startSize.cols));

			std::function<int()> onResize =
			    [&]() -> int { return onPromptResize(prompt); };

			g_progressResizeHook = &onResize;

			auto* savedHook = rl_event_hook;
			const int savedWinch = rl_catch_sigwinch;

			rl_event_hook = progressResizeHookTrampoline;
			rl_catch_sigwinch = 0;

			auto input = readlineWithResize(prompt);

			rl_catch_sigwinch = savedWinch;
			rl_event_hook = savedHook;
			g_progressResizeHook = nullptr;

			return input.get()
			    ? std::string(input.get())
			    : std::string();

		}

		/**
		 * @brief Readline idle hook for terminal resize handling.
		 *
		 * Repaints the final report and restores the prompt when the terminal
		 * is resized while waiting for the answer.
		 */
		int onPromptResize(std::string& prompt) {

			const TermSize now = watcher_.get();

			if (sameSize(now, lastSize_)) {
				return 0;
			}

			curSize_ = now;
			lastSize_ = now;

			if (now.cols < kMinSafeCols) {
				return 0;
			}

			rl_set_screen_size(
			    static_cast<int>(now.rows),
			    static_cast<int>(now.cols));

			const std::string newPrompt =
			    promptFor(static_cast<int>(now.cols));

			if (newPrompt != prompt) {
				prompt = newPrompt;
				rl_set_prompt(prompt.c_str());
			}

			repaintReportBeforePrompt();

			rl_on_new_line();
			rl_forced_update_display();

			if (kUseSynchronizedOutput) {
				std::cout << kSyncEnd << std::flush;
			}

			return 0;

		}

		void repaintReportBeforePrompt() {

			std::string body;

			if (kUseSynchronizedOutput) {
				body.append(kSyncBegin);
			}

			body += "\033[H\033[2J\033[3J";
			body += resizeHeaderText();
			body += "\033[1A";
			body += statusAndBar();
			body += "\n\n";

			std::cout << body << std::flush;

		}

		// ---- data -----------------------------------------------------------

		ProgressBarColors pc_;
		InputBlocker inputBlocker_;

		std::atomic<std::size_t>* completedBytes_;
		std::size_t totalBytes_;

		std::atomic<std::size_t>* completedTasks_;
		std::atomic<std::size_t>* failedTasks_;
		std::size_t totalTasks_;

		std::atomic<bool>* isComplete_;
		bool* verbose_;

		const std::string& operation_;
		const std::string& header_;

		int processingBarWidth_;
		int finalBarWidth_ = 30;

		std::chrono::high_resolution_clock::time_point startTime_ =
		    std::chrono::high_resolution_clock::now();

		bool bytesTrackingEnabled_;

		std::string totalBytesFormatted_;

		std::size_t lastCompletedBytes_ = SIZE_MAX;

		std::string cachedCompletedBytesFormatted_ = "0.00 B";
		std::string cachedSpeedFormatted_ = "0.00 B/s";

		ProgressBarSizeWatcher watcher_;

		TermSize curSize_;
		TermSize lastSize_;

		bool hasDrawn_ = false;

		int lastRenderedLines_ = 1;

		double frozenElapsedSeconds_ = -1.0;

		bool countersFrozen_ = false;

		std::size_t frozenCompletedTasks_ = 0;
		std::size_t frozenFailedTasks_ = 0;
		std::size_t frozenCompletedBytes_ = 0;

		bool finalCancelled_ = false;
		bool finalSnapTo100_ = false;
	};

} // namespace

/**
 * @brief Displays a multi-line progress bar with byte tracking, task counts,
 *        and elapsed time.
 *
 * @par Terminal width
 * The bar width is configured per operation (41 / 46 / 47 columns while
 * running, 30 in the final layout) but is shrunk whenever the terminal
 * is too narrow for the widest line. The fit is computed from worst-case text
 * widths (3-digit percentage, task-count digits, a 5-character time,
 * "1023.99 MB"-style sizes), so the bar width stays stable instead of
 * jittering as the numbers grow. The bar never goes below 10 columns; if the
 * terminal is narrower than that allows, lines are clipped (ANSI-escape and
 * UTF-8 aware) instead of wrapping. The last column is always kept free so a
 * line can never trigger auto-wrap. If the terminal size cannot be determined
 * (stdout is not a tty), 80x24 is assumed; if a later query fails
 * transiently, the last known good size is kept so it is not mistaken for a
 * resize.
 *
 * @par Terminal resize
 * A ProgressBarSizeWatcher installs a SIGWINCH handler (chained to the
 * previously installed one) that writes to a self-pipe polled together with
 * stdin, so a resize is repainted immediately and no polling thread is
 * needed. The loop also still renders every 100 ms for live values. The
 * watcher only reports sizes; all drawing happens on the calling thread.
 * Redrawing relies on moving the cursor up over the previous frame, which
 * cannot be trusted after a resize: the terminal may have reflowed the old
 * lines or pushed them into scrollback, where cursor movement cannot reach.
 * When the size changes, the screen and scrollback are wiped, the
 * caller-supplied @p header ("Processing ... (Ctrl+c:cancel)") is reprinted,
 * and the block is redrawn below it, so the header line that the final
 * "Status:" report overwrites is preserved. The same check runs once more
 * when the operation completes, so a resize between the last frame and the
 * final report is handled.
 *
 * @par Resize at the verbose prompt
 * The watcher keeps running while the "Display verbose output?" prompt waits.
 * An rl_event_hook (called by readline from its idle loop, on this thread)
 * compares the watcher's size with the last painted one and, on a change,
 * wipes the screen, repaints the final report (header, status, bar, with the
 * elapsed time and counters frozen at completion) and then calls
 * rl_on_new_line() + rl_redisplay() so the prompt and any half-typed answer
 * are restored. The whole repaint is one synchronized update. Readline's own
 * SIGWINCH handling is disabled for that call (rl_catch_sigwinch = 0), so the
 * watcher's handler is the only one involved.
 *
 * @par Structure
 * The work is done by ProgressBarSession (anonymous namespace above): a
 * small class whose methods each handle one concern (progress maths, bar
 * layout, line building, final report, verbose prompt). This function only
 * forwards its arguments and runs the session.
 *
 * @param completedBytes Atomic counter for bytes processed.
 * @param totalBytes     Total bytes to process.
 * @param completedTasks Atomic counter for successful tasks.
 * @param failedTasks    Atomic counter for failed tasks.
 * @param totalTasks     Total number of tasks.
 * @param isComplete     Atomic boolean signaling operation completion.
 * @param verbose        Pointer to boolean to store user preference for
 *                       verbose output.
 * @param operation      Description of the current operation.
 * @param header         Exact text the caller printed above the progress
 *                       block (blank line, "Processing ... (Ctrl+c:cancel)"
 *                       line, trailing newline). Reprinted after a resize
 *                       wipe. If empty, a single blank line is used instead.
 */
void displayProgressBarWithSize(
    std::atomic<std::size_t>* completedBytes,
    std::size_t totalBytes,
    std::atomic<std::size_t>* completedTasks,
    std::atomic<std::size_t>* failedTasks,
    std::size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation,
    const std::string& header) {

	ProgressBarSession session(
	    completedBytes,
	    totalBytes,
	    completedTasks,
	    failedTasks,
	    totalTasks,
	    isComplete,
	    verbose,
	    operation,
	    header);

	session.run();

}

// Keep the std::thread constructor instantiation in a single translation unit.
std::thread launchProgressBar(
    std::atomic<std::size_t>* completedBytes,
    std::size_t totalBytes,
    std::atomic<std::size_t>* completedTasks,
    std::atomic<std::size_t>* failedTasks,
    std::size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation,
    const std::string& header) {

	return std::thread(displayProgressBarWithSize,
	                   completedBytes, totalBytes,
	                   completedTasks, failedTasks, totalTasks,
	                   isComplete, verbose, operation, header);

}
