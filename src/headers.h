// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HEADERS_H
#define HEADERS_H

//==============================
// STANDARD LIBRARY INCLUDES
//==============================
// C++ Standard Library
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// System Libraries
#include <grp.h>
#include <poll.h>
#include <libmount/libmount.h>
#include <pwd.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>

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
inline size_t SORT_THREAD_CAP  = 4;
inline size_t FILTER_THREAD_CAP  = 4;

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
inline std::unordered_map<std::string, std::string> transformationCache;
inline std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>> cachedParsesForUmount;
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


//==============================
// ISO COMMANDER FUNCTIONS
//==============================

//------------------
// Boolean Functions
//------------------
bool readUserConfigUpdates(const std::string& filePath);
bool paginationSet(const std::string& filePath);
bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& isFiltered, bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, bool& need2Sort, std::atomic<bool>& isAtISOList);
bool isValidInput(const std::string& input);
bool isHistoryFileEmpty(const std::string& filePath);
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning);
bool isValidDirectory(const std::string& path);
bool saveToDatabase(const std::vector<std::string>& globalIsoFileList, std::atomic<bool>& newISOFound);
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning);
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, const std::string& operation, const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage);
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg);
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex);
bool writeConfig(const std::string& configPath, const std::map<std::string, std::string>& config);

//------------------
// Integer Functions
//------------------
int handleMountUmountCommands(int argc, char* argv[]);
int prevent_readline_keybindings(int, int);
int clear_screen_and_buffer(int, int);

//------------------
// Void Functions (UI)
//------------------
void printMenu();
void submenu1(std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void submenu2(std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning);
void print_ascii();
void helpSelections();
void helpSearches(bool isCpMv, bool import2ISO);
void helpMappings();
void pressEnterToTry();
void pressEnterToReturn();
void pressEnterToContinue();
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, size_t& currentPage, std::atomic<bool>& isImportRunning);
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose, const std::string& operation);
void displayConfigurationOptions(const std::string& configPath);
void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize, const std::unordered_map<std::string, std::string>& transformationCache, const std::vector<std::string>& globalIsoFileList);
void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages);
void setDisplayMode(const std::string& inputSearch);
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch);
void syncCache(const std::string& filePath);
void updateUIAppearance(const std::string& configPath, const std::string& inputSearch);

//------------------
// Void Functions (Input Handling)
//------------------
void flushStdin();
void disable_ctrl_d();
void enable_ctrl_d();
void disableInput();
void customListingsFunction(char **matches, int num_matches, int max_length);
void resetReadlinePagination();
void restoreInput();
void restoreReadline();
void disableReadlineForConfirmation();
void setupReadlineToIgnoreCtrlC();
char** completion_cb(const char* text, int start, int end);

//------------------
// Void Functions (Signal Handling)
//------------------
void signalHandler(int signum);
void setupSignalHandlerCancellations();
void signalHandlerCancellations(int signal);
void clearScrollBuffer();

//------------------
// Void Functions (File Operations)
//------------------
void updateDatabaseAfterOperations(const std::string& filePathsStr, std::atomic<bool>& newISOFound);
void sortFilesCaseInsensitive(std::vector<std::string>& files);
void updatePagination(const std::string& inputSearch, const std::string& configPath);
void clearMessageAfterTimeout(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive);
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& operationBreak, bool& verbose, bool isUnmount);
void processInputForCpMvRm(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound);
void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<int>& processedIndices);
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int verboseLevel);
void resetVerboseSets(std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts);
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile, const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc);
void verboseForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound);
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind);
void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::unordered_set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths);

//------------------
// Void Functions (History Management)
//------------------
void loadHistory(bool& filterHistory);
void saveHistory(bool& filterHistory);
void clearHistory(const std::string& inputSearch);

//------------------
// Void Functions (ISO Operations)
//------------------
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);
void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool silentMode);
void databaseSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& filterHistory, std::atomic<bool>& newISOFound);
void loadFromDatabase(std::vector<std::string>& globalIsoFileList);
void refreshForDatabase(bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound);
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int maxDepth, bool promptFlag);
void backgroundDatabaseImport(std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void removeNonExistentPathsFromDatabase(std::vector<std::string>& globalIsoFileList);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos,
std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks,
std::atomic<size_t>* failedTasks, bool overwriteExisting, std::vector<std::string>* successfulDestPaths, std::mutex* destPathsMutex);
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& operation, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort, size_t& currentPage);
void toLowerInPlace(std::string& str);
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages);

//------------------
// Conversion Functions
//------------------
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts,
const bool& modeMdf, const bool& modeNrg, const bool& modeChd, const bool& modeDaa, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks,
std::vector<std::string>* successfulOutputPaths, std::mutex* outPathsMutex);
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list, std::vector<std::string>& pendingIndices, 
bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning);
void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning);
void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list, std::atomic<bool>& isImportRunning);
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, const bool& modeChd, const bool& modeDaa, 
std::unordered_set<std::string>& processedErrors,
std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound);

//------------------
// Return Type Functions
//------------------
std::map<std::string, std::string> readConfig(const std::string& configPath);
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);
std::string trimWhitespace(const std::string& str);
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location);
std::string userDestDirCpMv(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting);
std::string handlePaginatedDisplay(const std::vector<std::string>& entries, std::unordered_set<std::string>& uniqueErrorMessages, const std::string& promptPrefix, const std::string& promptSuffix, const std::function<void()>& setupEnvironmentFn, bool& isPageTurn);
std::string formatFileSize(uint64_t size);
std::string formatSpeed(double mbPerSec);
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query);
std::tuple<std::string, std::string, std::string> parseMountPointComponents(std::string_view dir);
uint64_t getBlockDeviceSize(const std::string& device);
size_t calculateTotalBytesForConversions(const std::vector<std::string>& filesToProcess, bool modeMdf, bool modeNrg, bool modeChd, bool modeDaa);
size_t getTotalFileSize(const std::vector<std::string>& files);

//==============================
// CONVERSION UTILITIES
//==============================

// CCD2ISO
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

// CHD2ISO
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

//DAA2ISO
bool convertDaaToIso(const std::string &inputFile, const std::string &outputFile, std::atomic<size_t> *completedBytes);

// MDF2ISO
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

// NRG2ISO
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes);

#endif // HEADERS_H
