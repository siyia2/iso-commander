// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PAUSEPROMPT_H
#define PAUSEPROMPT_H

#include <functional>

/**
 * @brief User Interaction: Blocking Prompts
 *
 * These functions halt execution until the user provides an Esc, EOL (Enter)
 * or EOT (Ctrl+D) signal. They are designed to handle raw terminal
 * input to avoid common buffering issues with std::cin.
 *
 * @par Resize repaint
 * Each function takes an optional @c redrawScreen callback. If given, a
 * ConsoleResizeWatcher repaints the screen while the pause waits: when the
 * terminal width changes, @c redrawScreen is called to redraw the whole screen
 * above the message, and the message is then printed again. Only pass it for
 * screens that can be fully redrawn (such as the help screens). After one-off
 * messages ("Updated and saved", "Invalid value", scan results) leave it empty,
 * because a repaint would erase them. The callback runs on the watcher thread,
 * so it must only draw (no input, no side effects) and should not throw.
 * An empty callback means no watcher and no thread.
 */

/**
 * @brief Prompts user to retry a failed operation.
 *
 * @param redrawScreen Optional. Redraws the whole screen above the message on
 *        terminal resize. Empty (default) means no repaint.
 */
void pressEnterToTry(const std::function<void()>& redrawScreen = nullptr);

/**
 * @brief Prompts user before returning to the previous menu.
 *
 * @param redrawScreen Optional. Redraws the whole screen above the message on
 *        terminal resize. The help screens pass their drawing lambda here.
 *        Empty (default) means no repaint.
 */
void pressEnterToReturn(const std::function<void()>& redrawScreen = nullptr);

/**
 * @brief Simple pause to allow the user to read screen output before proceeding.
 *
 * @param redrawScreen Optional. Redraws the whole screen above the message on
 *        terminal resize. Empty (default) means no repaint.
 */
void pressEnterToContinue(const std::function<void()>& redrawScreen = nullptr);

#endif // PAUSEPROMPT_H
