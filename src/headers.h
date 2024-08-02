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
#include <thread>
#include <vector>
#include <unistd.h>


// Get max available CPU cores for global use
extern unsigned int maxThreads;

// Mutex for HighLevel functions
extern std::mutex Mutex4High;

// Mutex for LowLevel functions
extern std::mutex Mutex4Low;

// For storing isoFiles in RAM
extern std::vector<std::string> globalIsoFileList;


// For cache directory creation
extern bool gapPrinted; // for cache refresh for directory function
extern bool promptFlag; // for cache refresh for directory function

// Global variable to control recursion depth
extern int maxDepth;

// For saving history to a differrent cache for FilterPatterns
extern bool historyPattern;

extern bool verbose;

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
void select_and_operate_files_by_number(const std::string& operation);
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, std::set<std::string>& uniqueErrorMessages);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete);

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

int custom_complete(int count, int key);



//	voids

// Art
void printVersionNumber(const std::string& version);
void printMenu();
void submenu1();
void submenu2();
void print_ascii();

// General functions
void loadHistory();
void saveHistory();
void signalHandler(int signum);
void displayProgressBar(const std::atomic<size_t>& completedIsos, const size_t& totalIsos, std::atomic<bool>& isComplete);
void clearScrollBuffer();

// Mount functions
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails);
void printMountedAndErrors(std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages);
void mountIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails);
void select_and_mount_files_by_number();
void printIsoFileList(const std::vector<std::string>& isoFiles);
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFilesIn, std::set<std::string>& mountedFiles,std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails, std::set<std::string>& uniqueErrorMessages);

// Cache functions
void loadCache(std::vector<std::string>& isoFiles);
void manualRefreshCache(const std::string& initialDir = "");
void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles, std::set<std::string>& uniqueErrorMessages);
void removeNonExistentPathsFromCache();

// Filter functions
void toLowerInPlace(std::string& str);
void sortFilesCaseInsensitive(std::vector<std::string>& files);
void filterMountPoints(const std::vector<std::string>& isoDirs, std::vector<std::string>& filterPatterns, std::vector<std::string>& filteredIsoDirs, size_t start, size_t end);
size_t boyerMooreSearchMountPoints(const std::string& haystack, const std::string& needle);

// Unmount functions
void printUnmountedAndErrors(std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);

//	stds

// General functions
std::string shell_escape(const std::string& s);
std::pair<std::string, std::string> extractDirectoryAndFilename(const std::string& path);


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
bool blacklist(const std::filesystem::path& entry, bool blacklistMdf);

// stds
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback, std::set<std::string>& invalidDirectoryPaths, std::set<std::string>& processedErrors);

// voids
void applyFilter(std::vector<std::string>& files, const std::vector<std::string>& originalFiles, const std::string& fileTypeName);
void convertToISO(const std::string& inputPath, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts, bool modeMdf);
void verboseFind(std::set<std::string>& invalidDirectoryPaths);
void verboseConversion(std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts);
void select_and_convert_files_to_iso(const std::string& fileTypeChoice);
void processInput(const std::string& input, const std::vector<std::string>& fileList, bool modeMdf, std::set<std::string>& processedErrors, std::set<std::string>& successOuts, std::set<std::string>& skippedOuts, std::set<std::string>& failedOuts, std::set<std::string>& deletedOuts);
void printFileList(const std::vector<std::string>& fileList);

// CCD2ISO

// bools
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath);

//MDF2ISO

//bools
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath);



#endif // HEADERS_H
