
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <readline/readline.h>

import std;
import isocmd.input.inputHandling;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;

// Declarations used only by this file (moved out of the module interface).
void pressEnterTo(const std::string& prompt);
void pressEnterToWithRepaint(const std::string& prompt,
                             const std::function<void()>& repaint);

namespace {

    /** @brief Set to 1 by the SIGWINCH handler; checked on the main loop. */
    volatile std::sig_atomic_t g_winch = 0;

    /** @brief SIGWINCH handler: just flags that the terminal was resized. */
    void handle_winch(int) {
        g_winch = 1;
    }

    /**
     * @brief Restores std::cout to a usable state after a transient write
     *        error and releases the terminal from potentially incomplete
     *        synchronized-update or rendering state.
     */
    void recoverCoutIfNeeded() {
        if (std::cout.good())
            return;

        std::cout.clear();
        std::cout << "\033[?2026l"
                  << "\033[0m"
                  << "\033[r"
                  << "\033[?7h"
                  << "\033[?25h"
                  << std::flush;
    }

    /**
     * @brief RAII guard to safely configure and restore terminal raw mode settings.
     */
    struct TerminalGuard {
        struct termios saved{};
        bool active = false;

        TerminalGuard() = default;

        bool activate() {
            if (tcgetattr(STDIN_FILENO, &saved) < 0)
                return false;

            struct termios raw = saved;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;

            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
                return false;

            active = true;
            return true;
        }

        ~TerminalGuard() {
            if (active) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            }
        }

        bool ok() const { return active; }
    };

    /**
     * @brief RAII guard to manage SIGWINCH signal blocking and action handlers.
     */
    struct SigwinchGuard {
        struct sigaction old_sa{};
        sigset_t old_mask{};
        bool mask_active = false;
        bool action_active = false;

        SigwinchGuard() = default;

        bool activate(void (*handler)(int)) {
            sigset_t block;
            sigemptyset(&block);
            sigaddset(&block, SIGWINCH);

            if (sigprocmask(SIG_BLOCK, &block, &old_mask) < 0)
                return false;

            mask_active = true;

            struct sigaction sa{};
            sa.sa_handler = handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;

            if (sigaction(SIGWINCH, &sa, &old_sa) < 0)
                return false;

            action_active = true;
            return true;
        }

        ~SigwinchGuard() {
            if (action_active)
                sigaction(SIGWINCH, &old_sa, nullptr);

            if (mask_active)
                sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        }

        bool ok() const { return action_active; }
        sigset_t getOldMask() const { return old_mask; }
    };

    /**
     * @brief Synchronizes Readline's screen dimensions with the current terminal size.
     */
     void syncReadlineScreenSize() {
        const TermSize sz = terminalSize();
        if (sz.cols != 0 && sz.rows != 0) {
            rl_set_screen_size(
                static_cast<int>(sz.rows),
                static_cast<int>(sz.cols)
            );
        }
    }

    /**
     * @brief Draws the prompt layout inside a synchronized output block and flushes stdout.
     *
     * @param prompt      Prompt text rendered after a leading newline.
     * @param repaint     Optional callback invoked before the prompt to redraw preceding content.
     * @param clearScreen When true, emits ANSI cursor-home and scrollback/screen clear sequences
     *                    before redrawing.
     */
     void renderPromptFrame(const std::string& prompt,
                                  const std::function<void()>* repaint,
                                  bool clearScreen) {
        syncReadlineScreenSize();
        {
            SyncOutput sync;
            if (clearScreen) {
                std::cout << "\033[H\033[2J\033[3J";
            }
            if (repaint && *repaint) {
                (*repaint)();
            }
            std::cout << "\n"
                      << color
                      << prompt
                      << UI::Palette::BoldReset;
        }
        std::cout.flush();
    }

    /**
     * @brief Checks whether a byte should dismiss the prompt.
     *
     * Recognizes LF, CR, Ctrl+D (EOT, ASCII 4), and Esc (ASCII 27).
     *
     * @param ch Input byte read from standard input.
     * @return true if the byte terminates the prompt loop; otherwise false.
     */
     bool isDismissKey(char ch) {
        return ch == '\n' || ch == '\r' || ch == 4 || ch == 27;
    }

    /**
     * @brief Waits for standard input readiness with an atomic signal mask and reads one byte.
     *
     * @param waitMask Signal mask applied during ppoll() to safely unmask SIGWINCH.
     * @param outCh    Receives the byte read when input is available.
     * @return  1 if a byte was read into @p outCh,
     *          0 if input ended or encountered a non-recoverable poll/read error,
     *         -1 if interrupted by a signal or woken without POLLIN and ready to retry.
     */
     int pollAndReadKey(const sigset_t& waitMask, char& outCh) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        const int r = ppoll(&pfd, 1, nullptr, &waitMask);

        if (r < 0) {
            return (errno == EINTR) ? -1 : 0;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return 0;
        }

        if (!(pfd.revents & POLLIN)) {
            return -1;
        }

        const ssize_t n = read(STDIN_FILENO, &outCh, 1);
        if (n < 0) {
            return (errno == EINTR) ? -1 : 0;
        }

        return (n == 0) ? 0 : 1;
    }

    /**
     * @brief Core implementation of the "press Enter to continue" prompt.
     *
     * Switches the terminal to raw mode (disabling canonical buffering and echo)
     * so that special keys such as Enter, Esc, and Ctrl+D are caught immediately.
     * Resize events trigger a full redraw using escape sequences (\033[H\033[2J\033[3J)
     * wrapped in synchronized updates (SyncOutput) to prevent flicker. SIGWINCH
     * signals are safely handled without race conditions using atomic unmasking via ppoll().
     *
     * @param prompt  Text to display after a leading newline.
     * @param repaint Optional callback function to redraw content preceding the prompt. May be null.
     */
    void pressEnterToImpl(const std::string& prompt,
                          const std::function<void()>* repaint) {
        enable_ctrl_d();
        recoverCoutIfNeeded();

        SigwinchGuard sigGuard;
        TerminalGuard termGuard;

        if (!termGuard.activate()) {
            return;
        }

        if (!sigGuard.activate(handle_winch)) {
            return;
        }

        // SIGWINCH is now blocked, so resetting the flag is race-free.
        g_winch = 0;

        sigset_t waitMask = sigGuard.getOldMask();
        sigdelset(&waitMask, SIGWINCH);

        renderPromptFrame(prompt, repaint, false);

        bool done = false;
        while (!done) {
            recoverCoutIfNeeded();

            if (g_winch) {
                g_winch = 0;
                renderPromptFrame(prompt, repaint, true);
            }

            char ch = '\0';
            const int status = pollAndReadKey(waitMask, ch);
            if (status < 0) {
                continue;
            }
            if (status == 0 || isDismissKey(ch)) {
                done = true;
            }
        }

        // Clean up and restore output state for subsequent lines.
        std::cout << '\n';
        std::cout.flush();
    }

} // namespace

/**
 * @brief Waits for Enter, Esc or Ctrl+D after showing "\n<prompt>".
 *
 * On resize the screen is cleared and just the prompt is redrawn.
 *
 * @param prompt Text to show after the leading newline.
 */
void pressEnterTo(const std::string& prompt) {
    pressEnterToImpl(prompt, nullptr);
}

/**
 * @brief Like pressEnterTo(), but @p repaint redraws the caller's content
 *        on resize.
 *
 * @param prompt  Text to show after the leading newline.
 * @param repaint Callback that redraws everything printed before the prompt.
 */
void pressEnterToWithRepaint(const std::string& prompt,
                             const std::function<void()>& repaint) {
    pressEnterToImpl(prompt, &repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToTry().
 */
void pressEnterToTryWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to try again...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToReturn().
 */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to return...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToContinue().
 */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to continue...", repaint);
}
