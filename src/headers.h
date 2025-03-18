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


// Global alias (fs) for the std::filesystem
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

// Cache for original full paths
extern std::unordered_map<std::string, std::string> originalPathsCache;

// Cache for directory and filename transformations
extern std::unordered_map<std::string, std::string> transformationCache;

// Cache for mount-points
extern std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>> cachedParsesForUmount;

// Memory cached binImgFiles here
extern std::vector<std::string> binImgFilesCache;

// Memory cached mdfImgFiles here
extern std::vector<std::string> mdfMdsFilesCache;

// Memory cached nrgImgFiles here
extern std::vector<std::string> nrgFilesCache; 

// Holds IsoCache directory name
extern const std::string cacheFileName;

// Holds IsoCache directory path
extern const std::string databaseFilePath;

// Holds folder history directory path
extern const std::string historyFilePath;

// Holds filter history
extern const std::string filterHistoryFilePath;

// Limit for max stored path entries
extern const int MAX_HISTORY_LINES;

// Limit for max stored filter entries
extern const int MAX_HISTORY_PATTERN_LINES;

// Holds configuration path
extern const std::string configPath;

// For signaling cancellations
extern std::atomic<bool> g_operationCancelled;

// Max cache size limit for IsoCache
extern const uintmax_t maxDatabaseSize;

// Hold current page pagination
extern size_t currentPage;

// Hold max entries per page for pagination
extern size_t ITEMS_PER_PAGE;

// Global variable to lock the program into one instance
extern int lockFileDescriptor;


// ISO COMMANDER

// bools
bool readUserConfigUpdates(const std::string& filePath);

bool paginationSet(const std::string& filePath);

bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, std::atomic<bool>& isAtISOList);

bool isValidInput(const std::string& input);

bool isHistoryFileEmpty(const std::string& filePath);

bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess);

bool isValidDirectory(const std::string& path);

bool saveToDatabase(const std::vector<std::string>& isoFiles, std::atomic<bool>& newISOFound);

bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess);

bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, const std::string& operation, const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount);

bool blacklist(const std::filesystem::path& entry, const bool& blacklistMdf, const bool& blacklistNrg);

char** completion_cb(const char* text, int start, int end);

bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex);

// ints
int prevent_readline_keybindings(int, int);

int clear_screen_and_buffer(int, int);

// voids
void printMenu();

void submenu1(std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);

void submenu2(std::atomic<bool>& newISOFound);

void print_ascii();


void flushStdin();

void disable_ctrl_d();

void enable_ctrl_d();

void disableInput();

void customListingsFunction(char **matches, int num_matches, int max_length);

void restoreInput();

void restoreReadline();

void disableReadlineForConfirmation();

void signalHandler(int signum);

void setupSignalHandlerCancellations();

void signalHandlerCancellations(int signal);

void clearScrollBuffer();

void setupReadlineToIgnoreCtrlC();

void sortFilesCaseInsensitive(std::vector<std::string>& files);

void updatePagination(const std::string& inputSearch, const std::string& configPath);

void clearMessageAfterTimeout(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive);

void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

void processIsoOperations(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& operationBreak, bool& verbose, bool isUnmount);

void helpSelections();

void helpSearches(bool isCpMv, bool import2ISO);

void helpMappings();

void setDisplayMode(const std::string& inputSearch);

void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages);

void selectForIsoFiles(const std::string& operation, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);

void printList(const std::vector<std::string>& items, const std::string& listType, const std::string& listSubType, std::vector<std::string>& pendingIndices, bool& hasPendingProcess);

void tokenizeInput(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<int>& processedIndices);

void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose, const std::string& operation);

void verbosePrint(std::unordered_set<std::string>& primarySet, std::unordered_set<std::string>& secondarySet, std::unordered_set<std::string>& tertiarySet, std::unordered_set<std::string>& errorSet, int verboseLevel);

void resetVerboseSets(std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts);

void reportErrorCpMvRm(const std::string& errorType, const std::string& srcDir, const std::string& srcFile, const std::string& destDir, const std::string& errorDetail, const std::string& operation, std::vector<std::string>& verboseErrors, std::atomic<size_t>* failedTasks, std::atomic<bool>& operationSuccessful, const std::function<void()>& batchInsertFunc);

void verboseForDatabase(std::vector<std::string>& allIsoFiles, std::atomic<size_t>& totalFiles, std::vector<std::string>& validPaths, std::unordered_set<std::string>& invalidPaths, std::unordered_set<std::string>& uniqueErrorMessages, bool& promptFlag, int& maxDepth, bool& filterHistory, const std::chrono::high_resolution_clock::time_point& start_time, std::atomic<bool>& newISOFound);

void verboseFind(std::unordered_set<std::string>& invalidDirectoryPaths, const std::vector<std::string>& directoryPaths, std::unordered_set<std::string>& processedErrorsFind);

void verboseSearchResults(const std::string& fileExtension, std::unordered_set<std::string>& fileNames, std::unordered_set<std::string>& invalidDirectoryPaths, bool newFilesFound, bool list, int currentCacheOld, const std::vector<std::string>& files, const std::chrono::high_resolution_clock::time_point& start_time, std::unordered_set<std::string>& processedErrorsFind, std::vector<std::string>& directoryPaths);

void loadHistory(bool& filterHistory);

void saveHistory(bool& filterHistory);

void clearHistory(const std::string& inputSearch);

void mountIsoFiles(const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& mountedFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& mountedFails, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks);

void unmountISO(const std::vector<std::string>& isoDirs, std::unordered_set<std::string>& unmountedFiles, std::unordered_set<std::string>& unmountedErrors, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks);

void databaseSwitches(std::string& inputSearch, const bool& promptFlag, const int& maxDepth, const bool& filterHistory, std::atomic<bool>& newISOFound);
void loadFromDatabase(std::vector<std::string>& isoFiles);

void refreshForDatabase(std::string& initialDir, bool promptFlag, int maxDepth, bool filterHistory, std::atomic<bool>& newISOFound);

void traverse(const std::filesystem::path& path, std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages, std::atomic<size_t>& totalFiles, std::mutex& traverseFilesMutex, std::mutex& traverseErrorsMutex, int& maxDepth, bool& promptFlag);

void backgroundDatabaseImport(std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound);

void displayConfigurationOptions(const std::string& configPath);

void displayDatabaseStatistics(const std::string& databaseFilePath, std::uintmax_t maxDatabaseSize, const std::unordered_map<std::string, std::string>& transformationCache, const std::vector<std::string>& globalIsoFileList);

void removeNonExistentPathsFromDatabase();

void handleIsoFileOperation(const std::vector<std::string>& isoFiles, const std::vector<std::string>& isoFilesCopy, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, const std::string& userDestDir, bool isMove, bool isCopy, bool isDelete, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, bool overwriteExisting);

void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& fileExtensionWithOutDots, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort);
void toLowerInPlace(std::string& str);

void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages);

void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, const bool& modeMdf, const bool& modeNrg, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks,
 std::atomic<size_t>* failedTasks, std::atomic<bool>& newISOFound);
 
void clearAndLoadImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, 
bool& list, std::vector<std::string>& pendingIndices, bool& hasPendingProcess);

void promptSearchBinImgMdfNrg(const std::string& fileTypeChoice, std::atomic<bool>& newISOFound);

void selectForImageFiles(const std::string& fileType, std::vector<std::string>& files, std::atomic<bool>& newISOFound, bool& list);

void processInput(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, 
bool& verbose, bool& needsScrnClr, std::atomic<bool>& newISOFound);

void processOperationInput(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, 
bool& verbose, std::atomic<bool>& newISOFound);

// stds
std::map<std::string, std::string> readConfig(const std::string& configPath);

std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);

std::string trimWhitespace(const std::string& str);

std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path, const std::string& location);

std::string userDestDirRm(const std::vector<std::string>& isoFiles, std::vector<std::vector<int>>& indexChunks, std::unordered_set<std::string>& uniqueErrorMessages, std::string& userDestDir, std::string& operationColor, std::string& operationDescription, bool& umountMvRmBreak, bool& filterHistory, bool& isDelete, bool& isCopy, bool& abortDel, bool& overwriteExisting);

std::string handlePaginatedDisplay(const std::vector<std::string>& entries, std::unordered_set<std::string>& uniqueErrorMessages, const std::string& promptPrefix, const std::string& promptSuffix, const std::function<void()>& setupEnvironmentFn, bool& isPageTurn);

std::vector<std::string> filterFiles(const std::vector<std::string>& files, const std::string& query);

std::tuple<std::string, std::string, std::string> parseMountPointComponents(std::string_view dir);


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
