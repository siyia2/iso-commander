// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.ui.interface.pausePrompts;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @file
 * @brief User interaction: blocking prompts.
 *
 * Each function here halts the calling thread until the user presses Enter,
 * Esc or Ctrl+D. Input is read directly from the terminal (no `std::cin`
 * buffering); the exact set of accepted keys is documented per function.
 *
 * @par Resize handling
 * The `*WithRepaint` variants invoke the supplied callback on resize to redraw
 * the caller's content, then reprint the prompt. The callback must be
 * non-blocking and safe to call repeatedly; typically it is the same lambda
 * the caller passes to @c ListResizeWatcher.
 */

/**
 * @brief Waits for the user to retry, repainting on resize.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToTryWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Waits for the user to return, repainting on resize.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToReturnWithRepaint(const std::function<void()>& repaint);

/**
 * @brief Waits for the user to continue, repainting on resize.
 *
 * @param repaint Callback invoked on resize to redraw the caller's content.
 *                Must be non-blocking and safe to call repeatedly.
 */
void pressEnterToContinueWithRepaint(const std::function<void()>& repaint);

} // extern "C++"
