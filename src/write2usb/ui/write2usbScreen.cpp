// SPDX-License-Identifier: GPL-3.0-or-later

// Renders the live USB progress screen and handles resize and cancellation notifications.

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <readline/readline.h>

import std;
import isocmd.core.cancellation;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;
import isocmd.ui.display.readlineResize;
import isocmd.write2usb.ui.textWidth;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;

namespace fs = std::filesystem;

namespace write_screen_detail {

	/// Write end of the self-pipe the SIGWINCH handler pokes. -1 = no watcher active.
	inline std::atomic<int> g_fd{-1};

	/// Handler that was installed before ours; forwarded to so Readline's own
	/// SIGWINCH handling (rl_catch_sigwinch) keeps working.
	inline struct sigaction g_prev {};
	inline bool             g_installed = false;

	static_assert(std::atomic<int>::is_always_lock_free,
	              "g_fd must be lock-free to be read from a signal handler");

	inline void onWriteWinch(int sig, siginfo_t* info, void* ctx) {

		const int savedErrno = errno;
		const int fd = g_fd.load(std::memory_order_acquire);
		if (fd >= 0) {
			const char b = 'w';
			[[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);   // async-signal-safe; non-blocking
		}
		errno = savedErrno;

		if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) return;
		if (g_prev.sa_flags & SA_SIGINFO) {
			if (g_prev.sa_sigaction) g_prev.sa_sigaction(sig, info, ctx);
		} else {
			g_prev.sa_handler(sig);
		}

	}

	/// SIGINT handler that was installed before ours (the program's cancellation
	/// handler, which sets g_operationCancelled). Always called first.
	inline struct sigaction g_prevInt {};
	inline bool             g_intInstalled = false;

	/// Chained SIGINT handler. Forwards to the cancellation handler FIRST, so
	/// g_operationCancelled is already set when the relay thread wakes the
	/// waiters, then pokes the same self-pipe as SIGWINCH (byte 'c').
	inline void onWriteInt(int sig, siginfo_t* info, void* ctx) {

		const int savedErrno = errno;

		if (g_prevInt.sa_flags & SA_SIGINFO) {
			if (g_prevInt.sa_sigaction) g_prevInt.sa_sigaction(sig, info, ctx);
		} else if (g_prevInt.sa_handler != SIG_DFL && g_prevInt.sa_handler != SIG_IGN) {
			g_prevInt.sa_handler(sig);
		}

		const int fd = g_fd.load(std::memory_order_acquire);
		if (fd >= 0) {
			const char b = 'c';
			[[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);   // async-signal-safe; non-blocking
		}
		errno = savedErrno;

	}

} // namespace write_screen_detail

// ------------------------------------------------------------------------- //
// Confirmation-prompt resize watcher                                        //
// ------------------------------------------------------------------------- //

namespace confirm_resize_detail {

	struct ResizeRepaintState {
		std::size_t lastCols = 0;
		std::function<std::vector<std::size_t>(std::size_t)> paint;
		std::vector<std::size_t> lineWidths;
		int wakeRd = -1;
	};

	/// The confirmation prompt has one active watcher at a time.
	static ResizeRepaintState g_confirmResize;

	/// Write end of the self-pipe the SIGWINCH handler pokes. -1 = no watcher active.
	inline std::atomic<int> g_fd{-1};

	static_assert(std::atomic<int>::is_always_lock_free,
	              "g_fd must be lock-free to be read from a signal handler");

	inline struct sigaction g_prev {};
	inline bool g_installed = false;

	inline void onConfirmWinch(int sig, siginfo_t* info, void* ctx) {

		const int savedErrno = errno;
		const int fd = g_fd.load(std::memory_order_acquire);
		if (fd >= 0) {
			const char b = 1;
			[[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);
		}
		errno = savedErrno;

		if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) return;
		if (g_prev.sa_flags & SA_SIGINFO) {
			if (g_prev.sa_sigaction) g_prev.sa_sigaction(sig, info, ctx);
		} else {
			g_prev.sa_handler(sig);
		}

	}

	static void confirmResizeRepaint(ResizeRepaintState& s) {

		const TermSize sz = terminalSize();
		const std::size_t cols = sz.cols;
		const std::size_t rows = sz.rows;

		if (cols == 0 || rows == 0 || cols == s.lastCols) return;
		s.lastCols = cols;

		rl_set_screen_size(static_cast<int>(rows), static_cast<int>(cols));

		std::size_t warningRows = 0;
		for (std::size_t w : s.lineWidths)
		    warningRows += std::max<std::size_t>(1, (w + cols - 1) / cols);

		// The editable prompt scrolls horizontally; its cursor never adds rows.
		if (warningRows >= rows) return;

		SyncOutput sync;
		s.lineWidths = s.paint(cols);
		std::cout.flush();

		rl_on_new_line();
		redisplayPrompt();

	}

	/// Readline's getc hook waits on stdin and the self-pipe, with no idle polling.
	static int confirmResizeGetc(FILE* stream) {

		ResizeRepaintState& s = g_confirmResize;
		const int inFd = fileno(stream);

		for (;;) {
			struct pollfd fds[2] = {
				{ inFd,     POLLIN, 0 },
				{ s.wakeRd, POLLIN, 0 },
			};

			if (::poll(fds, 2, -1) < 0) {
				if (errno == EINTR) continue;
				return rl_getc(stream);
			}

			if (fds[1].revents & POLLIN) {
				char buf[64];
				while (::read(s.wakeRd, buf, sizeof buf) > 0) {}
				confirmResizeRepaint(s);
			}

			if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
			    return rl_getc(stream);
		}

	}

} // namespace confirm_resize_detail

struct ConfirmResizeWatcher::Impl {
	Impl(Painter paint, std::size_t drawnCols, std::vector<std::size_t> drawnWidths)
	    : state_(confirm_resize_detail::g_confirmResize) {

		using namespace confirm_resize_detail;
		assert(g_fd.load() < 0 && "only one ConfirmResizeWatcher may be active");

		state_.paint       = std::move(paint);
		state_.lineWidths  = std::move(drawnWidths);
		state_.lastCols    = drawnCols;

		int fds[2];
		if (::pipe(fds) != 0) return;
		for (int fd : fds) {
			::fcntl(fd, F_SETFD, FD_CLOEXEC);
			::fcntl(fd, F_SETFL, O_NONBLOCK);
		}
		wakeWr_       = fds[1];
		state_.wakeRd = fds[0];
		g_fd.store(wakeWr_, std::memory_order_release);

		struct sigaction sa {};
		sa.sa_sigaction = onConfirmWinch;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGWINCH, &sa, &g_prev) != 0) {
			closePipe();
			return;
		}
		g_installed = true;

		savedEventHook_  = rl_event_hook;
		savedGetc_       = rl_getc_function;
		rl_event_hook    = nullptr;
		rl_getc_function = confirmResizeGetc;
		active_ = true;

	}

	~Impl() {

		stop();

	}

	void stop() {

		using namespace confirm_resize_detail;
		if (!active_) return;
		active_ = false;

		rl_getc_function = savedGetc_;
		rl_event_hook    = savedEventHook_;

		if (g_installed) {
			sigaction(SIGWINCH, &g_prev, nullptr);
			g_installed = false;
		}
		closePipe();

	}

	void closePipe() {

		confirm_resize_detail::g_fd.store(-1, std::memory_order_release);
		if (wakeWr_ >= 0)        { ::close(wakeWr_);       wakeWr_ = -1; }
		if (state_.wakeRd >= 0)  { ::close(state_.wakeRd); state_.wakeRd = -1; }

	}

	confirm_resize_detail::ResizeRepaintState& state_;
	bool active_ = false;
	int wakeWr_ = -1;
	rl_hook_func_t* savedEventHook_ = nullptr;
	rl_getc_func_t* savedGetc_ = nullptr;
};

ConfirmResizeWatcher::ConfirmResizeWatcher(
        Painter paint, std::size_t drawnCols, std::vector<std::size_t> drawnWidths)
    : impl_(std::make_unique<Impl>(std::move(paint), drawnCols,
                                   std::move(drawnWidths))) {}

ConfirmResizeWatcher::~ConfirmResizeWatcher() = default;

void ConfirmResizeWatcher::stop() {

	if (impl_) impl_->stop();

}

/**
 * @brief Owns the live progress screen and keeps it correctly laid out.
 *
 * The screen has two phases:
 * - Running  — a keeper thread redraws the per-task rows.
 * - Finished — after finish() it shows the final rows, the status line and
 *   the summary. The keeper keeps running and repaints on terminal resize,
 *   unless a prompt was registered with setPrompt(): then the program's
 *   resize hook owns repainting (it calls repaint() and lets Readline redraw
 *   the prompt and typed input), so the keeper stays out of the way.
 *
 * @par Rendering
 * All drawing happens under one mutex (renderMutex_), from either the keeper
 * thread, finish() or repaint(), so frames never interleave.
 *
 * @par Waking the keeper (no polling)
 * The keeper blocks on the condition variable in the shared WriteSync state
 * and wakes only for:
 * - a task completion,
 * - a terminal resize (SIGWINCH),
 * - Ctrl+C (SIGINT), which also wakes the thread waiting in
 *   performWriteOperation(), and
 * - while running, the 500 ms pulse deadline (live percentages and speeds).
 * Once finished it sleeps indefinitely until a resize or stop().
 *
 * A signal handler cannot notify a condition variable, so SIGWINCH and SIGINT
 * are relayed: each handler writes one byte to a self-pipe (async-signal-safe)
 * and a small relay thread blocked in read() on that pipe notifies the
 * condition variable (and, for SIGWINCH, sets resizeSignalled_). The relay
 * thread consumes no CPU while idle.
 *
 * The SIGWINCH handler forwards to the previously installed handler. The
 * SIGINT handler is chained in front of the program's cancellation handler:
 * it calls that handler first (so g_operationCancelled is set) and only then
 * pokes the pipe. It is installed only if a real handler is present, and is
 * restored by stop() only if it is still the active one (performWriteOperation
 * sets SIGINT to SIG_IGN after the wait, which must not be overwritten).
 *
 * If the pipe or handler cannot be set up, the keeper falls back to checking
 * the terminal size every 100 ms, and cancelWakeable() reports false so the
 * cancellation wait falls back to a 500 ms recheck.
 *
 * Only one WriteScreen may be started at a time (the handler state is
 * process-global).
 *
 * @par Row layout
 * @code
 * filename → /dev/sdX <drive name> [▇▇▄░░] percentage speed
 * @endcode
 * Device path, drive name and speed are padded to the widest value across
 * rows so the bars line up. Drive names are always shown in full. The bar
 * has 5 cells and 10% steps while writing (empty ░, half ▄, full ▇)
 * and during active verification (empty space, half ⡆, full ⣶).
 * Both are floored from the percentage, so the bar is full only at 100%.
 * Terminal states restore the regular write bar. Failure or cancellation
 * after a completed write shows the full write bar rather than the
 * interrupted verification percentage.
 * The status column shows the live percentage (writing or verification),
 * DONE or PASS (speed shown
 * as the write-only " (avg)"), FAIL or CXL
 * (cancelled).
 *
 * @par Filename truncation
 * Names are shown in full unless the widest row exceeds the terminal width.
 * Only then is each name shortened to the space the terminal actually leaves
 * for the filename column (terminal width minus the rest of the row) as
 * "stem….iso": the ".iso" extension is always kept, the stem never shrinks
 * below 4 columns, and truncation is UTF-8 safe. On very narrow terminals
 * the stem floor can make the row slightly wider than the budget; the
 * per-row clip then prevents wrap.
 *
 * @par Redraw and result screen
 * At an unchanged size, a frame moves the cursor up by the previous frame's
 * physical line count and redraws. After a size change, and always for the
 * result screen, the screen and scrollback are wiped and redrawn from the
 * top. Rows are clipped to avoid wrapping, and each frame is written once
 * inside a synchronized update. Frames are skipped while the terminal size
 * is unknown (0 columns or rows). The result screen looks like this:
 * @code
 * (line 1: empty)
 * Status: write2usb → COMPLETED        (line 2)
 * (line 3: empty)
 * <rows...>                            (from line 4)
 * (empty)
 * Written: n/m | Time Elapsed: x.xs
 * Written: n/m | Verified: v/m | Time Elapsed: x.xs (with verification)
 * @endcode
 * The prompt itself is not printed by this class: after repaint() the
 * program's resize hook calls redisplayPrompt(), which redraws the prompt and
 * the user's typed input.
 *
 * @par Lifetime
 * Holds shared_ptr copies of the progress vector and the WriteSync state, so
 * it never refers to the stack of the function that created it. The
 * destructor stops and joins the keeper and relay threads and restores the
 * previous SIGWINCH handler.
 */
class WriteScreen {
public:
	WriteScreen(std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr,
	            std::shared_ptr<WriteSync> syncPtr,
	            WriteTheme theme)
	    : progressDataPtr_(std::move(progressDataPtr)),
	      progressData_(*progressDataPtr_),
	      syncPtr_(std::move(syncPtr)),
	      wt_(std::move(theme)) {}

	~WriteScreen() {

		stop();

	}

	WriteScreen(const WriteScreen&)            = delete;
	WriteScreen& operator=(const WriteScreen&) = delete;

	/// Prints the header, hides the cursor, computes the per-operation column
	/// data, installs the resize signal relay and starts the keeper thread.
	void start() {

		{
			std::lock_guard<std::mutex> rl(renderMutex_);
			std::cout << headerText(false) << "\033[?25l" << std::flush;
		}
		computeConstants();
		installResizeSignal();
		thread_ = std::thread([this] { keeperLoop(); });

	}

	/// Switches to the result screen and paints it. Call once, after the wait
	/// for completion / cancellation. The keeper keeps running afterwards.
	void finish(std::chrono::steady_clock::duration elapsed) {

		std::lock_guard<std::mutex> rl(renderMutex_);

		std::size_t failed = 0, written = 0, verified = 0;
		bool verificationRequested = false;
		for (const auto& prog : progressData_) {
			if (prog.failed.load()) ++failed;
			const bool complete = prog.completed.load();
			if (complete || prog.writeCompleted.load()) ++written;
			if (prog.verifyRequested) {
				verificationRequested = true;
				if (complete) ++verified;
			}
		}

		const std::size_t completed = syncPtr_->completedTasks.load();
		const bool   cancelled = isUsbWriteCancelled(syncPtr_->cancellation.get_token());

		const std::string operation =
		    std::string(UI::Palette::Yellow) + "write2usb" + wt_.rl_resetCol;

		const std::string word =
		    !cancelled
		        ? (failed > 0
		            ? (completed > 0
		                ? wt_.colorWarning + "PARTIAL"
		                : wt_.colorFailure + "FAILED")
		            : wt_.colorSuccess + "COMPLETED")
		        : wt_.colorWarning + "INTERRUPTED";

		std::ostringstream st;
		st << color << "Status: " << operation << color << " → " << word << wt_.rl_resetCol;
		statusLine_ = st.str();

		const double secs = std::chrono::duration<double>(elapsed).count();

		std::ostringstream sm;
		sm << std::fixed << std::setprecision(1);
		sm << "\n" << color << "Written: "
		   << wt_.colorSuccess << written
		   << color << "/"
		   << wt_.colorWarning << progressData_.size();
		if (verificationRequested) {
			sm << color << " | Verified: "
			   << wt_.colorSuccess << verified
			   << color << "/"
			   << wt_.colorWarning << progressData_.size();
		}
		sm << color << " | Time Elapsed: "
		   << color << secs << "s"
		   << wt_.colorStatus << UI::Palette::Reset << "\n";
		summaryText_ = sm.str();

		phase_ = Phase::Finished;
		renderLocked();

	}

	/// Registers the program's "press Enter" prompt. Once set, the keeper no
	/// longer repaints on resize by itself: the program's resize hook calls
	/// repaint() instead and Readline redraws the prompt and typed input.
	void setPrompt(std::string prompt) {

		std::lock_guard<std::mutex> rl(renderMutex_);
		promptText_ = std::move(prompt);

	}

	/// True if Ctrl+C is relayed into the shared condition variable, i.e. a
	/// waiter on that condition variable is woken immediately on cancellation
	/// and needs no timed recheck. Valid after start().
	bool cancelWakeable() const {

		return cancelChained_;

	}

	/// Re-emits the result screen (summary) at the current terminal width.
	/// Called by the pressEnterTo*WithRepaint() machinery on resize so the
	/// summary survives a terminal wipe. Safe to call from the main thread
	/// after finish(); takes the same lock the keeper uses.
	void repaint() {

		std::lock_guard<std::mutex> rl(renderMutex_);
		renderLocked();

	}

	/// Stops and joins the keeper and relay threads and restores the previous
	/// SIGWINCH handler. Safe to call more than once.
	void stop() {

		if (!thread_.joinable()) return;
		stop_.store(true, std::memory_order_release);

		if (wakeWr_ >= 0) {                         // wake the relay thread out of read()
			const char b = 1;
			[[maybe_unused]] const ssize_t r = ::write(wakeWr_, &b, 1);
		}
		if (relay_.joinable()) relay_.join();

		{ std::lock_guard<std::mutex> lk(syncPtr_->mutex); }
		syncPtr_->cv.notify_all();
		thread_.join();

		uninstallResizeSignal();

	}

private:
	enum class Phase { Running, Finished };

	static constexpr int  BAR_WIDTH = 5;
	static constexpr int  BAR_STEPS = BAR_WIDTH * 2;
	static constexpr auto kPulse = std::chrono::milliseconds(500);
	static constexpr auto kPoll  = std::chrono::milliseconds(100);   // fallback only

	// Right-pads an ASCII string to the given byte width. Used for device
	// paths and other ASCII-only columns.
	static std::string padRight(std::string s, std::size_t target) {

		if (s.size() < target) s.append(target - s.size(), ' ');
		return s;

	}

	// ---- Resize signal relay -------------------------------------------------

	void installResizeSignal() {

		using namespace write_screen_detail;
		assert(g_fd.load() < 0 && "only one WriteScreen may be started at a time");

		int fds[2];
		if (::pipe(fds) != 0) { pollFallback_ = true; return; }
		for (int fd : fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
		::fcntl(fds[1], F_SETFL, O_NONBLOCK);       // handler must never block
		wakeRd_ = fds[0];
		wakeWr_ = fds[1];

		g_fd.store(wakeWr_, std::memory_order_release);

		struct sigaction sa {};
		sa.sa_sigaction = onWriteWinch;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGWINCH, &sa, &g_prev) != 0) {
			g_fd.store(-1, std::memory_order_release);
			::close(wakeRd_); ::close(wakeWr_);
			wakeRd_ = wakeWr_ = -1;
			pollFallback_ = true;
			return;
		}
		g_installed = true;

		relay_ = std::thread([this] { relayLoop(); });

		installCancelSignal();

	}

	// Chains SIGINT in front of the program's cancellation handler so Ctrl+C
	// wakes cv waiters immediately. Skipped when no real handler is installed
	// (nothing would set g_operationCancelled anyway). The previous handler's
	// SA_RESTART bit and mask are kept so its behaviour towards blocking
	// syscalls in the worker threads does not change.
	void installCancelSignal() {

		using namespace write_screen_detail;

		struct sigaction cur {};
		if (sigaction(SIGINT, nullptr, &cur) != 0) return;
		if (!(cur.sa_flags & SA_SIGINFO) &&
		    (cur.sa_handler == SIG_DFL || cur.sa_handler == SIG_IGN)) return;

		g_prevInt = cur;

		struct sigaction sa {};
		sa.sa_sigaction = onWriteInt;
		sa.sa_flags     = SA_SIGINFO | (cur.sa_flags & SA_RESTART);
		sa.sa_mask      = cur.sa_mask;
		if (sigaction(SIGINT, &sa, nullptr) != 0) return;

		g_intInstalled = true;
		cancelChained_ = true;

	}

	void uninstallResizeSignal() {

		using namespace write_screen_detail;
		if (g_intInstalled) {
			// Restore only if our handler is still the active one; after
			// performWriteOperation's signal(SIGINT, SIG_IGN) it is not, and
			// the ignore must stay.
			struct sigaction cur {};
			if (sigaction(SIGINT, nullptr, &cur) == 0 &&
			    (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == onWriteInt)
			    sigaction(SIGINT, &g_prevInt, nullptr);
			g_intInstalled = false;
			cancelChained_ = false;
		}
		if (g_installed) {
			sigaction(SIGWINCH, &g_prev, nullptr);
			g_installed = false;
		}
		if (wakeWr_ >= 0) {
			g_fd.store(-1, std::memory_order_release);
			::close(wakeWr_);
			wakeWr_ = -1;
		}
		if (wakeRd_ >= 0) { ::close(wakeRd_); wakeRd_ = -1; }

	}

	// Blocks in read() until the handler (or stop()) writes a byte. Zero CPU
	// while idle. Turns the signal into a condition-variable wakeup.
	void relayLoop() {

		char buf[64];
		for (;;) {
			const ssize_t n = ::read(wakeRd_, buf, sizeof buf);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0 || stop_.load(std::memory_order_acquire)) break;

			// 'w' = SIGWINCH, 'c' = SIGINT. Either way the waiters are
			// notified below; only a resize sets the resize flag.
			for (ssize_t i = 0; i < n; ++i) {
				if (buf[i] == 'w') {
					resizeSignalled_.store(true, std::memory_order_release);
					break;
				}
			}
			{ std::lock_guard<std::mutex> lk(syncPtr_->mutex); }   // pairs with the keeper's predicate check
			syncPtr_->cv.notify_all();
		}

	}

	// ---- Layout --------------------------------------------------------------

	// Device names, full (untruncated) drive names and column widths cannot
	// change while the operation runs, so they are computed once, before the
	// keeper starts.
	void computeConstants() {

		std::unordered_map<std::string, std::string> deviceNames;
		for (const auto& prog : progressData_) {
			if (deviceNames.find(prog.device) == deviceNames.end())
			    deviceNames[prog.device] = getDriveName(prog.device);
		}

		rowShortNames_.reserve(progressData_.size());
		for (const auto& prog : progressData_) {
			rowShortNames_.push_back(deviceNames[prog.device]);
			maxDeviceLen_ = std::max(maxDeviceLen_, prog.device.size());
			maxNameWidth_ = std::max(maxNameWidth_, utf8Width(rowShortNames_.back()));
		}

	}

	// Header text: reprinted after every full-screen wipe. While running it is
	// the "Processing..." banner; once finished, line 2 holds the status line.
	std::string headerText(bool finished) const {

		std::ostringstream h;
		if (!finished) {
			h << "\n" << color << "Processing "
			  << (progressData_.size() > 1 ? "tasks" : "task") << " for "
			  << UI::Palette::Yellow << "write2usb" << color
			  << "... (" << UI::Palette::Red << "Ctrl+c"
			  << color << ":cancel)\n\n";
		} else {
			h << "\n" << statusLine_ << "\n\n";
		}
		return h.str();

	}

	void handleActivePhase(bool completion, std::chrono::steady_clock::time_point& lastDraw) {

		const auto size = terminalSize();
		if (size.cols == 0 || size.rows == 0) {
			lastDraw = std::chrono::steady_clock::now();            // size unknown: skip, don't spin
			return;
		}
		const bool sizeChanged = hasDrawn_ &&
		    (size.cols != lastCols_ || size.rows != lastRows_);
		const bool pulse = (std::chrono::steady_clock::now() - lastDraw) >= kPulse;
		if (sizeChanged || completion || pulse) {
			renderLocked(size, sizeChanged);
			lastDraw = std::chrono::steady_clock::now();
		}

	}

	void handleFinishedPhaseResize(bool resizeEvt) {

		if (!((resizeEvt || pollFallback_) && promptText_.empty())) {
			return;
		}
		// Finished and no prompt registered: the keeper owns resize
		// repaints. With a prompt, the resize hook calls repaint().
		const auto size = terminalSize();
		if (size.cols != 0 && size.rows != 0 &&
		    hasDrawn_ && (size.cols != lastCols_ || size.rows != lastRows_)) {
			renderLocked(size, true);
		}

	}

	void keeperLoop() {

		using clock = std::chrono::steady_clock;
		clock::time_point lastDraw{};
		std::size_t seenFinished = static_cast<std::size_t>(-1);

		while (!stop_.load(std::memory_order_acquire)) {
			bool finishedPhase = false;

			{
				std::lock_guard<std::mutex> rl(renderMutex_);

				const bool   resizeEvt   = resizeSignalled_.exchange(false, std::memory_order_acquire);
				const std::size_t finishedNow = syncPtr_->finishedTasks.load();
				const bool   completion  = (finishedNow != seenFinished);
				seenFinished = finishedNow;
				finishedPhase = (phase_ == Phase::Finished);

				if (!finishedPhase) {
					handleActivePhase(completion, lastDraw);
				} else {
					handleFinishedPhaseResize(resizeEvt);
				}
			}

			std::unique_lock<std::mutex> lk(syncPtr_->mutex);
			const auto pred = [&] {

				return stop_.load(std::memory_order_acquire) ||
				       resizeSignalled_.load(std::memory_order_acquire) ||
				       syncPtr_->finishedTasks.load() != seenFinished;

			};

			// Running: wake for the pulse. Finished: sleep until an event.
			// Fallback (no signal relay): also wake every kPoll to check size.
			if (!finishedPhase || pollFallback_) {
				auto deadline = finishedPhase ? clock::now() + kPoll : lastDraw + kPulse;
				if (pollFallback_) deadline = std::min(deadline, clock::now() + kPoll);
				syncPtr_->cv.wait_until(lk, deadline, pred);
			} else {
				syncPtr_->cv.wait(lk, pred);
			}
		}

	}

	// Convenience overload: queries the terminal size and derives the resize
	// flag itself. Used by finish() and repaint(), which render once and do
	// not already hold this information. The caller must hold renderMutex_.
	void renderLocked() {

		const auto size = terminalSize();
		if (size.cols == 0 || size.rows == 0) return;       // size unknown: skip
		const bool resized = hasDrawn_ &&
		    (size.cols != lastCols_ || size.rows != lastRows_);
		renderLocked(size, resized);

	}

	std::string buildHeaderFrame(bool finished, bool resized, std::size_t cols, std::size_t rows) {

		std::string frame;
		frame += "\033[?2026h";
		if (finished || resized) {
			// The result screen is always painted from scratch, and a resized
			// running screen is too: after a resize the terminal may have
			// reflowed or scrolled old content into scrollback, and cursor-up
			// cannot move above the visible screen.
			frame += "\033[H\033[2J\033[3J";   // home, clear screen, clear scrollback
			frame += headerText(finished);
		} else {
			// Same size as last frame: move up over the previous rows (accounting
			// for any wrapping) and clear to the end of the screen. Relative
			// movement is used instead of \033[s / \033[u because a saved absolute
			// cursor position goes stale when the terminal re-wraps.
			std::size_t up = 0;
			for (std::size_t w : lastRowWidths_)
			    up += std::max<std::size_t>(1, (w + cols - 1) / cols);
			if (up > 0) frame += "\033[" + std::to_string(up) + "A";
			frame += "\r\033[J";
		}
		lastCols_ = cols;
		lastRows_ = rows;
		hasDrawn_ = true;
		return frame;

	}

	struct RowSpeed {
		std::string text;
		bool completed;
	};

	std::string buildRowString(std::size_t i, const std::string& shortFile, const RowSpeed& speed,
	                           std::size_t maxFilenameWidth, std::size_t maxSpeedWidth, std::size_t usableCols) {

		const auto& prog = progressData_[i];

		std::string paddedFilename = shortFile;
		paddedFilename.append(maxFilenameWidth - utf8Width(shortFile), ' ');

		const std::string paddedDevice = padRight(prog.device, maxDeviceLen_);

		const std::string& name = rowShortNames_[i];
		const std::string namePad(maxNameWidth_ - utf8Width(name), ' ');

		// Writing uses half blocks; active verification uses Braille
		// patterns with half-cell glyphs. Both advance in 10% steps.
		const bool failed = prog.failed.load();
		const bool cancelled = isUsbWriteCancelled(prog.cancelToken);
		const bool verificationActive = phase_ == Phase::Running &&
		    prog.verifying.load() && !speed.completed && !failed && !cancelled;
		// Verification reuses the progress counter. On failure or cancellation
		// after a completed write, restore the write percentage instead.
		const bool restoreWriteProgress = !speed.completed &&
		    (failed || cancelled) && prog.writeCompleted.load();
		int pct = restoreWriteProgress ? 100 : std::clamp(prog.progress.load(), 0, 100);
		int steps = (pct * BAR_STEPS) / 100;
		int full  = steps / 2;
		bool half = (steps % 2) != 0;

		std::string bar, empty;
		for (int j = 0; j < full; ++j) bar += (verificationActive ? "⣶" : "▇");
		if (half)                      bar += (verificationActive ? "⡆" : "▄");
		for (int j = 0; j < BAR_WIDTH - full - (half ? 1 : 0); ++j) empty += (verificationActive ? " " : "░");

		std::ostringstream row;
		row << wt_.fileCol << paddedFilename
		    << color << " → " << wt_.deviceCol << paddedDevice
		    << color << " " << wt_.infoLabel << "<" << name << ">" << namePad
		    << color << " ["
		    << bar << wt_.speedCol << empty
		    << color << "] ";

		const bool isCompleted = speed.completed;
		const auto& speedStr = speed.text;

		if (isCompleted)                                    row << wt_.colorSuccess << (prog.verifying.load() ? "PASS " : "DONE ");
		else if (failed)                                   row << wt_.colorFailure << "FAIL ";
		else if (cancelled)                                row << wt_.colorWarning << "CXL  ";
		else {
			std::string pctStr = std::to_string(pct) + "%";
			pctStr.resize(5, ' ');
			row << color << pctStr;
		}

		const std::string speedPad(maxSpeedWidth - utf8Width(speedStr), ' ');
		row << color << speedPad << speedStr;
		if (isCompleted)
		    row << " (avg)";

		// Last line of defense: if the terminal is too narrow even for the
		// truncated row, cut the row instead of letting it wrap.
		return clipToWidth(row.str(), usableCols);

	}

	// Builds the whole frame in one string and writes it once (less flicker).
	// Terminal size and the resize flag are passed in so the renderer and the
	// keeper share exactly one source of truth for "did the size change?".
	// The caller must hold renderMutex_.
	void renderLocked(TermSize termSize, bool resized) {

		if (termSize.cols == 0 || termSize.rows == 0) return;   // also guards the division below

		const bool finished = (phase_ == Phase::Finished);

		std::size_t maxFilenameWidth = 0;
		std::size_t maxSpeedWidth    = 0;

		std::vector<RowSpeed> speeds;
		speeds.reserve(progressData_.size());
		bool anyCompleted = false;
		for (const auto& prog : progressData_) {
			const bool completed = prog.completed.load();
			const double speed = !completed && prog.verifying.load()
			    ? prog.verificationSpeed.load() : prog.speed.load();
			speeds.push_back({formatSpeed(speed), completed});
			maxSpeedWidth = std::max(maxSpeedWidth, utf8Width(speeds.back().text));
			anyCompleted |= completed;
		}
		const std::size_t avgExtra = anyCompleted ? 6 : 0;

		const std::size_t fixedWidth = 3 + maxDeviceLen_ + (maxNameWidth_ + 3) + 2 + BAR_WIDTH
		                        + 2 + 5 + maxSpeedWidth + avgExtra;

		const std::size_t cols = termSize.cols;
		const std::size_t rows = termSize.rows;
		const std::size_t usableCols = cols > 1 ? cols - 1 : 1;

		const std::size_t allowedFilenameWidth =
		    usableCols > fixedWidth ? usableCols - fixedWidth : 0;

		std::vector<std::string> shortFiles;
		shortFiles.reserve(progressData_.size());
		for (const auto& prog : progressData_) {
			shortFiles.push_back(truncateIsoName(prog.filename, allowedFilenameWidth));
			maxFilenameWidth = std::max(maxFilenameWidth, utf8Width(shortFiles.back()));
		}

		std::string frame = buildHeaderFrame(finished, resized, cols, rows);
		lastRowWidths_.clear();
		lastRowWidths_.reserve(progressData_.size());

		for (std::size_t i = 0; i < progressData_.size(); ++i) {
			const std::string line = buildRowString(i, shortFiles[i], speeds[i],
			                                        maxFilenameWidth, maxSpeedWidth, usableCols);
			lastRowWidths_.push_back(std::min(visibleWidth(line), usableCols));
			frame += line;
			frame += "\033[0m\n";
		}

		if (finished) {
			frame += summaryText_;
			frame += "\033[?25h";            // show the cursor again
		}
		frame += "\033[?2026l";              // end synchronized update
		std::cout << frame << std::flush;

	}

	std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr_;
	std::vector<ProgressInfo>&                 progressData_;
	std::shared_ptr<WriteSync>                 syncPtr_;
	const WriteTheme                           wt_;

	// Written once in computeConstants() before the keeper thread starts.
	// rowShortNames_ holds the full, never-truncated drive names.
	std::vector<std::string> rowShortNames_;
	std::size_t maxDeviceLen_ = 0;
	std::size_t maxNameWidth_ = 0;

	// Everything below is guarded by renderMutex_.
	std::mutex          renderMutex_;
	Phase               phase_ = Phase::Running;
	std::string         statusLine_;
	std::string         summaryText_;
	std::string         promptText_;     // non-empty = the resize hook owns repainting
	// Visible width (in columns) of every row printed by the previous frame.
	// Used to work out how far to move the cursor back up, even if the
	// terminal was resized (and therefore re-wrapped the old rows) in between.
	std::vector<std::size_t> lastRowWidths_;
	// Terminal size at the time of the previous frame. A change in either
	// dimension triggers a full wipe + repaint instead of a cursor-up redraw.
	std::size_t              lastCols_ = 0, lastRows_ = 0;
	bool                hasDrawn_ = false;

	// Resize relay. wakeRd_/wakeWr_/pollFallback_ are set in start() before
	// the threads run and only cleared after they are joined.
	int                 wakeRd_ = -1, wakeWr_ = -1;
	bool                pollFallback_ = false;
	bool                cancelChained_ = false;   // SIGINT relayed into the cv
	std::atomic<bool>   resizeSignalled_{false};
	std::thread         relay_;

	std::atomic<bool>   stop_{false};
	std::thread         thread_;
};

// Opaque WriteScreen interface declared in the write2usbUI module.
std::shared_ptr<WriteScreen> createWriteScreen(
        std::shared_ptr<std::vector<ProgressInfo>> progressData,
        std::shared_ptr<WriteSync> sync, WriteTheme theme) {

	return std::make_shared<WriteScreen>(std::move(progressData), std::move(sync), std::move(theme));

}

void startWriteScreen(WriteScreen& screen) {

	screen.start();

}

void finishWriteScreen(WriteScreen& screen, std::chrono::steady_clock::duration elapsed) {

	screen.finish(elapsed);

}

bool writeScreenCancelWakeable(const WriteScreen& screen) {

	return screen.cancelWakeable();

}

void setWriteScreenPrompt(WriteScreen& screen, std::string prompt) {

	screen.setPrompt(std::move(prompt));

}

void repaintWriteScreen(WriteScreen& screen) {

	screen.repaint();

}

void stopWriteScreen(WriteScreen& screen) {

	screen.stop();

}
