
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
     * @brief Core implementation of the "press Enter to continue" prompt.
     *
     * Never called directly; use pressEnterTo() / pressEnterToWithRepaint()
     * and their wrappers.
     *
     * The terminal is switched to raw mode (no canonical buffering, no echo)
     * so Enter, Esc and Ctrl+D are detected immediately and keystrokes never
     * advance the cursor.
     *
     * Resize handling is a full redraw, not an in-place patch. There is no
     * escape-sequence-only way to locate the prompt after a resize on
     * reflowing terminals: VTE-based ones reflow content but leave both the
     * live cursor and any DECSC-saved cell at their old absolute coordinates.
     *
     * On resize the screen is cleared with "\033[H\033[2J\033[3J".
     *
     * Both the initial draw and every resize redraw are written as a single
     * synchronized-update frame (SyncOutput).
     *
     * SIGWINCH remains blocked throughout the event loop. ppoll() atomically
     * restores the original signal mask while waiting, allowing a pending
     * SIGWINCH to execute its handler and interrupt ppoll(). This closes the
     * lost-wakeup race between checking g_winch and entering ppoll().
     *
     * Back-to-back SIGWINCH notifications are collapsed into one redraw.
     *
     * The original termios state, SIGWINCH disposition and signal mask are
     * restored before returning.
     *
     * @param prompt  Text to show after the leading newline.
     * @param repaint Optional pointer to a callback that redraws everything
     *                printed before the prompt. May be null.
     */
    void pressEnterToImpl(const std::string& prompt,
                          const std::function<void()>* repaint) {
        enable_ctrl_d();

        struct termios saved{};
        if (tcgetattr(STDIN_FILENO, &saved) < 0)
            return;

        struct termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
            return;

        struct sigaction sa{}, old_sa{};
        sa.sa_handler = handle_winch;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // No SA_RESTART: ppoll() must return EINTR.

        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGWINCH);

        // Block SIGWINCH before installing the handler.
        // It remains blocked throughout the event loop.
        if (sigprocmask(SIG_BLOCK, &block, &old) < 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            return;
        }

        g_winch = 0;

        if (sigaction(SIGWINCH, &sa, &old_sa) < 0) {
            sigprocmask(SIG_SETMASK, &old, nullptr);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            return;
        }

        // Keep Readline's wrapping in step with the terminal size.
        auto syncReadlineSize = [] {
            const TermSize sz = terminalSize();

            if (sz.cols != 0 && sz.rows != 0) {
                rl_set_screen_size(static_cast<int>(sz.rows),
                                   static_cast<int>(sz.cols));
            }
        };

        // Draw the blank separator and prompt.
        auto drawPromptLine = [&]() {
            std::cout << "\n" << color << prompt
                      << UI::Palette::BoldReset;
            std::cout.flush();
        };

        // Draw caller content followed by the prompt.
        auto drawLayout = [&]() {
            if (repaint && *repaint)
                (*repaint)();

            drawPromptLine();
        };

        // Initial draw.
        syncReadlineSize();

        {
            SyncOutput sync;
            drawLayout();
        }

        // ppoll() temporarily restores this mask while waiting.
        // SIGWINCH is therefore unblocked during ppoll() if it was
        // originally unblocked, and blocked again before it returns.
        const sigset_t wait_mask = old;

        bool done = false;

        while (!done) {
            if (g_winch) {
                g_winch = 0;

                // Discard input queued during the resize.
                tcflush(STDIN_FILENO, TCIFLUSH);

                syncReadlineSize();

                // One atomic redraw frame.
                {
                    SyncOutput sync;
                    std::cout << "\033[H\033[2J\033[3J";
                    drawLayout();
                }
            }

            // SIGWINCH is blocked here.
            // ppoll() atomically restores the original mask while waiting,
            // closing the lost-wakeup race.
            struct pollfd pfd{STDIN_FILENO, POLLIN, 0};

            const int r = ppoll(&pfd, 1, nullptr, &wait_mask);

            if (r < 0) {
                if (errno == EINTR)
                    continue; // SIGWINCH: check flag at the top.

                break;
            }

            if (r == 0)
                continue;

            // Handle terminal hangup/error conditions.
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

            if (n == 0)
                break;

            if (ch == '\n' || ch == '\r' || ch == 4) {
                done = true;
            }
            else if (ch == 27) {
                done = true;

                // Swallow the tail of an arrow-key / function-key sequence.
                // Bounded wait is only used on this one-shot Esc path.
                struct pollfd p{STDIN_FILENO, POLLIN, 0};

                while (poll(&p, 1, 20) > 0) {
                    char junk;

                    if (read(STDIN_FILENO, &junk, 1) <= 0)
                        break;
                }
            }
        }

        // Restore terminal and signal state.
        tcflush(STDIN_FILENO, TCIFLUSH);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);

        // Restore the previous signal disposition while SIGWINCH is blocked.
        sigaction(SIGWINCH, &old_sa, nullptr);

        // Restore the original signal mask.
        sigprocmask(SIG_SETMASK, &old, nullptr);
    }
}

/**
 * @brief Waits for Enter, Esc or Ctrl+D after showing "\n<prompt>".
 *
 * On resize the screen is cleared and only the prompt is redrawn.
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
    pressEnterToWithRepaint("↵ to try again...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToReturn().
 */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("↵ to return...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToContinue().
 */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("↵ to continue...", repaint);
}
