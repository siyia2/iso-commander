// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// Function to automatically update ISO list if auto-update is on
void refreshListAfterAutoUpdate(int timeoutSeconds, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& updateHasRun, bool& umountMvRmBreak, std::vector<std::string>& filteredFiles, bool& isFiltered, std::string& listSubtype, std::vector<std::string>& pendingIndices, bool& hasPendingProcess, std::atomic<bool>& newISOFound) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));
        
        if (!isImportRunning.load()) {
			if (newISOFound.load() && isAtISOList.load()) {
				
				clearAndLoadFiles(filteredFiles, isFiltered, listSubtype, umountMvRmBreak, pendingIndices, hasPendingProcess);
            
				std::cout << "\n";
				rl_on_new_line(); 
				rl_redisplay();
			}
            updateHasRun.store(false);
            newISOFound.store(false);
            
            break;
        }
    }
}


// Utility function to clear screen buffer and load IsoFiles from database to a global vector only for the first time and only for if the database file has been modified.
bool clearAndLoadFiles(std::vector<std::string>& filteredFiles, bool& isFiltered, const std::string& listSubType, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess) {
    
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();
    
    static std::filesystem::file_time_type lastModifiedTime;

    // Check if the database file exists and has been modified
    bool needToReload = false;
    if (std::filesystem::exists(databaseFilePath)) {
        std::filesystem::file_time_type currentModifiedTime = 
            std::filesystem::last_write_time(databaseFilePath);

        if (lastModifiedTime == std::filesystem::file_time_type{}) {
            // First time checking, always load
            needToReload = true;
        } else if (currentModifiedTime > lastModifiedTime) {
            // Cache file has been modified since last load
            needToReload = true;
        }

        // Update last modified time
        lastModifiedTime = currentModifiedTime;
    } else {
        // Cache file doesn't exist, need to load
        needToReload = true;
    }

    // Common operations
    clearScrollBuffer();
    if (needToReload) {
        loadFromDatabase(globalIsoFileList);
        {
            std::lock_guard<std::mutex> lock(updateListMutex);
            // Clear any pending automatically unless user is on filtered list(already filtered lists are unaffected by main list changes)
            if (!isFiltered) {
				pendingIndices.clear();
				hasPendingProcess = false;
			}
			// Optimization: sort only if (needToReload) from database
            sortFilesCaseInsensitive(globalIsoFileList);
        }
    }
    
    // Lock to prevent simultaneous access to std::cout
    {
        std::lock_guard<std::mutex> printLock(couNtMutex);
        if (umountMvRmBreak) {
			if (isFiltered) {
				// Reset pending flags if filtered but only for destructive list actions mv/rm (because they can modify already filtered lists)
				pendingIndices.clear();
				hasPendingProcess = false;
				currentPage = 0;
			}
            isFiltered = false;
            
        }
        printList(isFiltered ? filteredFiles : globalIsoFileList, "ISO_FILES", listSubType, pendingIndices, hasPendingProcess);
        
        if (globalIsoFileList.empty()) {
            std::cout << "\033[1;93mISO Cache is empty. Choose 'ImportISO' from the Main Menu Options.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return false;
        }
    }

    return true;
}


// Mounpoint location
const std::string MOUNTED_ISO_PATH = "/mnt";

// Function to load and display mount-points
bool loadAndDisplayMountedISOs(std::vector<std::string>& isoDirs, std::vector<std::string>& filteredFiles, bool& isFiltered, bool& umountMvRmBreak, std::vector<std::string>& pendingIndices, bool& hasPendingProcess) {
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
        // reset pending if list changed
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
		if (isFiltered) currentPage = 0;
		std::vector<std::string>().swap(filteredFiles); 
        isFiltered = false;
        
    }
    printList(isFiltered ? filteredFiles : isoDirs, "MOUNTED_ISOS", "", pendingIndices, hasPendingProcess);

    return true;
}



// Function to clear and load list for image files
void clearAndLoadImageFiles(std::vector<std::string>& files, const std::string& fileType, bool& need2Sort, bool& isFiltered, bool& list,std::vector<std::string>& pendingIndices, bool& hasPendingProcess) {
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
            
    if (!list) {
		if (need2Sort) {
			sortFilesCaseInsensitive(files); // Sort the files case-insensitively
				(fileType == "bin" || fileType == "img") 
					? sortFilesCaseInsensitive(binImgFilesCache) 
						: fileType == "mdf" 
							? sortFilesCaseInsensitive(mdfMdsFilesCache) 
						: sortFilesCaseInsensitive(nrgFilesCache);
		}
			need2Sort = false;
	}
	
    printList(files, "IMAGE_FILES", "conversions", pendingIndices, hasPendingProcess); // Print the current list of files
}


//Function to disblay errors from tokenization
void displayErrors(std::unordered_set<std::string>& uniqueErrorMessages) {
    // Display user input errors at the top
    if (!uniqueErrorMessages.empty()) {
        std::cout << "\n";
        for (const auto& err : uniqueErrorMessages) {
            std::cout << err << "\n";
        }
        uniqueErrorMessages.clear();
    }
}

