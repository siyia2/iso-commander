// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.interface.pausePrompts;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ui/interface/pausePrompts ----

/**
 * @file
 * @brief User interaction: blocking prompts.
 *
 * Each function here halts the calling thread until the user presses Enter,
 * Esc or Ctrl+D. Input is read directly from the terminal (no `std::cin`
 * buffering); the exact set of accepted keys is documented per function.
 *
 * @par Resize handling
 * The plain variants that install their own SIGWINCH handler clear the screen
 * and redraw only the prompt on resize. The caller's content is lost on
 * resize because no repaint callback is supplied.
 *
 * The `*WithRepaint` variants invoke the supplied callback on resize to redraw
 * the caller's content, then reprint the prompt. The callback must be
 * non-blocking and safe to call repeatedly; typically it is the same lambda
 * the caller passes to @c ListResizeWatcher.
 *
 * One deliberate exception: @ref pressEnterToContinueNoResize installs no
 * SIGWINCH handler and performs no repaint of its own. It is used where
 * another component already owns the resize repaint (currently the WriteScreen
 * keeper thread in @c writeToUsb, which polls the terminal size and repaints
 * the summary plus the prompt text handed to it via @c setPrompt). In that
 * setup two repainters would race and the summary would be wiped on resize, so
 * the raw prompt must stay silent and let the keeper draw. Everywhere else,
 * prefer the repaint-aware variants.
 */

/**
 * @brief Repaint-aware variant of @c pressEnterToTry.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToTryWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Repaint-aware variant of @c pressEnterToReturn.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Repaint-aware variant of @c pressEnterToContinue.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Like @c pressEnterToContinue, but installs no SIGWINCH handler and
 *        performs no repaint of its own.
 *
 * For use where another component already owns the resize repaint — currently
 * @c writeToUsb, where the WriteScreen keeper thread polls the terminal size
 * and redraws the summary plus the prompt text it was given via
 * @c WriteScreen::setPrompt. Installing the raw prompt's own SIGWINCH handler
 * there would produce two repainters racing on the same screen (stripes, a
 * wiped summary, or a duplicated prompt).
 *
 * @warning The caller is responsible for making the owning component repaint
 *          the prompt on resize. If nothing does, the prompt is simply not
 *          redrawn and may be left stale or overwritten. This function only
 *          blocks until Enter, Esc or Ctrl+D.
 *
 * Do not use this as a general-purpose replacement for
 * @c pressEnterToContinue or @ref pressEnterToContinueWithRepaint.
 */
void pressEnterToContinueNoResize();

} // extern "C++"
