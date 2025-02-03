// SPDX-License-Identifier: GNU General Public License v3.0 or later

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
#include <libudev.h>
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
#include <set>
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


// Get max available CPU cores for global use
extern unsigned int maxThreads;

// For storing isoFiles in RAM cache
extern std::vector<std::string> globalIsoFileList; 

// Global mutex to protect the verbose sets
extern std::mutex globalSetsMutex;

// Holds IsoCache directory path
extern const std::string cacheFileName;

// Cache for directory and filename transformations
extern std::unordered_map<std::string, std::string> transformationCache;

// For automatic ISO cache refresh from history paths
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

// voids

void printVersionNumber(const std::string& version);
void printMenu();
void submenu1(int& maxDepth, bool& historyPattern, bool& verbose);
void submenu2(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void print_ascii();
void flushStdin();
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
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname,std::set<std::string>& uniqueErrors);

// stds
std::map<std::string, std::string> readConfig(const std::string& configPath);
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);


// GENERAL

// bools
bool isValidInput(const std::string& input);

// voids
void helpSelections();
void helpSearches(bool isCpMv);
void helpMappings();
void clearHistory(const std::string& inputSearch);
void setDisplayMode(const std::string& inputSearch);
void selectForIsoFiles(const std::string& operation, bool& historyPattern, int& maxDepth, bool& verbose);
void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType);
void verbosePrint(const std::set<std::string>& primarySet, const std::set<std::string>& secondarySet , const std::set<std::string>& tertiarySet, const std::set<std::string>& quaternarySet,const std::set<std::string>& errorSet, int printType);
void tokenizeInput(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::set<int>& processedIndices);
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose);

// size_ts
size_t getTotalFileSize(const std::vector<std::string>& files);

// stds
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location);


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
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails);
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles,std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages, bool& verbose);


// UMOUNT

// bools
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered);

// voids
void prepareUnmount(const std::string& input, std::vector<std::string>& selectedIsoDirs, std::vector<std::string>& currentFiles, std::set<std::string>& operationFiles, std::set<std::string>& operationFails, std::set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& verbose);
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);

// stds
std::string modifyDirectoryPath(const std::string& dir);


// CACHE

// bools
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);
bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType);

// stds
std::string getHomeDirectory();
std::vector<std::string> loadCache();

// voids
void verboseIsoCacheRefresh(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::set<std::string>& invalidPaths, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& historyPattern, const std::chrono::high_resolution_clock::time_point& start_time);
void cacheAndMiscSwitches (std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& historyPattern);
void loadCache(std::vector<std::string>& isoFiles);
void manualRefreshCache(const std::string& initialDir = "", bool promptFlag = true, int maxDepth = -1, bool historyPattern = false);
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag);
void backgroundCacheImport(int maxDepthParam, std::atomic<bool>& isImportRunning);
void removeNonExistentPathsFromCache();


//	CP&MV&RM

// stds
std::string userDestDirRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting);

//	voids
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& umountMvRmBreak, bool& historyPattern, bool& verbose);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, bool overwriteExisting);

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
void writeToUsb(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages);

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
std::set<std::string> processBatchPaths(const std::vector<std::string>& batchPaths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback,std::set<std::string>& processedErrorsFind);
std::vector<std::string> findFiles(const std::vector<std::string>& inputPaths, std::set<std::string>& fileNames, int& currentCacheOld, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, const std::vector<std::string>& directoryPaths, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrorsFind);

// voids
void convertToISO(const std::vector<std::string>& imageFiles, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, const bool& modeMdf, const bool& modeNrg, int& maxDepth, bool& promptFlag, bool& historyPattern, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks);
void verboseFind(std::set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths,std::set<std::string>& processedErrorsFind);
void verboseSearchResults(const std::string& fileExtension, std::set<std::string>& fileNames, std::set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::set<std::string>& processedErrorsFind,std::vector<std::string>& directoryPaths);
void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, bool& verbose, bool& promptFlag, int& maxDepth, bool& historyPattern);
void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose, bool& needsScrnClr);
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
