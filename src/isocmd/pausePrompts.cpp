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
     * repaint and the prompt line reach the terminal atomically. Without
     * that, the clear and the repaint were separate frames and the terminal
     * briefly rendered the cleared screen between them (visible as a flash
     * on VTE-based terminals on every resize).
     *
     * The loop is fully event-driven: no periodic wake-ups. The main loop
     * blocks in read(); SIGWINCH is delivered with SA_RESTART cleared so the
     * signal interrupts read() with EINTR, the loop re-checks g_winch at the
     * top, and a resize is redrawn immediately. The classic lost-wakeup race
     * (signal arrives after the flag check but before read() enters) is
     * closed by ppoll() with an empty temporary mask: it unblocks SIGWINCH
     * atomically with respect to entry to the wait, so a signal delivered at
     * that instant either runs the handler before ppoll() blocks (flag set,
     * ppoll() then returns immediately because the signal is pending/
     * delivered) or wakes ppoll(). There is no timeout — ppoll() sleeps
     * indefinitely — so this is not a polling loop; it is a blocking wait
     * that is signal-wakeable without a race window.
     *
     * Back-to-back SIGWINCH notifications are collapsed: the flag is cleared
     * once before the redraw and only one redraw runs per iteration, so two
     * repaints cannot interleave.
     *
     * The original termios state and the previous SIGWINCH disposition are
     * restored before returning, and unread input (e.g. the tail of an
     * arrow-key escape sequence) is flushed.
     *
     * @param prompt  Text to show after the leading newline (e.g.
     *                "\u21B5 to continue...").
     * @param repaint Optional pointer to a callback that redraws everything
     *                printed before the prompt. May be null.
     */
    void pressEnterToImpl(const std::string& prompt,
                          const std::function<void()>* repaint) {
        enable_ctrl_d();

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
        sa.sa_flags = 0;                          // no SA_RESTART: read()/ppoll() must EINTR

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

        // Writes the blank separator and the prompt line. The caller's
        // repaint (if any) is expected to have already drawn everything above
        // this point; it runs first so the prompt lands under it. Flushes so
        // the following '?2026l' (from the enclosing SyncOutput) commits the
        // whole frame.
        auto drawPromptLine = [&]() {
            std::cout << "\n" << color << prompt << UI::Palette::BoldReset;
            std::cout.flush();
        };

        // The prompt body (repaint + prompt line) without any framing. Used
        // both for the initial draw and for resize redraws, each wrapped in
        // its own SyncOutput by the caller.
        auto drawLayout = [&]() {
            if (repaint && *repaint) (*repaint)();
            drawPromptLine();
        };

        // Initial draw: one atomic frame so the caller's repaint and the
        // prompt never appear as two separate screens.
        syncReadlineSize();
        {
            SyncOutput sync;
            drawLayout();
        }

        // Empty mask used by ppoll() below: unblocking SIGWINCH relative to
        // entry into the wait is what closes the lost-wakeup race. SIGWINCH
        // is currently unblocked in the ambient mask; passing an empty set
        // here is explicit and matches the ambient state, so ppoll() atomically
        // unblocks nothing new — it simply enters the wait with SIGWINCH
        // deliverable and cannot miss a signal that fires between the flag
        // check above and the wait itself.
        sigset_t wait_mask;
        sigemptyset(&wait_mask);

        bool done = false;
        while (!done) {
            if (g_winch) {
                g_winch = 0;

                // Discard bytes that queued while the terminal was resizing;
                // they were typed against the old layout. tcflush() is the
                // clean way to do this — no extra poll/read syscalls.
                tcflush(STDIN_FILENO, TCIFLUSH);

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
            }

            // Blocking wait with no timeout. ppoll() with a null timeout is
            // not a polling loop: it sleeps until stdin is readable or a
            // signal fires. The empty wait_mask closes the race between the
            // g_winch check above and entry into the wait.
            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int r = ppoll(&pfd, 1, nullptr, &wait_mask);
            if (r < 0) {
                if (errno == EINTR) continue;     // SIGWINCH → re-check flag at top
                break;
            }

            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            if (ch == '\n' || ch == '\r' || ch == 4) done = true;
            else if (ch == 27) {
                done = true;
                // Swallow the tail of an arrow-key / function-key sequence.
                // Bounded wait is unavoidable here — we must give the terminal
                // a moment to deliver the remaining bytes — but this is a
                // one-shot path on Esc, not the main loop.
                struct pollfd p{ STDIN_FILENO, POLLIN, 0 };
                while (poll(&p, 1, 20) > 0) {
                    char junk;
                    if (read(STDIN_FILENO, &junk, 1) <= 0) break;
                }
            }
        }

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
