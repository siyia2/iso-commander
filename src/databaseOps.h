// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DATABASEOPS_H
#define DATABASEOPS_H

#include <sys/stat.h>
#include <filesystem>
#include <unordered_set>
#include <vector>
#include <atomic>

void loadFromDatabase(std::vector<std::string>& globalIsoFileList);
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound);
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int maxDepth, bool promptFlag);
void backgroundDatabaseImport(std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound, std::atomic<bool>& stopImport);
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList);

#endif // DATABASEOPS_H
