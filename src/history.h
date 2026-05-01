// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HISTORY_H
#define HISTORY_H

#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <filesystem>
#include <readline/readline.h>
#include <readline/history.h>

void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch);

#endif // HISTORY_H
