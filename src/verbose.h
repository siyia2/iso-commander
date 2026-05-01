// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VERBOSE_H
#define VERBOSE_H

#include <termios.h>
#include <csignal> 
#include <unistd.h>
#include <readline/history.h>
#include <functional>

void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, 
std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int verboseLevel);
void resetVerboseSets(std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, 
std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts);
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile, 
const std::string& destDir, const std::string& errorDetail, const std::string& operation, 
std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, 
std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc);
void saveAndReportResultsForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, 
std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, 
bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound);
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, 
std::unordered_set<std::string>& processedErrorsFind);
void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, 
bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, 
std::unordered_set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths);

#endif // VERBOSE_H
