// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../threadpool.h"
#include "../mdf.h"
#include "../ccd.h"
#include "../themes.h"
#include "../daa2iso.h"
#include "../chd.h"
#include <chd.h>

/**
 * @file operations.cpp
 * @brief High-level processing functions for mounting, file management (Cp/Mv/Rm), and format conversions.
 */

/**
 * @brief Orchestrates the mounting or unmounting of ISO files using a static thread pool and progress tracking.
 * * @param input Raw user input string (indices or "00").
 * @param files List of available files.
 * @param operationFiles Set to populate with successfully processed files.
 * @param skippedMessages Set to track items skipped during the operation.
 * @param operationFails Set to track failed file paths.
 * @param uniqueErrorMessages Set to collect unique error strings.
 * @param operationBreak Boolean flag to control outer loop flow.
 * @param verbose Toggle for detailed progress output.
 * @param isUnmount True for unmount operation, false for mount.
 */
void processInputForMountOrUmount(const std::string& input, const std::vector<std::string>& files, std::unordered_set<std::string>& operationFiles, std::unordered_set<std::string>& skippedMessages, std::unordered_set<std::string>& operationFails, std::unordered_set<std::string>& uniqueErrorMessages, bool& operationBreak, bool& verbose, bool isUnmount) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);
    
    std::unordered_set<int> indicesToProcess;
    
    if (input == "00") {
        for (int i = 1; i <= static_cast<int>(files.size()); ++i) {
            indicesToProcess.insert(i);
        }
    } else {
        tokenizeInput(input, files, uniqueErrorMessages, indicesToProcess);
        if (indicesToProcess.empty()) {
            if (isUnmount) operationBreak = false;
            return;
        }
    }
    
    std::vector<std::string> selectedFiles;
    selectedFiles.reserve(indicesToProcess.size());
    for (int index : indicesToProcess) {
        selectedFiles.push_back(files[index - 1]);
    }
    
    std::string operationColor = std::string(isUnmount ? originalColors::yellow : originalColors::green);
    std::string operationName = isUnmount ? "umount" : "mount";
    
    std::cout << originalColors::boldAlt << "\n Processing" 
          << (selectedFiles.size() > 1 ? " tasks" : " task") 
          << " for " << operationColor << operationName 
          << originalColors::boldAlt << "... (" 
          << originalColors::red << "Ctrl+c" 
          << originalColors::boldAlt << ":cancel)\n";
    
    std::string coloredProcess = std::string(operationColor) + operationName + std::string(originalColors::boldAlt);;
    
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isUnmount ? UMOUNT_THREAD_CAP : MOUNT_THREAD_CAP;
    size_t numThreads = std::max(size_t(2), std::min({selectedFiles.size(), cap, poolSize}));

    if ((selectedFiles.size() + numThreads - 1) / numThreads > 100) {
        numThreads = (selectedFiles.size() + 99) / 100;
    }

    std::vector<std::vector<std::string>> chunks(numThreads);
    for (size_t i = 0; i < selectedFiles.size(); ++i) {
        chunks[i % numThreads].push_back(std::move(selectedFiles[i]));
    }
    
    std::vector<std::future<void>> futures;
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);
    
    std::thread progressThread(
        displayProgressBarWithSize, 
        nullptr,
        static_cast<size_t>(0),
        &completedTasks,
        &failedTasks,
        selectedFiles.size(),
        &isProcessingComplete,
        &verbose,
        std::string(coloredProcess)
    );
    
    for (const auto& chunk : chunks) {
        futures.emplace_back(pool.enqueue([&, chunk]() {
            if (g_operationCancelled.load()) return;
            
            if (isUnmount) {
                unmountISO(chunk, operationFiles, operationFails, &completedTasks, &failedTasks, false);
            } else {
                mountIsoFiles(chunk, operationFiles, skippedMessages, operationFails, &completedTasks, &failedTasks, false);
            }
        }));
    }
    
    for (auto& future : futures) {
        future.wait();
    }
    
    if (completedTasks == 0 && isUnmount) operationBreak = false;
    
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();
}

/**
 * @brief Groups file indices into chunks for parallel processing, preventing race conditions by grouping identical filenames.
 * * @param processedIndices Set of indices selected by the user.
 * @param isoFiles Master list of file paths.
 * @param numThreads Desired concurrency level.
 * @param isDelete Flag indicating if the operation is a deletion (avoids name-collision logic).
 * @return A vector of chunks, where each chunk is a vector of indices.
 */
std::vector<std::vector<int>> groupFilesIntoChunksForCpMvRm(const std::unordered_set<int>& processedIndices, const std::vector<std::string>& isoFiles, unsigned int numThreads, bool isDelete) 
{
    std::vector<int> processedIndicesVector(processedIndices.begin(), processedIndices.end());
    std::vector<std::vector<int>> indexChunks;

    if (processedIndicesVector.empty()) return indexChunks;

    if (!isDelete) {
        std::unordered_map<std::string, std::vector<int>> groups;
        for (int idx : processedIndicesVector) {
            std::string baseName = std::filesystem::path(isoFiles[idx - 1]).filename().string();
            groups[baseName].push_back(idx);
        }

        std::vector<int> uniqueNameFiles;
        for (auto& [baseName, indices] : groups) {
            if (indices.size() > 1) {
                indexChunks.push_back(std::move(indices));
            } else {
                uniqueNameFiles.push_back(indices[0]);
            }
        }

        if (!uniqueNameFiles.empty()) {
            size_t usedChunks = indexChunks.size();
            size_t remainingThreads = (numThreads > usedChunks) ? numThreads - usedChunks : 1;

            size_t maxFilesPerChunk = std::max<size_t>(1, (uniqueNameFiles.size() + remainingThreads - 1) / remainingThreads);

            for (size_t i = 0; i < uniqueNameFiles.size(); i += maxFilesPerChunk) {
                auto end = std::min(i + maxFilesPerChunk, uniqueNameFiles.size());
                indexChunks.emplace_back(uniqueNameFiles.begin() + i, uniqueNameFiles.begin() + end);
            }
        }
    } else {
        size_t maxFilesPerChunk = std::max<size_t>(1, (processedIndicesVector.size() + numThreads - 1) / numThreads);
        for (size_t i = 0; i < processedIndicesVector.size(); i += maxFilesPerChunk) {
            auto end = std::min(i + maxFilesPerChunk, processedIndicesVector.size());
            indexChunks.emplace_back(processedIndicesVector.begin() + i, processedIndicesVector.begin() + end);
        }
    }

    return indexChunks;
}

/**
 * @brief Sums the physical file sizes of a given list of file paths.
 * * @param files Vector of file paths.
 * @return Total size in bytes.
 */
size_t getTotalFileSize(const std::vector<std::string>& files) {
    size_t totalSize = 0;
    for (const auto& file : files) {
        struct stat st;
        if (stat(file.c_str(), &st) == 0) {
            totalSize += st.st_size;
        }
    }
    return totalSize;
}

/**
 * @brief Handles bulk copy, move, or remove operations with threading and progress visualization.
 * * @param input Raw user input.
 * @param isoFiles Master list of files.
 * @param process Operation type ("cp", "mv", or "rm").
 * @param operationIsos Set to track successfully modified items.
 * @param operationErrors Set to track failed items.
 * @param uniqueErrorMessages Set for unique UI error reporting.
 * @param umountMvRmBreak Flag for outer loop control.
 * @param filterHistory Flag indicating if the view is filtered.
 * @param verbose Detailed output toggle.
 * @param newISOFound Atomic flag for filesystem changes.
 */
void processInputForCpMvRm(const std::string& input, const std::vector<std::string>& isoFiles, const std::string& process, std::unordered_set<std::string>& operationIsos, std::unordered_set<std::string>& operationErrors, std::unordered_set<std::string>& uniqueErrorMessages, bool& umountMvRmBreak, bool& filterHistory, bool& verbose, std::atomic<bool>& newISOFound) {
    setupSignalHandlerCancellations();
    
    bool overwriteExisting = false;
    std::string userDestDir;
    std::unordered_set<int> processedIndices;

    bool isDelete = (process == "rm");
    bool isMove   = (process == "mv");
    bool isCopy   = (process == "cp");
    
    std::string operationDescription = isDelete ? "*PERMANENTLY DELETED*" : (isMove ? "*MOVED*" : "*COPIED*");
    std::string operationColor = std::string(isDelete ? originalColors::red : (isCopy ? originalColors::green : originalColors::yellow));

    tokenizeInput(input, isoFiles, uniqueErrorMessages, processedIndices);

    if (processedIndices.empty()) {
        umountMvRmBreak = false;
        return;
    }
    
    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t cap = isDelete ? RM_THREAD_CAP : CPMV_THREAD_CAP;
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), cap, poolSize}));
    
    std::vector<std::vector<int>> indexChunks = groupFilesIntoChunksForCpMvRm(processedIndices, isoFiles, numThreads, isDelete);

    bool abortDel = false;
    std::string processedUserDestDir = userDestDirCpMv(isoFiles, indexChunks, uniqueErrorMessages, userDestDir, 
                                                     operationColor, operationDescription, umountMvRmBreak, 
                                                     filterHistory, isDelete, isCopy, abortDel, overwriteExisting);
        
    g_operationCancelled.store(false);
    
    if ((processedUserDestDir == "" && (isCopy || isMove)) || abortDel) {
        uniqueErrorMessages.clear();
        return;
    }
    uniqueErrorMessages.clear();
    clearScrollBuffer();

    std::vector<std::string> filesToProcess;
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(isoFiles[index - 1]);
    }

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    size_t totalBytes = getTotalFileSize(filesToProcess);
    size_t totalTasks = filesToProcess.size();
                 
    if (isCopy || isMove) {
        size_t destCount = std::count(processedUserDestDir.begin(), processedUserDestDir.end(), ';') + 1;
        totalBytes *= destCount;
        totalTasks *= destCount;
    }
    
    std::cout << "\n" << originalColors::boldAlt << " Processing " 
          << (totalTasks > 1 ? "tasks" : "task") << " for " << operationColor << process 
          << originalColors::boldAlt << "... (" 
          << originalColors::red << "Ctrl+c" 
          << originalColors::boldAlt << ":cancel)\n";
             
    std::string coloredProcess = 
    isDelete ? std::string(originalColors::red)    + process + std::string(originalColors::boldAlt) :
    isMove   ? std::string(originalColors::yellow) + process + std::string(originalColors::boldAlt) :
    isCopy   ? std::string(originalColors::green)  + process + std::string(originalColors::boldAlt) :
    process;
    
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes, 
                                 totalBytes, &completedTasks, &failedTasks, 
                                 totalTasks, &isProcessingComplete, &verbose, std::string(coloredProcess));

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> isoFilesInChunk;
        isoFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(isoFilesInChunk),
            [&isoFiles](size_t index) { return isoFiles[index - 1]; });

        futures.emplace_back(pool.enqueue([isoFilesInChunk = std::move(isoFilesInChunk), 
                                             &isoFiles, &operationIsos, &operationErrors, &userDestDir, 
                                             isMove, isCopy, isDelete, &completedBytes, &completedTasks, 
                                             &failedTasks, &overwriteExisting]() {
            handleIsoFileOperation(isoFilesInChunk, isoFiles, operationIsos, operationErrors, 
                                   userDestDir, isMove, isCopy, isDelete, 
                                   &completedBytes, &completedTasks, &failedTasks, overwriteExisting);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }
    
    if (completedTasks == 0) umountMvRmBreak = false;
    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);  
    progressThread.join();
    
    if (!isDelete) {
        bool promptFlag = false;
        int maxDepth = 0;
        refreshForDatabase(userDestDir, promptFlag, maxDepth, filterHistory, newISOFound);
    }

    clear_history();
}

/**
 * @brief Processes user input to convert multiple disk image files to ISO format.
 *
 * This function parses the user-provided input string (e.g., "1-5,7,9"), identifies
 * the corresponding files from the master list, and orchestrates multi‑threaded
 * conversion of supported image types (BIN/CCD, MDF, NRG, CHD, DAA) to standard ISO.
 *
 * The function includes an inline size estimation pass to provide an accurate
 * progress bar during conversion. Estimation logic varies by input format:
 * - **NRG**: Excludes a 300 KiB (307200 byte) header.
 * - **MDF**: Reads sector geometry via `MdfTypeInfo` and computes user data bytes.
 * - **BIN/CCD**: Assumes 2352‑byte raw sectors with 2048 bytes of user data.
 * - **CHD**: Opens the CHD file, inspects the header, and calculates total sectors
 *   multiplied by 2048 bytes (ISO user data per sector).
 * - **DAA**: Calls `getDaaIsoSize()` to query the uncompressed ISO size from the
 *   DAA archive header without extracting the entire file.
 *
 * @param input          Raw user selection string (e.g., "1,3-5").
 * @param fileList       Master vector of all non‑ISO image paths.
 * @param modeMdf        If `true`, treat input files as Alcohol 120% MDF images.
 * @param modeNrg        If `true`, treat input files as Nero NRG images.
 * @param modeChd        If `true`, treat input files as MAME CHD compressed images.
 * @param modeDaa        If `true`, treat input files as PowerISO DAA compressed images.
 * @param processedErrors Set to record any parsing errors (invalid indices/patterns).
 * @param successOuts    Set populated with paths of successfully converted ISOs.
 * @param skippedOuts    Set populated with paths skipped due to cancellation or errors.
 * @param failedOuts     Set populated with paths that failed conversion.
 * @param verbose        If `true`, extra progress details are displayed.
 * @param needsClrScrn   Reference flag set to `true` if the terminal should be cleared.
 * @param newISOFound    Atomic flag set to `true` when at least one new ISO is created.
 *
 * @note The function manages a thread pool and displays a live progress bar.
 *       Cancellation via SIGINT (Ctrl+C) is handled gracefully.
 */
void processInputForConversions(const std::string& input, std::vector<std::string>& fileList,
                               const bool& modeMdf, const bool& modeNrg, const bool& modeChd,
                               const bool& modeDaa,          // <-- new DAA flag
                               std::unordered_set<std::string>& processedErrors,
                               std::unordered_set<std::string>& successOuts,
                               std::unordered_set<std::string>& skippedOuts,
                               std::unordered_set<std::string>& failedOuts,
                               bool& verbose, bool& needsClrScrn, std::atomic<bool>& newISOFound)
{
    setupSignalHandlerCancellations();
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    g_operationCancelled.store(false);
    std::unordered_set<int> processedIndices;

    if (!(input.empty() || std::all_of(input.begin(), input.end(), isspace))) {
        tokenizeInput(input, fileList, processedErrors, processedIndices);
    } else return;

    if (processedIndices.empty()) {
        clearScrollBuffer();
        std::cout << "\n" << (isOrig ? originalColors::red : theme->secondary)
                  << "No valid input provided." << originalColors::boldAlt << "\n";
        std::cout << color << "\n↵ to continue..." << reset;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        needsClrScrn = true;
        return;
    }

    ThreadPool& pool = getStaticThreadPool();
    const size_t poolSize = pool.threadCount();
    const size_t numThreads = std::max(size_t(2), std::min({processedIndices.size(), CONV_THREAD_CAP, poolSize}));

    std::vector<std::vector<size_t>> indexChunks;
    const size_t totalFiles = processedIndices.size();
    const size_t filesPerChunk = (totalFiles + numThreads - 1) / numThreads;

    auto it = processedIndices.begin();
    for (size_t i = 0; i < totalFiles; i += filesPerChunk) {
        auto chunkEnd = std::next(it, std::min(filesPerChunk, static_cast<size_t>(std::distance(it, processedIndices.end()))));
        indexChunks.emplace_back(it, chunkEnd);
        it = chunkEnd;
    }

    std::vector<std::string> filesToProcess;
    filesToProcess.reserve(processedIndices.size());
    for (const auto& index : processedIndices) {
        filesToProcess.push_back(fileList[index - 1]);
    }

    // ========== INLINE SIZE CALCULATION (ALL FORMATS INCLUDING DAA) ==========
    size_t totalBytes = 0;
    for (const auto& file : filesToProcess) {
        std::string ext = file.substr(file.find_last_of(".") + 1);
        toLowerInPlace(ext);

        // CHD mode
		if (modeChd && ext == "chd") {
			chd_file* rawChd = nullptr;
			chd_error err = chd_open(file.c_str(), CHD_OPEN_READ, nullptr, &rawChd);
			if (err == CHDERR_NONE && rawChd) {
				const chd_header* header = chd_get_header(rawChd);
				if (header) {
					uint32_t hunkSize = header->hunkbytes;
					uint32_t rawSectorSize = 0;

					if      (hunkSize % 2448 == 0) rawSectorSize = 2448;
					else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
					else if (hunkSize % 2048 == 0) rawSectorSize = 2048;

					if (rawSectorSize != 0) {
						// Mirrors convertChdToIso: always extracts 2048 user data bytes per sector
						constexpr uint32_t userDataSize = 2048;
						uint32_t sectorsPerHunk = hunkSize / rawSectorSize;
						uint64_t totalSectors = static_cast<uint64_t>(header->totalhunks) * sectorsPerHunk;
						totalBytes += totalSectors * userDataSize;
					}
					// else: unsupported hunk geometry, converter would also reject — skip
				}
				chd_close(rawChd);
			}
		}
        // NRG mode
        else if (modeNrg && ext == "nrg") {
            std::ifstream nrg(file, std::ios::binary | std::ios::ate);
            if (nrg) {
                size_t sz = nrg.tellg();
                totalBytes += (sz > 307200) ? (sz - 307200) : 0;
            }
        }
        // MDF mode
        else if (modeMdf && ext == "mdf") {
            std::ifstream mdf(file, std::ios::binary);
            if (mdf) {
                MdfTypeInfo info;
                if (!info.determineMdfType(mdf)) continue;
                mdf.seekg(0, std::ios::end);
                size_t fileSize = mdf.tellg();
                size_t sectors = fileSize / info.sector_size;
                totalBytes += sectors * info.sector_data;
            }
        }
        // DAA mode
        else if (modeDaa && ext == "daa") {
            uint64_t isoSize = getDaaIsoSize(file);
            if (isoSize > 0) totalBytes += isoSize;
        }
        // BIN/IMG (CCD) mode – default
        else if (!modeMdf && !modeNrg && !modeChd && !modeDaa &&
                 (ext == "bin" || ext == "img" || ext == "ccd")) {
            std::ifstream ccd(file, std::ios::binary | std::ios::ate);
            if (ccd) {
                size_t fileSize = ccd.tellg();
                totalBytes += (fileSize / sizeof(CcdSector)) * DATA_SIZE;
            }
        }
        // If none matched, skip (should not happen)
    }
    // ===========================================================

    size_t totalTasks = filesToProcess.size();
    std::string suffix = (totalTasks > 1 ? " conversions" : " conversion");

    std::string operation;
    if (modeMdf)      operation = std::string(originalColors::orange) + "MDF"     + std::string(originalColors::boldAlt) + suffix;
    else if (modeNrg) operation = std::string(originalColors::orange) + "NRG"     + std::string(originalColors::boldAlt) + suffix;
    else if (modeChd) operation = std::string(originalColors::orange) + "CHD"     + std::string(originalColors::boldAlt) + suffix;
    else if (modeDaa) operation = std::string(originalColors::orange) + "DAA"     + std::string(originalColors::boldAlt) + suffix;
    else              operation = std::string(originalColors::orange) + "BIN/IMG" + std::string(originalColors::boldAlt) + suffix;

    clearScrollBuffer();

    std::cout << "\n" << originalColors::boldAlt << " Processing "
              << operation << originalColors::boldAlt << "... ("
              << originalColors::red << "Ctrl+c"
              << originalColors::boldAlt << ":cancel)\n";

    std::atomic<size_t> completedBytes(0);
    std::atomic<size_t> completedTasks(0);
    std::atomic<size_t> failedTasks(0);
    std::atomic<bool> isProcessingComplete(false);

    std::thread progressThread(displayProgressBarWithSize, &completedBytes,
        totalBytes, &completedTasks, &failedTasks, totalTasks, &isProcessingComplete, &verbose, operation);

    std::vector<std::future<void>> futures;
    futures.reserve(indexChunks.size());

    for (const auto& chunk : indexChunks) {
        std::vector<std::string> imageFilesInChunk;
        imageFilesInChunk.reserve(chunk.size());
        std::transform(chunk.begin(), chunk.end(), std::back_inserter(imageFilesInChunk),
            [&fileList](size_t index) { return fileList[index - 1]; });

        futures.emplace_back(pool.enqueue([imageFilesInChunk = std::move(imageFilesInChunk),
            &successOuts, &skippedOuts, &failedOuts,
            modeMdf, modeNrg, modeChd, modeDaa,   // <-- pass DAA flag
            &completedBytes, &completedTasks, &failedTasks, &newISOFound]() {
            convertToISO(imageFilesInChunk, successOuts, skippedOuts, failedOuts,
                modeMdf, modeNrg, modeChd, modeDaa,   // <-- added DAA flag
                &completedBytes, &completedTasks, &failedTasks, newISOFound);
        }));
    }

    for (auto& future : futures) {
        future.wait();
    }

    isProcessingComplete.store(true);
    signal(SIGINT, SIG_IGN);
    progressThread.join();
}
