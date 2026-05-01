// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INPUTHANDLING_H
#define INPUTHANDLING_H

#include <fstream>
#include <iostream>
#include <functional>
#include <signal.h>
#include <termios.h>
#include <readline/readline.h> 
#include <cstdlib>
#include <memory>
#include <memory>
#include <iomanip>

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
