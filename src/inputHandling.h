// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INPUTHANDLING_H
#define INPUTHANDLING_H

// C++ Standard Library Headers
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>

// Third-Party Library Headers
#include <readline/readline.h>

// C / System Headers
#include <signal.h>
#include <termios.h>

void flushStdin();
void disable_ctrl_d();
void enable_ctrl_d();
void disableInput();
void customListingsFunction(char **matches, int num_matches, int max_length);
void resetReadlinePagination();
void restoreInput();
void restoreReadline();
void disableReadlineForConfirmation();
void setupReadlineToIgnoreCtrlC();
void signalHandler(int signum);
void setupSignalHandlerCancellations();
void clearScrollBuffer();

#endif // INPUTHANDLING
