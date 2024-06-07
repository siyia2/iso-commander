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
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <libmount/libmount.h>
#include <memory>
#include <mntent.h>
#include <mutex>
#include <queue>
#include <readline/readline.h>
#include <readline/history.h>
#include <set>
#include <shared_mutex>
#include <string>
#include <sys/mount.h>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


// Global and shared classes and variables


// Get max available CPU cores for global use
extern unsigned int maxThreads;

// For storing unique input errors
extern std::set<std::string> uniqueErrorMessages;

// Mutexes for global use
extern std::mutex Mutex4Low;
extern std::mutex Mutex4Med;
extern std::mutex Mutex4High;

// For cache directory creation
extern bool gapPrinted; // for cache refresh for directory function
extern bool promptFlag; // for cache refresh for directory function
extern bool gapPrintedtraverse; // for traverse function

// For making cache refresh headless
extern bool promptFlag;

// For saving history to a differrent cache for FilterPatterns
extern bool historyPattern;


//	CP&MV&RM

//	bools

// General
bool directoryExists(const std::string& path);
bool isValidLinuxPathFormat(const std::string& path);

// RM functions

//	bools
bool fileExists(const std::string& filename);


//	voids

// General
void select_and_operate_files_by_number(const std::string& operation);
void processOperationInput(const std::string& input, std::vector<std::string>& isoFiles, const std::string& process, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors);
void handleIsoFileOperation(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& operationIsos, std::set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete);


// RM functions
void handleDeleteIsoFile(const std::vector<std::string>& isoFiles, std::vector<std::string>& isoFilesCopy, std::set<std::string>& deletedSet);


//	ISO COMMANDER

//	bools

// Mount functions
bool isAlreadyMounted(const std::string& mountPoint);

// Iso cache functions
bool saveCache(const std::vector<std::string>& isoFiles, std::size_t maxCacheSize);
bool ends_with_iso(const std::string& str);

// Unmount functions
bool isDirectoryEmpty(const std::string& path);

// General functions
bool isAllZeros(const std::string& str);
bool isNumeric(const std::string& str);


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
void clearScrollBuffer();

// Mount functions
void mountAllIsoFiles(const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails);
void printMountedAndErrors(std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages, std::set<std::string>& mountedFails);
void mountIsoFile(const std::vector<std::string>& isoFilesToMount, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails);
void select_and_mount_files_by_number();
void printIsoFileList(const std::vector<std::string>& isoFiles);
void processAndMountIsoFiles(const std::string& input, const std::vector<std::string>& isoFiles, std::set<std::string>& mountedFiles, std::set<std::string>& skippedMessages,std::set<std::string>& mountedFails);

// Cache functions
void manualRefreshCache(const std::string& initialDir = "");
void parallelTraverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles);
void refreshCacheForDirectory(const std::string& path, std::vector<std::string>& allIsoFiles);
void removeNonExistentPathsFromCache();

// Filter functions
void sortFilesCaseInsensitive(std::vector<std::string>& files);
void filterMountPoints(const std::vector<std::string>& isoDirs, std::vector<std::string>& filterPatterns, std::vector<std::string>& filteredIsoDirs, std::mutex& resultMutex, size_t start, size_t end);
size_t boyerMooreSearchMountPoints(const std::string& haystack, const std::string& needle);

// Unmount functions
void printUnmountedAndErrors(bool invalidInput, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);
void listMountedISOs();
void unmountISOs();
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors);


//	stds

// General functions
std::string shell_escape(const std::string& s);
std::pair<std::string, std::string> extractDirectoryAndFilename(const std::string& path);
std::string readInputLine(const std::string& prompt);


// Cache functions
std::future<bool> iequals(std::string_view a, std::string_view b);
std::future<bool> FileExists(const std::string& path);
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
std::vector<std::string> findFiles(const std::vector<std::string>& paths, const std::string& mode, const std::function<void(const std::string&, const std::string&)>& callback);


// voids
void verboseFind();
void verboseConversion();
void select_and_convert_files_to_iso(const std::string& fileTypeChoice);
void processInput(const std::string& input, const std::vector<std::string>& fileList, const std::string& inputPaths, bool flag);
void printFileList(const std::vector<std::string>& fileList);

// bools
bool fileExistsConversions(const std::string& fullPath);


// BIN/IMG CONVERSION

// bools
bool isCcd2IsoInstalled();


// voids
void convertBINToISO(const std::string& inputPath);


// MDF/MDS CONVERSION

// bools
bool isMdf2IsoInstalled();


// voids
void convertMDFToISO(const std::string& inputPath);

#endif // HEADERS_H
