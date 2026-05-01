// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HISTORY_H
#define HISTORY_H

// C++ Standard Library Headers
#include <filesystem>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// C / System Headers
#include <sys/file.h>
#include <sys/stat.h>

void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch);

#endif // HISTORY_H
