// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SEARCHES_H
#define SEARCHES_H

void helpSearches(bool isCpMv, bool import2ISO);
void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize);
void databaseSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& filterHistory, std::atomic<bool>& newISOFound);

#endif // SEARCHES_H
