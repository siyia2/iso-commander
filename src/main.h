// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MAIN_H
#define MAIN_H

#include <thread>

bool readUserConfigUpdates(const std::string& filePath);
bool paginationSet(const std::string& filePath);
bool isHistoryFileEmpty(const std::string& filePath);
int handleMountUmountCommands(int argc, char* argv[]);
void clearMessageAfterTimeoutInMain(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive, std::atomic<bool>& stopMessage);
void monitorAndClearMessage(std::atomic<bool>& isRunning, std::atomic<bool>& messageActive, std::atomic<bool>& stopSignal, std::atomic<bool>& isAtMain);
void printMenu();
void submenu1(std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound,std::atomic<bool>& stopImport, std::vector<std::thread>& backgroundThreads, bool& search);
void submenu2(std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning);
void print_ascii();
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);

#endif // MAIN_H
