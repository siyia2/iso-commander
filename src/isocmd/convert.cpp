// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Function to check if a file already exists for conversion output
bool fileExists(const std::string& fullPath) {
        return std::filesystem::exists(fullPath);
}


// Function to convert a BIN/IMG/MDF/NRG file to ISO format
void convertToISO(const std::vector<std::string>& imageFiles, std::unordered_set<std::string>& successOuts, std::unordered_set<std::string>& skippedOuts, std::unordered_set<std::string>& failedOuts, const bool& modeMdf, const bool& modeNrg, std::atomic<size_t>* completedBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, std::atomic<bool>& newISOFound) {

    namespace fs = std::filesystem;

    // Batch size constant for inserting entries into sets
    const size_t BATCH_SIZE = 1000;

    // Collect unique directories from input file paths
    std::unordered_set<std::string> uniqueDirectories;
    for (const auto& filePath : imageFiles) {
        std::filesystem::path path(filePath);
        if (path.has_parent_path()) {
            uniqueDirectories.insert(path.parent_path().string());
        }
    }

    std::string result = std::accumulate(uniqueDirectories.begin(), uniqueDirectories.end(), std::string(), 
        [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ";" + b; });

    uid_t real_uid;
    gid_t real_gid;
    std::string real_username;
    std::string real_groupname;
    
    getRealUserId(real_uid, real_gid, real_username, real_groupname);

    // Thread-local message buffers to reduce lock contention
    std::vector<std::string> localSuccessMsgs, localFailedMsgs, localSkippedMsgs, localDeletedMsgs;

    // Function to check if any buffer has reached the batch size and flush if needed
    auto batchInsertMessages = [&]() {
        bool shouldFlush = 
            localSuccessMsgs.size() >= BATCH_SIZE ||
            localFailedMsgs.size() >= BATCH_SIZE ||
            localSkippedMsgs.size() >= BATCH_SIZE;
            
        if (shouldFlush) {
            std::lock_guard<std::mutex> lock(globalSetsMutex);
            successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
            failedOuts.insert(localFailedMsgs.begin(), localFailedMsgs.end());
            skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
            
            localSuccessMsgs.clear();
            localFailedMsgs.clear();
            localSkippedMsgs.clear();
        }
    };

    for (const std::string& inputPath : imageFiles) {
        auto [directory, fileNameOnly] = extractDirectoryAndFilename(inputPath, "conversions");

        if (!fs::exists(inputPath)) {
            localFailedMsgs.push_back(
                "\033[1;35mMissing: \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;35m.\033[0;1m");

            // Select the appropriate cache based on the mode.
            auto& cache = modeNrg ? nrgFilesCache :
                            (modeMdf ? mdfMdsFilesCache : binImgFilesCache);
            cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::ifstream file(inputPath);
        if (!file.good()) {
            localFailedMsgs.push_back("\033[1;91mThe specified file \033[1;93m'" + inputPath + "'\033[1;91m cannot be read. Check permissions.\033[0;1m");
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::string outputPath = inputPath.substr(0, inputPath.find_last_of(".")) + ".iso";
        if (fileExists(outputPath)) {
            localSkippedMsgs.push_back("\033[1;93mThe corresponding .iso file already exists for: \033[1;92m'" + directory + "/" + fileNameOnly + "'\033[1;93m. Skipped conversion.\033[0;1m");
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
            
            // Check if we need to batch insert
            batchInsertMessages();
            continue;
        }

        std::atomic<bool> conversionSuccess(false); // Atomic boolean for thread safety
        if (modeMdf) {
            conversionSuccess = convertMdfToIso(inputPath, outputPath, completedBytes);
        } else if (!modeMdf && !modeNrg) {
            conversionSuccess = convertCcdToIso(inputPath, outputPath, completedBytes);
        } else if (modeNrg) {
            conversionSuccess = convertNrgToIso(inputPath, outputPath, completedBytes);
        }

        auto [outDirectory, outFileNameOnly] = extractDirectoryAndFilename(outputPath, "conversions");

        if (conversionSuccess) {
            [[maybe_unused]] int ret = chown(outputPath.c_str(), real_uid, real_gid); // Attempt to change ownership, ignore result
            std::string fileNameLower = fileNameOnly;
			toLowerInPlace(fileNameLower);

			std::string fileType = 
								fileNameLower.ends_with(".bin") ? "\033[0;1m.bin" : 
								fileNameLower.ends_with(".img") ? "\033[0;1m.bin" : 
								fileNameLower.ends_with(".mdf") ? "\033[0;1m.mdf" : 
								fileNameLower.ends_with(".nrg") ? "\033[0;1m.nrg" : "\033[0;1mImage";

			localSuccessMsgs.push_back(fileType + " file converted to ISO: \033[1;92m'" + outDirectory + "/" + outFileNameOnly + "'\033[0;1m.\033[0;1m");
            completedTasks->fetch_add(1, std::memory_order_acq_rel);
        } else {
			if (fs::exists(outputPath)) std::remove(outputPath.c_str());
            localFailedMsgs.push_back("\033[1;91mConversion of \033[1;93m'" + directory + "/" + fileNameOnly + "'\033[1;91m " + 
                                      (g_operationCancelled.load() ? "cancelled" : "failed") + ".\033[0;1m");
            failedTasks->fetch_add(1, std::memory_order_acq_rel);
        }
        
        // Check if we need to batch insert
        batchInsertMessages();
    }

    // Insert any remaining messages under one lock
    {
        std::lock_guard<std::mutex> lock(globalSetsMutex);
        successOuts.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
        failedOuts.insert(localFailedMsgs.begin(), localFailedMsgs.end());
        skippedOuts.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
    }

    // Update cache and prompt flags
    if (!successOuts.empty()) {
		bool promptFlag = false;
		bool filterHistory = false;
		int maxDepth = 0;
        refreshForDatabase(result, promptFlag, maxDepth, filterHistory, newISOFound);
    }
}
