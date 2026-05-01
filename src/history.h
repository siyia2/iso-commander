// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HISTORY_H
#define HISTORY_H

#include <string>

void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch);

#endif // HISTORY_H
