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
#include <grp.h>
#include <iostream>
#include <libmount/libmount.h>
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

void tokenizeInput(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::vector<int>& processedIndices);

// Get max available CPU cores for global use
extern unsigned int maxThreads;

// For storing isoFiles in RAM cache
extern std::vector<std::string> globalIsoFileList; 

// For toggling between full and shortened paths in lists
extern bool toggleFullList;

// Caching the results of directory and filename transformations
extern std::unordered_map<std::string, std::string> transformationCache;


//	CP&MV&RM

//	bools
bool isValidLinuxPathFormat(const std::string& path);

// General
bool isValidDirectory(const std::string& path);
bool directoryExists(const std::string& path);
bool fileExists(const std::string& fullPath);

//	voids

// General
void verbose_cp_mv_rm(std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages);
void select_and_operate_files_by_number(const std::string& operation, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& mvDelBreak, bool& historyPattern, bool& verbose);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::mutex& Mutex4Low);

// stds

// General
std::string promptCpMvRm(std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& mvDelBreak, bool& historyPattern, bool& isDelete, bool& isCopy, bool& abortDel);

//	ISO COMMANDER

//	bools

// Iso cache functions
bool iequals(const std::string_view& a, const std::string_view& b);
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);

// Mount functions
bool isAlreadyMounted(const std::string& mountPoint);

// Unmount functions
bool isDirectoryEmpty(const std::string& path);

// General functions

bool startsWithZero(const std::string& str);
bool isNumeric(const std::string& str);



//	voids

// Art
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void submenu2(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void print_ascii();

// General functions
void flushStdin();
void disableInput();
void restoreInput();
void loadHistory(bool& historyPattern);
void saveHistory(bool& historyPattern);
void signalHandler(int signum);
void displayProgressBar(const std::atomic<size_t>& completedIsos, const size_t& totalIsos, std::atomic<bool>& isComplete, bool& verbose);
void clearScrollBuffer();

// Mount functions
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, bool& verbose, std::mutex& Mutex4Low);
void printMountedAndErrors(std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages);
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::mutex& Mutex4Low);
void select_and_mount_files_by_number(bool& historyPattern, bool& verbose);
void printIsoFileList(const std::vector<std::string>& isoFiles);
void processAndMountIsoFiles(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles,std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages, bool& verbose, std::mutex& Mutex4Low);

// Cache functions
void loadCache(std::vector<std::string>& isoFiles);
void manualRefreshCache(const std::string& initialDir = "", bool promptFlag = true, int maxDepth = -1, bool historyPattern = false);
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, size_t& totalProcessedFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag);
void removeNonExistentPathsFromCache();

// Filter functions
void toLowerInPlace(std::string& str);
void sortFilesCaseInsensitive(std::vector<std::string>& files);

// Unmount functions
void printUnmountedAndErrors(std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);
void listMountedISOs();
void unmountISOs(bool& historyPattern, bool& verbose);
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::mutex& Mutex4Low);

//	stds

// General functions
std::string shell_escape(const std::string& s);
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path);


// Cache functions
std::string getHomeDirectory();
std::vector<std::string> loadCache();

// Filter functions
std::vector<size_t> boyerMooreSearch(const std::string& pattern, const std::string& text);
std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query);

// Unmount functions
std::vector<std::string> parseUserInputUnmountISOs(const std::string& input, const std::vector<std::string>& isoDirs, bool& invalidInput, bool& noValid, bool& isFiltered);


// CONVERSION TOOLS

// General

// bools
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf, bool blacklistNrg);

// stds
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrors, bool gapSet);

// voids
void applyFilter(std::vector<std::string>& files, const std::vector<std::string>& originalFiles, const std::string& fileTypeName, bool& historyPattern);
void convertToISO(const std::vector<std::string>& imageFiles, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool modeMdf, bool modeNrg, int& maxDepth, bool& promptFlag, bool& historyPattern, std::mutex& Mutex4Low);
void verboseFind(std::set<std::string>& invalidDirectoryPaths, bool gapSet);
void verboseConversion(std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts);
void select_and_convert_files_to_iso(const std::string& fileTypeChoice, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void handle_file_conversion_for_select_and_convert_to_iso(const std::string& fileType, std::vector<std::string>& files, std::vector<std::string>& originalFiles, bool& verbose, bool& promptFlag, bool& modeMdf, bool& modeNrg, int& maxDepth, bool& historyPattern);
void processInput(const std::string& input, std::vector<std::string>& fileList, bool modeMdf, bool modeNrg, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose);
void printFileList(const std::vector<std::string>& fileList);


// CCD2ISO

// bools
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath);

//MDF2ISO

//bools
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath);

//NRG2ISO

//bools
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile);



#endif // HEADERS_H
