// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PAUSEPROMPTS_H
#define PAUSEPROMPTS_H

#include <functional>
#include <string>

/**
 * @brief User Interaction: Blocking Prompts
 *
 * Halt until Esc, Enter or Ctrl+D. Raw terminal input; no std::cin buffering.
 *
 * Resize handling:
 *  - pressEnterTo / pressEnterToContinue / pressEnterToTry / pressEnterToReturn
 *    clear the screen and redraw only the prompt on SIGWINCH. The caller's
 *    content is lost on resize because no repaint callback is supplied.
 *  - The *WithRepaint variants invoke the supplied callback on resize to redraw
 *    the caller's content, then reprint the prompt. The callback must be
 *    non-blocking and safe to call repeatedly; typically it is the same lambda
 *    the caller passes to ListResizeWatcher.
 *
 * One deliberate exception: pressEnterToContinueNoResize() installs no
 * SIGWINCH handler and performs no repaint of its own. It is used where
 * another component already owns the resize repaint (the WriteScreen keeper
 * thread in writeToUsb, which polls the terminal size and repaints the
 * summary plus the prompt text handed to it via setPrompt). In that setup two
 * repainters would race and the summary would be wiped on resize, so the raw
 * prompt must stay silent and let the keeper draw. Everywhere else, prefer
 * the repaint-aware variants.
 */

/** @brief Blocks until Enter, Esc or Ctrl+D, showing "\n<prompt>". */
void pressEnterTo(const std::string& prompt);

/** @brief Shows "\u21B5 to try again..." and blocks until Enter, Esc or Ctrl+D. */
void pressEnterToTry();

/** @brief Shows "\u21B5 to return..." and blocks until Enter, Esc or Ctrl+D. */
void pressEnterToReturn();

/** @brief Shows "\u21B5 to continue..." and blocks until Enter, Esc or Ctrl+D. */
void pressEnterToContinue();

/**
 * @brief Like pressEnterTo(), but @p repaint redraws the caller's content on
 *        resize. The prompt is reprinted after the callback returns.
 */
void pressEnterToWithRepaint(const std::string& prompt,
                             const std::function<void()>& repaint);

/** @brief Repaint-aware variant of pressEnterToTry(). */
void pressEnterToTryWithRepaint(const std::function<void()>& repaint);

/** @brief Repaint-aware variant of pressEnterToReturn(). */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint);

/** @brief Repaint-aware variant of pressEnterToContinue(). */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Like pressEnterToContinue(), but installs no SIGWINCH handler and
 *        performs no repaint of its own.
 *
 * For use where another component already owns the resize repaint — in
 * particular writeToUsb, where the WriteScreen keeper thread polls the
 * terminal size and redraws the summary plus the prompt text it was given via
 * WriteScreen::setPrompt. Installing the raw prompt's own SIGWINCH handler
 * there would produce two repainters racing on the same screen (stripes, a
 * wiped summary, or a duplicated prompt). Callers must arrange for the owning
 * component to repaint the prompt on resize; this function only blocks until
 * Enter, Esc or Ctrl+D.
 *
 * Do not use this as a general-purpose replacement for
 * pressEnterToContinue() or pressEnterToContinueWithRepaint().
 */
void pressEnterToContinueNoResize();

#endif // PAUSEPROMPTS_H
