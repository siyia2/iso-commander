// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GLOBALS_H
#define GLOBALS_H

#include <atomic>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstddef>
#include <thread>
#include <unordered_set>
#include <filesystem>

//==============================
// GLOBAL NAMESPACE ALIASES
//==============================
namespace fs = std::filesystem;

//==============================
// GLOBAL VARIABLES & CONSTANTS
//==============================
// Thread Management
inline unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());

// Operation thread caps for static pool

// Global cap for static threads
inline size_t MAX_USEFUL_THREADS = 32;

// High I/O
inline size_t CPMV_THREAD_CAP   = 8;
inline size_t CONV_THREAD_CAP   = 8;

// Moderate I/O
inline size_t MOUNT_THREAD_CAP  = 16;
inline size_t CLEAN_THREAD_CAP  = 16;

// Low I/O
inline size_t UMOUNT_THREAD_CAP = 32;
inline size_t RM_THREAD_CAP     = 32;

// Low I/O but fast
inline size_t SORT_THREAD_CAP   = 4;
inline size_t FILTER_THREAD_CAP = 4;

// Mutex Protection
inline std::mutex globalSetsMutex;
inline std::mutex updateListMutex;
inline std::mutex binImgCacheMutex;
inline std::mutex mdfMdsCacheMutex;
inline std::mutex nrgCacheMutex;
inline std::mutex chdCacheMutex;
inline std::mutex daaGbiCacheMutex;
inline std::mutex couNtMutex;

// Data Caches
inline std::vector<std::string> globalIsoFileList;
inline thread_local std::unordered_map<std::string, std::string> transformationCache;
inline thread_local std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>> cachedParsesForUmount;
inline std::vector<std::string> binImgFilesCache;
inline std::vector<std::string> mdfMdsFilesCache;
inline std::vector<std::string> nrgFilesCache;
inline std::vector<std::string> chdFilesCache;
inline std::vector<std::string> daaGbiFilesCache;
inline std::map<std::string, std::string> g_configCache;
inline std::string g_cachedPath;

// File Paths
inline const std::string databaseDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/share/isocmd/database/";
inline const std::string databaseFilename  = "iso_commander_database.txt";
inline const std::string databaseFilePath  = databaseDirectory + databaseFilename;
inline const std::string historyFilePath = databaseDirectory + "iso_commander_path_database.txt";
inline const std::string filterHistoryFilePath = databaseDirectory + "iso_commander_filter_database.txt";

inline const std::string configDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/isocmd/";
inline const std::string configPath = configDirectory + "config";

// Configuration Limits
inline int MAX_HISTORY_LINES         = 100;
inline int MAX_HISTORY_PATTERN_LINES = 50;
// Using 'constexpr' is often preferred for POD types if the values are known at compile time
inline constexpr uintmax_t maxDatabaseSize     = 1024 * 1024 * 1; // e.g., 1MB

// State Management
inline std::atomic<bool> isoListDirty{true}; // true = force load iso list on first call
inline std::atomic<bool> g_operationCancelled{false};
inline bool needSortingAfterflno			   = false;
inline size_t ITEMS_PER_PAGE                   = 25;
inline int lockFileDescriptor                  = -1;

#endif // GLOBALS_H
