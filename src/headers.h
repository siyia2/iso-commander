// SPDX-License-Identifier: GNU General Public License v2.0

#ifndef HEADERS_H
#define HEADERS_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <grp.h>
#include <iostream>
#include <libmount/libmount.h>
#include <map>
#include <memory>
#include <mntent.h>
#include <mutex>
#include <pwd.h>
#include <queue>
#include <random>
#include <readline/readline.h>
#include <readline/history.h>
#include <shared_mutex>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include <unordered_set>


// Create alias (fs) for the std::filesystem
namespace fs = std::filesystem;

// Get max available CPU cores for global use
extern unsigned int maxThreads;

// Global mutex to protect the verbose sets
extern std::mutex globalSetsMutex;

// Global mutex to prevent race conditions when live updating ISO list
extern std::mutex updateListMutex;

// Global mutex to protect counter cout
extern std::mutex couNtMutex;

// For storing isoFiles in RAM cache
extern std::vector<std::string> globalIsoFileList; 

// Cache for directory and filename transformations
extern std::unordered_map<std::string, std::string> transformationCache;

// Holds IsoCache directory name
extern const std::string cacheFileName;

// Holds IsoCache directory path
extern const std::string cacheFilePath;

// Holds folder history directory path
extern const std::string historyFilePath;

// Holds configuration path
extern const std::string configPath;

// For signaling cancellations
extern std::atomic<bool> g_operationCancelled;

// Max cache size limit for IsoCache
extern const uintmax_t maxCacheSize;


//	ISO COMMANDER


// MAIN

// bools
bool isValidDirectory(const std::string& path);
bool directoryExists(const std::string& path);
bool fileExists(const std::string& fullPath);
bool startsWithZero(const std::string& str);
bool isNumeric(const std::string& str);
bool isDirectoryEmpty(const std::string& path);
bool readUserConfigUpdates(const std::string& filePath);

// ints
int prevent_readline_keybindings(int, int);
int clear_screen_and_buffer(int, int);
int naturalCompare(const std::string &a, const std::string &b);

// voids
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1(int& maxDepth, bool& historyPattern, bool& verbose, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void submenu2(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound);
void print_ascii();
void flushStdin();
void disable_ctrl_d();
void enable_ctrl_d();
void disableInput();
void restoreInput();
void configMap();
void signalHandler(int signum);
void setupSignalHandlerCancellations();
void signalHandlerCancellations(int signal);
void clearScrollBuffer();
void setupReadlineToIgnoreCtrlC();
void sortFilesCaseInsensitive(std::vector<std::string>& files);
void clearMessageAfterTimeout(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive);
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

// stds
std::map<std::string, std::string> readConfig(const std::string& configPath);
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);


// GENERAL

// bools
bool isValidInput(const std::string& input);

// voids
void helpSelections();
void helpSearches(bool isCpMv, bool import2ISO);
void helpMappings();
void clearHistory(const std::string& inputSearch);
void setDisplayMode(const std::string& inputSearch);
void refreshListAfterAutoUpdate(int timeoutSeconds, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateHasRun, bool& umountMvRmBreak, std::vector<std::string>& filteredFiles, bool& isFiltered, std::string& listSubtype, std::atomic<bool>& newISOFound);
void selectForIsoFiles(const std::string& operation, bool& historyPattern, int& maxDepth, bool& verbose, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void handleSelectIsoFilesResults(std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& skippedMessages, const std::string& operation, bool verbose, bool isMount, bool isFiltered, bool umountMvRmBreak, bool isUnmount, bool& needsClrScrn, bool& historyPattern);
void processOperationForSelectedIsoFiles(const std::string& inputString,bool isMount, bool isUnmount, bool write, bool isFiltered, const std::vector<std::string>& filteredFiles, std::vector<std::string>& isoDirs, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& skippedMessages, bool verbose, bool& needsClrScrn, const std::string& operation, std::atomic<bool>& isAtISOList, bool& umountMvRmBreak, bool promptFlag, int& maxDepth, bool& historyPattern, std::atomic<bool>& newISOFound);
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType);
void resetVerboseSets(std::unordered_set<std::string>& processedErrors,std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts);
void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<int>& processedIndices);
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose);

// size_ts
size_t getTotalFileSize(const std::vector<std::string>& files);

// stds
std::string trimWhitespace(const std::string& str);
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location);


// VERBOSE

//voids
void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int verboseLevel);
void resetVerboseSets(std::unordered_set<std::string>& processedErrors,std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts);
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile,const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc);
void verboseIsoCacheRefresh(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& historyPattern, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound);
void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths,std::unordered_set<std::string>& processedErrorsFind);
void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::unordered_set<std::string>& processedErrorsFind,std::vector<std::string>& directoryPaths);


// HISTORY

// bools
bool isHistoryFileEmpty(const std::string& filePath);

// voids
void loadHistory(bool& historyPattern);
void saveHistory(bool& historyPattern);


// MOUNT

// bools
bool isAlreadyMounted(const std::string& mountPoint);

// voids
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks);
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles,std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& verbose);


// UMOUNT

// bools
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak);

// voids
void prepareUnmount(const std::string& input, const std::vector<std::string>& currentFiles, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose);
void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks);

// stds
std::string modifyDirectoryPath(const std::string& dir);
std::string formatVerboseMessage(const std::string& messageType, const std::string& path);


// CACHE

// bools
bool saveCache(const std::vector<std::string>& isoFiles, std::atomic<bool>& newISOFound);
bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak);

// stds
std::string getHomeDirectory();
std::vector<std::string> loadCache();

// voids
void cacheAndMiscSwitches (std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& historyPattern, std::atomic<bool>& newISOFound);
void loadCache(std::vector<std::string>& isoFiles);
void manualRefreshCache(std::string& initialDir, bool promptFlag, int maxDepth, bool historyPattern, std::atomic<bool>& newISOFound);
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag);
void backgroundCacheImport(int maxDepthParam, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);
void removeNonExistentPathsFromCache();


//	CP&MV&RM

// bools
bool bufferedCopyWithProgress(const fs::path& src, const fs::path& dst, std::atomic<size_t>* completedBytes, std::error_code& ec);
bool performMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile,const std::string& destDirProcessed, const std::string& destFile,size_t fileSize, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership);
bool performMultiDestMoveOperation(const fs::path& srcPath, const fs::path& destPath, const std::string& srcDir, const std::string& srcFile, const std::string& destDirProcessed, const std::string& destFile, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages, const std::function<void(const fs::path&)>& changeOwnership);

// stds
std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting);
std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting);
std::vector<std::string> generateIsoEntries(const std::vector<std::vector<int>>& indexChunks, const std::vector<std::string>& isoFiles);

//	voids
void processOperationInput(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& umountMvRmBreak, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound);
void performDeleteOperation(const fs::path& srcPath,const std::string& srcDir, const std::string& srcFile, size_t fileSize,std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::vector<std::string>& verboseIsos, std::vector<std::string>& verboseErrors ,std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertMessages);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting);
void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile,const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc);

// FILTER

// stds
std::string removeAnsiCodes(const std::string& input);
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text);
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query);

// voids
void toLowerInPlace(std::string& str);

// WRITE2USB

// bools
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex);
bool isUsbDevice(const std::string& devicePath);
bool isDeviceMounted(const std::string& device);

// voids
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages);

// stds
std::string formatFileSize(uint64_t size);
std::string formatSpeed(double bytesPerSec);
std::string getDriveName(const std::string& device);
std::vector<std::string> getRemovableDevices();
std::string formatSpeed(double mbPerSec);
std::vector<std::string> getRemovableDevices();

// CONVERSION TOOLS

// bools
bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg);

// stds
std::unordered_set<std::string> processBatchPaths(const std::vector<std::string>& batchPaths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback,std::unordered_set<std::string>& processedErrorsFind);
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths, std::unordered_set<std::string>& fileNames, int& currentCacheOld, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& invalidDirectoryPaths, std::unordered_set<std::string>& processedErrorsFind);

// voids
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, const bool& modeMdf, const bool& modeNrg, int& maxDepth, bool& promptFlag, bool& historyPattern, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::atomic<bool>& newISOFound);
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound);
void select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, bool& verbose, bool& promptFlag, int& maxDepth, bool& historyPattern, std::atomic<bool>& newISOFound, bool& list);
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose, bool& needsScrnClr, std::atomic<bool>& newISOFound);
void filterQuery(std::vector<std::string>& files, bool& historyPattern, const std::string& filterPrompt, bool& needsScrnClr);
void clearRamCache (bool& modeMdf, bool& modeNrg);


// CCD2ISO

// bools
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

//MDF2ISO

//bools
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

//NRG2ISO

//bools
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes);



#endif // HEADERS_H
