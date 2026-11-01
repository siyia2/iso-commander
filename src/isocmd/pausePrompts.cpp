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
    void handle_winch(int) { g_winch = 1; }

    /**
     * @brief Restores std::cout to a usable state after a transient write
     *        error, and releases the terminal from any state a partially
     *        applied synchronized-update frame may have left it in.
     *
     * If a flush inside a SyncOutput fails (EIO/EPIPE during a VTE reflow,
     * a TCSAFLUSH reset, etc.) C++ sets badbit on std::cout and every later
     * `std::cout << ...` becomes a silent no-op, while Readline — which
     * writes through its own FILE* — keeps working. That is the exact
     * signature of "menus and list disappear but prompts still render".
     *
     * This helper clears failbit/badbit and writes the escapes that undo
     * the sync-mode / SGR / scroll-region / cursor state a dropped
     * "\033[?2026l" may have left behind. It is a no-op when the stream is
     * healthy. Called once per iteration of the wait loop and once on entry,
     * so no single transient terminal error can permanently silence the UI.
     */
    void recoverCoutIfNeeded() {
        if (std::cout.good()) return;
        std::cout.clear();
        std::cout << "\033[?2026l"   // release synchronized-update, if stuck
                  << "\033[0m"       // reset SGR attributes
                  << "\033[r"        // reset scroll region
                  << "\033[?7h"      // re-enable auto-wrap
                  << "\033[?25h"     // re-show cursor
                  << std::flush;
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
     * reflowing terminals: VTE-based ones (xfce4-terminal, gnome-terminal,
     * tilix, …) reflow content but leave both the live cursor and any
     * DECSC-saved cell at their old absolute coordinates, so neither is a
     * usable anchor for the prompt's row. Patching in place therefore
     * produces drift on some terminals and striped partial repaints on VTE.
     * On resize the screen is cleared with "\033[H\033[2J\033[3J": the
     * "\033[2J" erases the visible screen, and the "\033[3J" erases the
     * scrollback as well. Both are needed on VTE-based terminals, which
     * push the cleared frame into scrollback on each resize; without
     * "\033[3J" a user scrolling up sees a growing stack of past frames,
     * looking like duplication even though the visible frame is correct.
     * If @p repaint is non-null, it is invoked after the clear to redraw
     * the caller's content; then the blank separator and the prompt are
     * reprinted. Without a callback the pre-prompt content is lost on
     * resize, but the prompt is still redrawn cleanly — never striped,
     * never duplicated, never drifting.
     *
     * Both the initial draw and every resize redraw are written as a single
     * synchronized-update frame (SyncOutput) so the clear, the caller's
     * repaint and the prompt line reach the terminal atomically. The flush
     * happens OUTSIDE the SyncOutput scope, after "\033[?2026l" has been
     * emitted, so a transient flush failure cannot leave std::cout in a
     * permanent bad state with sync mode still active on the terminal.
     *
     * Back-to-back SIGWINCH notifications are collapsed into a single repaint
     * (non-blocking poll + flag clear) so two repaints cannot interleave.
     *
     * The original termios state and the previous SIGWINCH disposition are
     * restored before returning, a trailing newline is emitted so callers
     * start on a fresh line (matching readline()'s behaviour), and unread
     * input (e.g. the tail of an arrow-key escape sequence) is flushed.
     *
     * @param prompt  Text to show after the leading newline (e.g.
     *                "\u21B5 to continue...").
     * @param repaint Optional pointer to a callback that redraws everything
     *                printed before the prompt. May be null.
     */
    void pressEnterToImpl(const std::string& prompt,
                          const std::function<void()>* repaint) {
        enable_ctrl_d();

        // Recover from any pre-existing bad stream state before we start
        // painting; otherwise the first frame would be silently dropped.
        recoverCoutIfNeeded();

        struct termios raw, saved;
        tcgetattr(STDIN_FILENO, &saved);
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

        struct sigaction sa{}, old_sa{};
        sa.sa_handler = handle_winch;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGWINCH);
        sigprocmask(SIG_BLOCK, &block, &old);
        g_winch = 0;                              // reset while blocked: no race
        sigaction(SIGWINCH, &sa, &old_sa);        // swap handler while blocked
        sigprocmask(SIG_SETMASK, &old, nullptr);  // unblock; pending signal now hits the new handler

        // Keep Readline's wrapping in step with the terminal we're about to
        // paint. Done on the initial draw and on every resize, before the
        // frame that depends on it is composed. No-op in this prompt's
        // raw-read loop, but keeps the invariant that any readline state
        // touched by a repaint reflects the current terminal size.
        auto syncReadlineSize = [] {
            const TermSize sz = terminalSize();
            if (sz.cols != 0 && sz.rows != 0) {
                rl_set_screen_size(static_cast<int>(sz.rows),
                                   static_cast<int>(sz.cols));
            }
        };

        // The prompt body (repaint + prompt line) without any framing. Used
        // both for the initial draw and for resize redraws, each wrapped in
        // its own SyncOutput by the caller. Deliberately does NOT flush:
        // the flush is the caller's job, after the SyncOutput has closed,
        // so a flush failure cannot poison std::cout while sync mode is
        // still active on the terminal.
        auto drawLayout = [&]() {
            if (repaint && *repaint) (*repaint)();
            std::cout << "\n" << color << prompt << UI::Palette::BoldReset;
        };

        // Initial draw: one atomic frame so the caller's repaint and the
        // prompt never appear as two separate screens. Flush after the
        // frame is closed.
        syncReadlineSize();
        {
            SyncOutput sync;
            drawLayout();
        }
        std::cout.flush();

        bool done = false;
        while (!done) {
            // If a prior flush failed, restore std::cout before painting
            // again; otherwise the repaint would be silently dropped.
            recoverCoutIfNeeded();

            if (g_winch) {
                g_winch = 0;

                // Drain any pending terminal noise so a queued keystroke is
                // not lost after the redraw.
                {
                    struct pollfd drain{ STDIN_FILENO, POLLIN, 0 };
                    char junk;
                    while (poll(&drain, 1, 0) > 0) {
                        if (read(STDIN_FILENO, &junk, 1) <= 0) break;
                    }
                }

                syncReadlineSize();

                // One frame: home/clear/clear + caller's repaint + prompt.
                // The caller's repaint may open its own SyncOutput; nested
                // 2026h/2026l is a no-op on compliant terminals and correct
                // with a ref-counted SyncOutput.
                {
                    SyncOutput sync;
                    std::cout << "\033[H\033[2J\033[3J";
                    drawLayout();
                }
                std::cout.flush();
            }

            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int r = poll(&pfd, 1, 100);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) continue;

            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) {
                done = true;
            } else if (ch == '\n' || ch == '\r' || ch == 4) {
                done = true;
            } else if (ch == 27) {
                done = true;
                // swallow the tail of an arrow-key / function-key sequence
                struct pollfd p{ STDIN_FILENO, POLLIN, 0 };
                while (poll(&p, 1, 20) > 0) {
                    char junk;
                    if (read(STDIN_FILENO, &junk, 1) <= 0) break;
                }
            }
        }

        // End the prompt line so callers start on a fresh row, exactly like
        // readline() does. Without this, subsequent relative cursor motion
        // (e.g. "\033[2A\033[K" in selectForImageFiles) lands on the wrong
        // row and erases the freshly drawn list.
        std::cout << '\n';
        std::cout.flush();

        tcflush(STDIN_FILENO, TCIFLUSH);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        sigaction(SIGWINCH, &old_sa, nullptr);
    }
}

/**
 * @brief Waits for Enter, Esc or Ctrl+D after showing "\n<prompt>".
 *
 * On resize the screen is cleared and just "\n<prompt>" is redrawn — the
 * pre-prompt content is lost because no repaint callback was supplied. For
 * callers that can redraw their content, use pressEnterToWithRepaint().
 *
 * @param prompt Text to show after the leading newline.
 */
void pressEnterTo(const std::string& prompt) {
    pressEnterToImpl(prompt, nullptr);
}

/**
 * @brief Like pressEnterTo(), but @p repaint is called to redraw the
 *        caller's content on resize, so the screen is fully restored instead
 *        of just the prompt.
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
 *
 * Shows "\u21B5 to try again..." and redraws the caller's content on resize
 * via @p repaint.
 */
void pressEnterToTryWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to try again...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToReturn().
 *
 * Shows "\u21B5 to return..." and redraws the caller's content on resize via
 * @p repaint.
 */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to return...", repaint);
}

/**
 * @brief Repaint-aware variant of pressEnterToContinue().
 *
 * Shows "\u21B5 to continue..." and redraws the caller's content on resize via
 * @p repaint.
 */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint) {
    pressEnterToWithRepaint("\u21B5 to continue...", repaint);
}
