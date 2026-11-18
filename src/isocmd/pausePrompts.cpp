
// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cerrno>
#include <functional>
#include <string>
#include <iostream>

// C / System Headers
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../inputHandling.h"
#include "../rendering.h"
#include "../themes.h"

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

        TerminalGuard() {
            if (tcgetattr(STDIN_FILENO, &saved) == 0) {
                struct termios raw = saved;
                raw.c_lflag &= ~(ICANON | ECHO);
                raw.c_cc[VMIN]  = 1;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
                    active = true;
                }
            }
        }

        ~TerminalGuard() {
            if (active) {
                tcflush(STDIN_FILENO, TCIFLUSH);
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
        bool active = false;

        explicit SigwinchGuard(void (*handler)(int)) {
            sigset_t block;
            sigemptyset(&block);
            sigaddset(&block, SIGWINCH);

            if (sigprocmask(SIG_BLOCK, &block, &old_mask) < 0)
                return;

            struct sigaction sa{};
            sa.sa_handler = handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0; // No SA_RESTART: ppoll() must return EINTR.

            if (sigaction(SIGWINCH, &sa, &old_sa) < 0) {
                sigprocmask(SIG_SETMASK, &old_mask, nullptr);
                return;
            }
            active = true;
        }

        ~SigwinchGuard() {
            if (active) {
                sigaction(SIGWINCH, &old_sa, nullptr);
                sigprocmask(SIG_SETMASK, &old_mask, nullptr);
            }
        }

        bool ok() const { return active; }
        sigset_t getOldMask() const { return old_mask; }
    };

    /**
     * @brief Core implementation of the "press Enter to continue" prompt.
     *
     * Switches the terminal to raw mode (disabling canonical buffering and echo)
     * so that special keys such as Enter, Esc, and Ctrl+D are caught immediately.
     *
     * Resize events trigger a full redraw using escape sequences (\033[H\033[2J\033[3J)
     * wrapped in synchronized updates (SyncOutput) to prevent flicker. SIGWINCH
     * signals are safely handled without race conditions using atomic unmasking via ppoll().
     *
     * @param prompt   Text to display after a leading newline.
     * @param repaint  Optional callback function to redraw content preceding the prompt. May be null.
     */
    void pressEnterToImpl(const std::string& prompt,
                          const std::function<void()>* repaint) {
        enable_ctrl_d();
        recoverCoutIfNeeded();

        SigwinchGuard sigGuard(handle_winch);
        if (!sigGuard.ok())
            return;

        // SIGWINCH is now blocked, so resetting the flag is race-free.
        g_winch = 0;

        TerminalGuard termGuard;
        if (!termGuard.ok())
            return;

        sigset_t wait_mask = sigGuard.getOldMask();
        sigdelset(&wait_mask, SIGWINCH);

        auto syncReadlineSize = [] {
            const TermSize sz = terminalSize();
            if (sz.cols != 0 && sz.rows != 0) {
                rl_set_screen_size(
                    static_cast<int>(sz.rows),
                    static_cast<int>(sz.cols)
                );
            }
        };

        auto drawLayout = [&]() {
            if (repaint && *repaint)
                (*repaint)();

            std::cout << "\n"
                      << color
                      << prompt
                      << UI::Palette::BoldReset;
        };

        // Initial draw frame.
        syncReadlineSize();
        {
            SyncOutput sync;
            drawLayout();
        }
        std::cout.flush();

        bool done = false;
        while (!done) {
            recoverCoutIfNeeded();

            if (g_winch) {
                g_winch = 0;
                syncReadlineSize();
                {
                    SyncOutput sync;
                    std::cout << "\033[H\033[2J\033[3J";
                    drawLayout();
                }
                std::cout.flush();
            }

            struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
            const int r = ppoll(&pfd, 1, nullptr, &wait_mask);

            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;

            if (!(pfd.revents & POLLIN))
                continue;

            char ch;
            const ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (n == 0 || ch == '\n' || ch == '\r' || ch == 4 || ch == 27) {
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
