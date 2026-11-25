// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.bar.progressBarSizeWatcher;

import std;
import isocmd.ui.cli.termSize;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ui/bar/progressBarSizeWatcher ----

/**
 * @brief Tracks terminal dimensions and wakes the progress render loop
 *        immediately when SIGWINCH is received.
 *
 * Installs a SIGWINCH handler that marks the size dirty and writes to a
 * non-blocking self-pipe. The progress render loop polls the pipe together
 * with stdin, avoiding the need for a background polling thread.
 *
 * Terminal dimensions are re-read only when the dirty flag is set, unless
 * fallback polling is active. The dimensions are stored in a packed atomic
 * value so readers never observe a torn columns/rows pair.
 *
 * @par Hardening
 * Failed queries and zero dimensions never overwrite the last known good
 * size. Before the first successful query, the fallback is 80x24.
 *
 * @par Fallback
 * If the pipe or signal handler cannot be installed, or another progress
 * watcher is already installed, get() re-reads the dimensions on every call.
 * The render loop's normal timeout still detects terminal changes.
 *
 * @par Threading
 * No background thread is created. All terminal drawing remains on the
 * calling thread. Only one ProgressBarSizeWatcher may be installed at a time.
 *
 * @par Close/write ordering
 * stop() restores the previous SIGWINCH handler, disarms the pipe descriptor,
 * waits for an in-flight handler write to finish, and then closes the pipe.
 */
class ProgressBarSizeWatcher {
public:
	/// Creates the watcher and installs the SIGWINCH handler.
	ProgressBarSizeWatcher();
	~ProgressBarSizeWatcher();

	ProgressBarSizeWatcher(const ProgressBarSizeWatcher&) = delete;
	ProgressBarSizeWatcher& operator=(const ProgressBarSizeWatcher&) = delete;

	/**
	 * @brief Returns the last known good terminal dimensions.
	 *
	 * Re-reads the terminal only when SIGWINCH has marked the size dirty
	 * or fallback polling is active.
	 */
	TermSize get();

	/**
	 * @brief Forces a synchronous terminal-size query.
	 *
	 * Useful immediately before printing the final report.
	 */
	TermSize refresh();

	/// Read end of the self-pipe for poll(), or -1 if unavailable.
	int wakeFd() const {

		return wake_[0];

	}

	/// Empties the self-pipe after poll() reports it readable.
	void drainWake() const;

	/**
	 * @brief Restores the previous SIGWINCH handler and closes the pipe.
	 *
	 * Safe to call more than once.
	 */
	void stop();

private:
	bool update();
	void closeWake();
	TermSize unpack() const;

	std::atomic<std::uint64_t> packed_{(std::uint64_t{80} << 32) | 24u};

	int wake_[2] = {-1, -1};

	bool installed_ = false;
	bool polling_ = false;
};

} // extern "C++"
