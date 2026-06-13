// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DATABASEOPS_H
#define DATABASEOPS_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Forward declaration of shared state
struct RefreshState;

// --- Filesystem Traversal ---

/**
 * Recursively scans the filesystem for ISO files.
 * Uses mutexes for thread-safe updates to shared lists and error sets.
 */
void traverse(
    const std::filesystem::path& path,
    std::vector<std::string>& isoFiles,
    std::unordered_set<std::string>& uniqueErrorMessages,
    std::atomic<size_t>& totalFiles,
    std::mutex& traverseFilesMutex,
    std::mutex& traverseErrorsMutex,
    int maxDepth,
    bool promptFlag
);


// --- Database Synchronization ---

/**
 * Populates the global list by reading existing entries from the database.
 */
void loadFromDatabase(std::vector<std::string>& globalIsoFileList);

/**
 * Populates the ISO database with ISOs from scanned folders.
 */
bool saveToDatabase(const std::vector<std::string>& globalIsoFileList, bool* newISOFound);

/**
 * Triggers a refresh logic to sync disk state with the database.
 */
void refreshForDatabase(
    bool promptFlag,
    int maxDepth,
    bool filterHistory,
    bool& newISOFound
);

/**
 * Identifies and removes database entries that no longer exist on the physical disk.
 */
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList);


// --- Background Operations ---

/**
 * Manages the asynchronous import process to ensure the UI/Main thread remains responsive.
 */
void backgroundDatabaseImport(
    std::shared_ptr<RefreshState> state
);

#endif // DATABASEOPS_H
