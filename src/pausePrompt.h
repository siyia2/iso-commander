// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PAUSEPROMPT_H
#define PAUSEPROMPT_H

/**
 * @brief User Interaction: Blocking Prompts
 *
 * These functions halt execution until the user provides an Esc, EOL (Enter)
 * or EOT (Ctrl+D) signal. They are designed to handle raw terminal
 * input to avoid common buffering issues with std::cin.
 */

/**
 * @brief Prompts user to retry a failed operation.
 */
void pressEnterToTry();

/**
 * @brief Prompts user before returning to the previous menu.
 */
void pressEnterToReturn();

/**
 * @brief Simple pause to allow the user to read screen output before proceeding.
 */
void pressEnterToContinue();

#endif // PAUSEPROMPT_H
