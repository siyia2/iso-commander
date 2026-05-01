// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PROCESS_H
#define PROCESS_H

#include <csignal>

void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, 
std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose, const std::string& operation);
void updateDatabaseAfterOperations(const std::string& filePathsStr, std::atomic<bool>& newISOFound);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos,
std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, 
std::atomic<size_t>* completedTasks,std::atomic<size_t>* failedTasks, bool overwriteExisting, std::vector<std::string>* successfulDestPaths, std::mutex* destPathsMutex);
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, 
std::unordered_set<std::string>& failedOuts, const bool& modeMdf, const bool& modeNrg, const bool& modeChd, const bool& modeDaa, 
std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>* successfulOutputPaths, std::mutex* outPathsMutex);
size_t calculateTotalBytesForConversions(const std::vector<std::string>& filesToProcess, bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa);
size_t getTotalFileSize(const std::vector<std::string>& files);
std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, 
std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, 
bool& isCopy, bool& abortDel, bool& overwriteExisting);

#endif // PROCESS_H
