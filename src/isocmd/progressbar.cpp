// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../inputHandling.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"

/**
 * @brief DEC private mode 2026 (synchronized output) control sequences.
 *
 * Wrapping a frame in kSyncBegin/kSyncEnd tells supporting terminals to hold
 * rendering until the whole frame has arrived, then present it atomically,
 * which removes flicker on redraws. Terminals without support ignore the
 * sequences, so they are safe to emit unconditionally.
 *
 * The mode is a plain on/off flag, not a counter: nested begin/end pairs do
 * not stack, and the first end marker closes the block.
 */
namespace {
constexpr bool             kUseSynchronizedOutput = true;
constexpr std::string_view kSyncBegin = "\033[?2026h";
constexpr std::string_view kSyncEnd   = "\033[?2026l";
}

namespace {

/**
 * @brief Background watcher that tracks the terminal size and wakes the
 *        render loop on a change.
 *
 * A watcher thread polls TIOCGWINSZ on stdout every kPoll. The size is kept
 * as one packed std::atomic<uint64_t> (cols << 32 | rows), so readers never
 * see a torn cols/rows pair.
 *
 * @par Hardening
 * A query that fails or reports a zero dimension (detached or transiently
 * resized pty) never overwrites the stored size, so a transient failure is
 * not mistaken for a resize. Before the first success the size is 80x24
 * (e.g. stdout is not a tty).
 *
 * @par Wake-up
 * When the stored size changes, the watcher writes one byte to a
 * non-blocking self-pipe. The render loop polls the read end together with
 * stdin, so a resize is repainted at once rather than on the next timeout.
 * If the pipe cannot be created the wake fd is -1 (ignored by poll()) and
 * the loop's own timeout still picks the change up.
 *
 * @par Threading
 * The watcher thread only updates the atomic and writes to the pipe; it
 * never touches the terminal output. All drawing stays on the caller's
 * thread.
 */
class TerminalSizeWatcher {
public:
    struct Size { size_t cols; size_t rows; };

    TerminalSizeWatcher() {
        if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) wake_[0] = wake_[1] = -1;
        update();
        thread_ = std::thread([this] { loop(); });
    }

    ~TerminalSizeWatcher() {
        stop();
        for (int fd : wake_) if (fd >= 0) close(fd);
    }

    TerminalSizeWatcher(const TerminalSizeWatcher&)            = delete;
    TerminalSizeWatcher& operator=(const TerminalSizeWatcher&) = delete;

    /// Last known good size (never 0x0, never torn).
    Size get() const {
        const uint64_t v = packed_.load(std::memory_order_relaxed);
        return { static_cast<size_t>(v >> 32), static_cast<size_t>(v & 0xFFFFFFFFu) };
    }

    /// Queries the terminal synchronously on the calling thread and returns
    /// the result. Use where the very latest size matters (e.g. just before
    /// the final report).
    Size refresh() { update(); return get(); }

    /// Read end of the self-pipe for poll(), or -1 if unavailable.
    int wakeFd() const { return wake_[0]; }

    /// Empties the self-pipe after poll() reported it readable.
    void drainWake() const {
        if (wake_[0] < 0) return;
        char buf[64];
        while (read(wake_[0], buf, sizeof buf) > 0) {}
    }

    /// Stops and joins the watcher thread. Safe to call more than once.
    void stop() {
        if (!thread_.joinable()) return;
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        thread_.join();
    }

private:
    static constexpr auto kPoll = std::chrono::milliseconds(50);

    /// Queries the terminal; stores and returns true only if a valid,
    /// different size was read.
    bool update() {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
            return false;
        const uint64_t v = (uint64_t{ws.ws_col} << 32) | uint64_t{ws.ws_row};
        return packed_.exchange(v, std::memory_order_relaxed) != v;
    }

    void wake() const {
        if (wake_[1] < 0) return;
        const char b = 1;
        // A full pipe (EAGAIN) already means "a wake-up is pending", so the
        // result is ignored.
        (void)!write(wake_[1], &b, 1);
    }

    void loop() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            cv_.wait_for(lk, kPoll, [&] { return stop_; });
            if (stop_) break;
            if (update()) wake();
        }
    }

    std::atomic<uint64_t>   packed_{(uint64_t{80} << 32) | 24u};
    int                     wake_[2] = {-1, -1};
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    std::thread             thread_;
};

/// Repaint callback used by the readline event hook while the verbose prompt
/// waits. rl_event_hook is a plain function pointer and cannot carry state,
/// so the active callback is reached through this pointer (set only around
/// that one readline() call).
std::function<int()>* g_progressResizeHook = nullptr;

/// rl_event_hook trampoline: forwards to the active callback, if any.
int progressResizeHookTrampoline() {
    return g_progressResizeHook ? (*g_progressResizeHook)() : 0;
}

} // namespace

/**
 * @brief Terminal progress bar with input blocking and themed output.
 */

struct termios oldt;
int oldf;

/**
 * @brief Disables terminal canonical mode and echoing to block user input
 *        during progress updates.
 *
 * @param oldt Pointer to store original terminal settings.
 * @param oldf Pointer to store original file descriptor flags.
 */
void disableInputForProgressBar(struct termios *oldt, int *oldf) {
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
void restoreInput(struct termios *oldt, int oldf) {
    tcsetattr(STDIN_FILENO, TCSANOW, oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
}

/**
 * @brief Displays a multi-line progress bar with byte tracking, task counts,
 *        and elapsed time.
 *
 * @par Terminal width
 * The bar width is configured per operation (42 / 46 / 50 columns while
 * running, 30 / 41 in the final layout) but is shrunk whenever the terminal
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
 * A TerminalSizeWatcher thread polls the size every 50 ms and wakes the
 * render loop through a self-pipe when it changes, so no SIGWINCH handler is
 * needed and a resize is repainted immediately. The loop also still renders
 * every 100 ms for live values. The watcher only reports sizes; all drawing
 * happens on the calling thread. Redrawing relies on moving the cursor up
 * over the previous frame, which cannot be trusted after a resize: the
 * terminal may have reflowed the old lines or pushed them into scrollback,
 * where cursor movement cannot reach. When the size changes, the screen and
 * scrollback are wiped, the caller-supplied @p header ("Processing ...
 * (Ctrl+c:cancel)") is reprinted, and the block is redrawn below it, so the
 * header line that the final "Status:" report overwrites is preserved. The
 * same check runs once more when the operation completes, so a resize between
 * the last frame and the final report is handled.
 *
 * @par Resize at the verbose prompt
 * The watcher keeps running while the "Display verbose output?" prompt waits.
 * An rl_event_hook (called by readline from its idle loop, on this thread)
 * compares the watcher's size with the last painted one and, on a change,
 * wipes the screen, repaints the final report (header, status, bar, with the
 * elapsed time and counters frozen at completion) and then calls
 * rl_on_new_line() + rl_redisplay() so the prompt and any half-typed answer
 * are restored. The whole repaint is one synchronized update.
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
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes,
    std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks,
    std::atomic<bool>* isComplete, bool* verbose, const std::string& operation,
    const std::string& header) {

    ProgressBarColors pc = resolveProgressTheme();

    disableInputForProgressBar(&oldt, &oldf);

    int processingBarWidth = 42;
    int finalBarWidth = 30;

    if (operation.find("mount") != std::string::npos || operation.find("umount") != std::string::npos) {
        processingBarWidth = 46;
    } else if (operation.find("2iso") != std::string::npos) {
        processingBarWidth = 48;
    }

    constexpr size_t MIN_BAR_WIDTH = 10;

    auto startTime = std::chrono::high_resolution_clock::now();
    const bool bytesTrackingEnabled = (completedBytes != nullptr);

    auto formatSize = [](double bytes) -> std::string {
        std::stringstream ss;
        const char* units[] = {" B", " KB", " MB", " GB"};
        int unit = 0;
        double size = bytes;
        while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
        ss << std::fixed << std::setprecision(2) << size << units[unit];
        return ss.str();
    };

    std::string totalBytesFormatted;
    size_t lastCompletedBytes = SIZE_MAX;
    std::string cachedCompletedBytesFormatted = "0.00 B";
    std::string cachedSpeedFormatted = "0.00 B/s";

    if (bytesTrackingEnabled)
        totalBytesFormatted = formatSize(static_cast<double>(totalBytes));

    using TermSize = TerminalSizeWatcher::Size;

    TerminalSizeWatcher watcher;

    auto sameSize = [](const TermSize& a, const TermSize& b) {
        return a.cols == b.cols && a.rows == b.rows;
    };

    auto numDigits = [](size_t n) -> size_t {
        size_t d = 1;
        while (n >= 10) { n /= 10; ++d; }
        return d;
    };

    auto clipToWidth = [](const std::string& s, size_t maxCols) -> std::string {
        std::string out;
        out.reserve(s.size());
        size_t w = 0;
        for (size_t i = 0; i < s.size(); ) {
            if (static_cast<unsigned char>(s[i]) == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
                size_t j = i + 2;
                while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j;
                if (j < s.size()) ++j;
                out.append(s, i, j - i);
                i = j;
                continue;
            }
            size_t len = 1;
            while (i + len < s.size() &&
                (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
            if (w >= maxCols) break;
            out.append(s, i, len);
            i += len;
            ++w;
        }
        return out;
    };

    TermSize curSize  = watcher.get();
    TermSize lastSize = curSize;
    bool hasDrawn = false;

    auto clipText = [&](const std::string& text, size_t maxCols) -> std::string {
        std::string out;
        size_t start = 0;
        while (true) {
            const size_t nl = text.find('\n', start);
            if (nl == std::string::npos) {
                out += clipToWidth(text.substr(start), maxCols);
                break;
            }
            out += clipToWidth(text.substr(start, nl - start), maxCols);
            out += '\n';
            start = nl + 1;
        }
        return out;
    };

    auto resizeHeaderText = [&]() -> std::string {
        const size_t usableCols = curSize.cols > 1 ? curSize.cols - 1 : 1;
        return header.empty() ? std::string("\n") : clipText(header, usableCols);
    };

    int lastRenderedLines = 1;

    double frozenElapsedSeconds = -1.0;

    bool   countersFrozen        = false;
    size_t frozenCompletedTasks  = 0;
    size_t frozenFailedTasks     = 0;
    size_t frozenCompletedBytes  = 0;

    auto renderProgressBar = [&](bool forceFull = false, bool useFinalLayout = false, bool cancelled = false) -> std::string {
        const size_t completedTasksValue = countersFrozen ? frozenCompletedTasks
            : completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue    = countersFrozen ? frozenFailedTasks
            : failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = countersFrozen ? frozenCompletedBytes
            : (bytesTrackingEnabled ? completedBytes->load(std::memory_order_acquire) : 0);

        double tasksProgress = cancelled
            ? static_cast<double>(completedTasksValue) / totalTasks
            : static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;

        double overallProgress = tasksProgress;

        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = cancelled ? bytesProgress : std::max(bytesProgress, tasksProgress);
        }

        if (forceFull && !cancelled) overallProgress = 1.0;

        auto currentTime    = std::chrono::high_resolution_clock::now();
        double elapsedSeconds = frozenElapsedSeconds >= 0.0
            ? frozenElapsedSeconds
            : std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count() / 1000.0;

        if (bytesTrackingEnabled && completedBytesValue != lastCompletedBytes) {
            cachedCompletedBytesFormatted = formatSize(static_cast<double>(completedBytesValue));
            double speed = elapsedSeconds > 0.0 ? static_cast<double>(completedBytesValue) / elapsedSeconds : 0.0;
            cachedSpeedFormatted = formatSize(speed) + "/s";
            lastCompletedBytes = completedBytesValue;
        }

        std::string timeStr, pctStr;
        {
            std::ostringstream t;
            t << std::fixed << std::setprecision(1) << elapsedSeconds;
            timeStr = t.str();
            std::ostringstream p;
            p << std::fixed << std::setprecision(0) << (overallProgress * 100.0);
            pctStr = p.str();
        }

        const size_t usableCols = curSize.cols > 1 ? curSize.cols - 1 : 1;

        const size_t taskDigits = numDigits(totalTasks);
        const size_t timeWidth  = std::max<size_t>(timeStr.size(), 5);
        const size_t line1Fixed = 1 + 2 + 3 + 3 + taskDigits + 1 + taskDigits + 8 + timeWidth + 1;
        size_t maxBar = usableCols > line1Fixed ? usableCols - line1Fixed : 0;

        if (bytesTrackingEnabled) {
            const size_t sizeWidth = std::max<size_t>(totalBytesFormatted.size(), 10);
            const size_t line2Text = 11 + sizeWidth + 1 + sizeWidth;
            const size_t line3Text = 7 + 10 + 2 + 6;
            const size_t textW     = std::max(line2Text, line3Text);
            const size_t avail     = usableCols > 3 + textW ? usableCols - 3 - textW : 0;
            maxBar = std::min(maxBar, avail);
        }

        const size_t configuredBar = static_cast<size_t>(useFinalLayout ? finalBarWidth : processingBarWidth);
        const int barWidth    = static_cast<int>(std::min(configuredBar, std::max(MIN_BAR_WIDTH, maxBar)));
        const int progressPos = static_cast<int>(barWidth * overallProgress);

        std::vector<std::string> lines;
        lines.reserve(3);

        {
            std::ostringstream l1;
            l1 << color << "[" << color;
            for (int i = 0; i < barWidth; ++i) {
                l1 << (i < progressPos ? "=" : (i == progressPos && !useFinalLayout ? ">" : " "));
                if (i == progressPos && !useFinalLayout) l1 << color;
            }
            l1 << color << "] " << pctStr
               << "% (" << completedTasksValue << "/" << totalTasks << ") Time: "
               << timeStr << "s";
            lines.push_back(l1.str());
        }

        if (bytesTrackingEnabled) {
            auto indentFor = [&](size_t textW) -> size_t {
                size_t pad = static_cast<size_t>(barWidth) + 3;
                if (pad + textW > usableCols)
                    pad = usableCols > textW ? usableCols - textW : 0;
                return pad;
            };

            const std::string processed = "Processed: " +
                ((useFinalLayout && !cancelled) ? totalBytesFormatted : cachedCompletedBytesFormatted) +
                "/" + totalBytesFormatted;
            lines.push_back(std::string(indentFor(processed.size()), ' ') + processed);

            std::string speedLine = "Speed: ";
            if (useFinalLayout && !cancelled) {
                double avgSpeed = elapsedSeconds > 0.0 ? static_cast<double>(totalBytes) / elapsedSeconds : 0.0;
                speedLine += formatSize(avgSpeed) + "/s (avg)";
            } else {
                speedLine += cachedSpeedFormatted;
            }
            lines.push_back(std::string(indentFor(speedLine.size()), ' ') + speedLine);
        }

        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) out += '\n';
            out += "\r\033[2K";
            out += clipToWidth(lines[i], usableCols);
        }

        lastRenderedLines = static_cast<int>(lines.size());
        return out;
    };

    std::cout << "\033[?25l" << std::flush;

    bool enterPressed = false;
    struct pollfd pfds[2] = {
        { STDIN_FILENO,    POLLIN, 0 },
        { watcher.wakeFd(), POLLIN, 0 },
    };

    while (!enterPressed) {
        if (poll(pfds, 2, 100) > 0) {
            if (pfds[0].revents & POLLIN) {
                char ch;
                while (read(STDIN_FILENO, &ch, 1) > 0);
            }
            if (pfds[1].revents & POLLIN) watcher.drainWake();
        }

        curSize = watcher.get();
        const bool resized = hasDrawn && !sameSize(curSize, lastSize);

        std::string frame;
        if (kUseSynchronizedOutput) frame.append(kSyncBegin);

        if (resized) {
            frame += "\033[H\033[2J\033[3J";
            frame += resizeHeaderText();
        }
        frame += renderProgressBar(false, false, false);
        lastSize = curSize;
        hasDrawn = true;

        const bool done = isComplete->load(std::memory_order_acquire);

        if (bytesTrackingEnabled && !done && lastRenderedLines > 1) {
            frame += "\033[" + std::to_string(lastRenderedLines - 1) + "A";
        }

        if (kUseSynchronizedOutput) frame.append(kSyncEnd);
        std::cout << frame << std::flush;

        if (done) {
            signal(SIGINT, SIG_IGN);

            const TermSize nowSize = watcher.refresh();
            const bool resizedAtEnd = !sameSize(nowSize, lastSize);
            curSize  = nowSize;
            lastSize = nowSize;

            frozenElapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - startTime).count() / 1000.0;
            frozenCompletedTasks = completedTasks->load(std::memory_order_acquire);
            frozenFailedTasks    = failedTasks->load(std::memory_order_acquire);
            frozenCompletedBytes = bytesTrackingEnabled ? completedBytes->load(std::memory_order_acquire) : 0;
            countersFrozen       = true;

            const size_t completedTasksValue = frozenCompletedTasks;
            const size_t failedTasksValue    = frozenFailedTasks;
            const bool wasCancelled          = GlobalState::g_operationCancelled.load(std::memory_order_acquire);
            bool snapTo100 = (!wasCancelled && failedTasksValue == 0);

            std::ostringstream fin;
            if (kUseSynchronizedOutput) fin << kSyncBegin;

            if (resizedAtEnd) {
                fin << "\033[H\033[2J\033[3J" << resizeHeaderText() << "\033[1A";
            } else {
                fin << (bytesTrackingEnabled ? "\033[1J\033[3A" : "\033[1J\033[1A");
            }

            auto statusAndBar = [&]() -> std::string {
                std::ostringstream s;
                s << "\r\033[2K" << color << " Status: " << operation << color << " → ";
                if (wasCancelled) {
                    s << pc.warning << "INTERRUPTED";
                } else if (failedTasksValue > 0) {
                    s << (completedTasksValue > 0 ? pc.warning : pc.failure)
                      << (completedTasksValue > 0 ? "PARTIAL" : "FAILED");
                } else {
                    s << pc.success << "COMPLETED";
                }
                s << pc.reset << '\n';
                s << renderProgressBar(snapTo100, true, wasCancelled);
                return s.str();
            };
            fin << statusAndBar();

            if (kUseSynchronizedOutput) fin << kSyncEnd;
            std::cout << fin.str() << std::flush;

            disableReadlineForConfirmation();
            enterPressed = true;
            std::cout << "\n\n";
            restoreInput(&oldt, oldf);
            reset_custom_keybindingsForSelect();

            if (!GlobalState::isNonInteractive.load()) {
                std::cout << "\033[?25h" << std::flush;

                constexpr int kMinSafeCols = 8;

                const std::string fullText  = "Display verbose output? (y/n): ";
                const std::string shortText = "Verbose? (y/n): ";
                const std::string tinyText  = "y/n: ";

                auto makePrompt = [&](const std::string& text) -> std::string {
                    return "\001" + std::string(color) + "\002" +
                           text +
                           "\001" + std::string(pc.reset) + "\002";
                };
                auto promptFor = [&](int cols) -> std::string {
                    if (cols >= static_cast<int>(fullText.size())  + 2) return makePrompt(fullText);
                    if (cols >= static_cast<int>(shortText.size()) + 2) return makePrompt(shortText);
                    return makePrompt(tinyText);
                };

                const TermSize startSize = watcher.get();
                lastSize = startSize;
                curSize  = startSize;

                std::string res;

                if (startSize.cols < kMinSafeCols) {
                    std::cout << promptFor(startSize.cols) << std::flush;
                    std::getline(std::cin, res);
                } else {
                    std::string prompt = promptFor(startSize.cols);

                    std::function<int()> onResize = [&]() -> int {
                        const TermSize now = watcher.get();
                        if (sameSize(now, lastSize)) return 0;

                        curSize  = now;
                        lastSize = now;

                        if (now.cols < kMinSafeCols) return 0;

                        rl_set_screen_size(static_cast<int>(now.rows),
                                           static_cast<int>(now.cols));

                        const std::string newPrompt = promptFor(now.cols);
                        if (newPrompt != prompt) {
                            prompt = newPrompt;
                            rl_set_prompt(prompt.c_str());
                        }

                        std::string body;
                        if (kUseSynchronizedOutput)
                            body.append(kSyncBegin);

                        body += "\033[H\033[2J\033[3J";
                        body += resizeHeaderText();
                        body += "\033[1A";
                        body += statusAndBar();
                        body += "\n\n";

                        std::cout << body << std::flush;

                        rl_on_new_line();
                        rl_forced_update_display();

                        if (kUseSynchronizedOutput)
                            std::cout << kSyncEnd << std::flush;

                        return 0;
                    };

                    g_progressResizeHook = &onResize;
                    auto*    savedHook  = rl_event_hook;
                    const int savedWinch = rl_catch_sigwinch;

                    rl_event_hook     = progressResizeHookTrampoline;
                    rl_catch_sigwinch = 0;

                    std::unique_ptr<char, decltype(&std::free)> input(
                        readline(prompt.c_str()), &std::free);

                    rl_catch_sigwinch    = savedWinch;
                    rl_event_hook        = savedHook;
                    g_progressResizeHook = nullptr;

                    if (input.get())
                        res = input.get();
                }
                *verbose = (res == "y" || res == "Y");
            } else {
                disableInputForProgressBar(&oldt, &oldf);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                restoreInput(&oldt, oldf);
                std::cout << "\033[?25h" << std::flush;
                *verbose = true;
            }

            watcher.stop();

            restoreReadline();
        }
    }
    std::cout << '\n';
}
