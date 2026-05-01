// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SELECT_H
#define SELECT_H

/**
 * @brief Shared display state for the ISO list view and its background refresh thread.
 *
 * Owned via shared_ptr by selectForIsoFiles and any detached refreshListAfterAutoUpdate
 * threads, ensuring the data outlives the caller without dangling references.
 */
struct RefreshState {
    std::vector<std::string> filteredFiles;
    std::vector<std::string> pendingIndices;
    bool isFiltered;
    bool hasPendingProcess;
    bool umountMvRmBreak;
    std::string listSubtype;
    size_t currentPage;
    size_t originalPage;
};

bool processPaginationHelpAndDisplay(const std::string& command, size_t& totalPages, size_t& currentPage, bool& isFiltered, 
bool& needsClrScrn, const bool isMount, const bool isUnmount, const bool isWrite, const bool isConversion, bool& need2Sort, 
std::atomic<bool>& isAtISOList);
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, 
bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, 
size_t& originalPage, std::atomic<bool>& isImportRunning);
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, 
bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, 
size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning);
bool handleFilteringForISO(const std::string& inputString, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& needsClrScrn, 
bool& filterHistory, const std::string& operation, const std::string& operationColor, const std::vector<std::string>& isoDirs, bool isUnmount, size_t& currentPage);
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, 
std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, 
bool& operationBreak, bool& verbose, bool isUnmount);
void processInputForCpMvRm(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, 
std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, 
bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound);
void handleSelectIsoFilesResults(std::unordered_set<std::string>& uniqueErrorMessages, std::unordered_set<std::string>& operationFiles, 
std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& skippedMessages, const std::string& operation, 
bool& verbose, bool isMount, bool& isFiltered, bool& umountMvRmBreak, bool isUnmount, bool& needsClrScrn);
void handleFilteringConvert2ISO(const std::string& mainInputString, std::vector<std::string>& files, const std::string& operation, bool& isFiltered, bool& needsClrScrn, bool& filterHistory, bool& need2Sort, size_t& currentPage);
void toLowerInPlace(std::string& str);
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles, std::unordered_set<std::string>& uniqueErrorMessages);
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list, std::vector<std::string>& pendingIndices, 
bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning);
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList, const bool& modeMdf, const bool& modeNrg, const bool& modeChd, const bool& modeDaa, 
std::unordered_set<std::string>& processedErrors, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, 
std::unordered_set<std::string>& failedOuts, bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound);

#endif // SELECT_H
