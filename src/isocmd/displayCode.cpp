// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../filtering.h"


// Default Display config options for lists
namespace displayConfig {
    bool toggleFullListMount = false;
    bool toggleFullListUmount = true;
    bool toggleFullListCpMvRm = false;
    bool toggleFullListWrite = false;
    bool toggleFullListConversions = false;
    bool toggleNamesOnly = false;
}


// Function to load and display ISO files from the database into a global vector, database file is used only on first access or on every modification
bool loadAndDisplayIso(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess,
size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning) {

    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    static std::mutex lastModifiedMutex;
    static std::filesystem::file_time_type lastModifiedTime;

    bool needToReload = false;

    {
        std::lock_guard<std::mutex> lock(lastModifiedMutex);
        if (std::filesystem::exists(databaseFilePath)) {
            std::filesystem::file_time_type currentModifiedTime =
                std::filesystem::last_write_time(databaseFilePath);
            if (lastModifiedTime == std::filesystem::file_time_type{} ||
                currentModifiedTime > lastModifiedTime) {
                needToReload = true;
            }
            lastModifiedTime = currentModifiedTime;
        } else {
            needToReload = true;
            lastModifiedTime = std::filesystem::file_time_type{};
        }
    }

    clearScrollBuffer();

    // Load into a local variable under dbFileMutex only — no updateListMutex held here
    // This breaks the lock-ordering inversion that existed when loadFromDatabase
    // was called from inside the updateListMutex block below
    std::vector<std::string> freshList;
    if (needToReload) {
        loadFromDatabase(freshList);  // acquires and releases dbFileMutex internally
    }

    // Now acquire updateListMutex alone to update shared state and print
    // Order: dbFileMutex already released → updateListMutex — consistent everywhere
    bool isEmpty = false;
    {
        std::lock_guard<std::mutex> lock(updateListMutex);

        if (needToReload) {
            globalIsoFileList = std::move(freshList);
            currentPage = originalPage;
            pendingIndices.clear();
            hasPendingProcess = false;

            if (isFiltered) {
                filteringStack.clear();
                isFiltered = false;
            }

            sortFilesCaseInsensitive(globalIsoFileList);
        }
        
        // To fix sorting order when changing to filename_only lists from within the program
        if (needSortingAfterflno) {
			sortFilesCaseInsensitive(globalIsoFileList);
			needSortingAfterflno = false;
		}

        if (umountMvRmBreak) {
            filteringStack.clear();
            isFiltered = false;
        }

        printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES", listSubType,
                  pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);

        isEmpty = globalIsoFileList.empty();
    }

    if (isEmpty) {
        std::cout << "\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return false;
    }

    return true;
}


// Mounpoint location
const std::string MOUNTED_ISO_PATH = "/mnt";


// Function to load and display mount-points
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, size_t& originalPage, std::atomic<bool>& isImportRunning) {
    signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
    disable_ctrl_d();
    
    // Static cache: previous hash and sorted directories.
    static size_t previousHash = 0;
    static std::vector<std::string> lastSortedDirs;
    std::vector<std::string> newIsoDirs;

	// Collect directories
	for (const auto& entry : std::filesystem::directory_iterator(MOUNTED_ISO_PATH)) {
		if (entry.is_directory()) {
			auto filename = entry.path().filename().string();
			if (filename.find("iso_") == 0) { // filename.starts_with("iso_")
				newIsoDirs.push_back(entry.path().string());
			}
		}
	}

    // Compute an order-independent hash:
    size_t currentHash = 0;
    for (const auto& path : newIsoDirs) {
        currentHash += std::hash<std::string>{}(path);
    }
    currentHash += newIsoDirs.size();

    // Optimization: sort only if the set of directories has changed
    if (currentHash != previousHash) {
        sortFilesCaseInsensitive(newIsoDirs);
        // Cache the sorted vector and update the hash
        lastSortedDirs = newIsoDirs;
        previousHash = currentHash;
        // reset pending if list has changed
        pendingIndices.clear();
		hasPendingProcess = false;
    } else {
        // Reuse the cached sorted vector if nothing has changed
        newIsoDirs = lastSortedDirs;
    }

    isoDirs = std::move(newIsoDirs);

    if (isoDirs.empty()) {
        clearScrollBuffer();
        std::cerr << "\n\033[1;93mNo paths matching the '/mnt/iso_{name}' pattern found.\033[0m\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0m\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::vector<std::string>().swap(isoDirs); // De-allocate memory for static vector
        std::unordered_map<std::string, std::tuple<std::string, std::string, std::string>>().swap(cachedParsesForUmount); //De-allocate memory map when no mounpoints exist
        return false;
    }

    clearScrollBuffer();

    if (filteredFiles.size() == isoDirs.size() || umountMvRmBreak) {
		// Restore original page in unfiltered list if possible
		originalPage = currentPage;
		// Clear the filtering stack when returning to unfiltered mode
        filteringStack.clear();
        isFiltered = false; 
        // Clear filteredFiles
		filteredFiles.clear();
    }
    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning);

    return true;
}


// Function to load and display the corresponding image files
void loadAndDisplayImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,std::vector<std::string>& pendingIndices, bool& hasPendingProcess, size_t& currentPage, std::atomic<bool>& isImportRunning) {
    // Clear the screen for new content
    clearScrollBuffer();
    
    // Optimization: sort only if found files are different from cache or it is their first listing
	files = 
    (!isFiltered && !binImgFilesCache.empty() && (fileType == "bin" || fileType == "img") &&
     (binImgFilesCache.size() != files.size() || !std::equal(binImgFilesCache.begin(), binImgFilesCache.end(), files.begin())))
        ? (need2Sort = true, binImgFilesCache) 
    : (!isFiltered && !mdfMdsFilesCache.empty() && fileType == "mdf" &&
       (mdfMdsFilesCache.size() != files.size() || !std::equal(mdfMdsFilesCache.begin(), mdfMdsFilesCache.end(), files.begin())))
        ? (need2Sort = true, mdfMdsFilesCache) 
    : (!isFiltered && !nrgFilesCache.empty() && fileType == "nrg" &&
       (nrgFilesCache.size() != files.size() || !std::equal(nrgFilesCache.begin(), nrgFilesCache.end(), files.begin())))
        ? (need2Sort = true, nrgFilesCache) 
    : files;
            
    if (!list || (list && needSortingAfterflno)) {
		if (need2Sort) {
			sortFilesCaseInsensitive(files); // Sort the files case-insensitively
				if (fileType == "bin" || fileType == "img") {
					std::lock_guard<std::mutex> lock(binImgCacheMutex);
					sortFilesCaseInsensitive(binImgFilesCache);
				} else if (fileType == "mdf") {
					std::lock_guard<std::mutex> lock(mdfMdsCacheMutex);
					sortFilesCaseInsensitive(mdfMdsFilesCache);
				} else {
					std::lock_guard<std::mutex> lock(nrgCacheMutex);
					sortFilesCaseInsensitive(nrgFilesCache);
				}
			}
			needSortingAfterflno = false;
			need2Sort = false;
	}
	
    printList(files, "IMAGE_FILES", "conversions", pendingIndices, hasPendingProcess, isFiltered, currentPage, isImportRunning); // Print the current list of files
}
