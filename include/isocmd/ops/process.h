// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PROCESS_H
#define PROCESS_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Input options and operation mode flags for an ISO file operation batch.
 */
struct FileOperationOptions {
    const std::vector<std::string>& isoFiles; ///< Source ISO file paths to operate on.
    std::string userDestDir;                  ///< Raw destination directory string from user input.
    bool isMove = false;                      ///< True when performing a move operation.
    bool isCopy = false;                      ///< True when performing a copy operation.
    bool isDelete = false;                    ///< True when performing a delete operation.
    bool overwriteExisting = false;           ///< True to overwrite existing destination files.
};

/**
 * @brief Shared progress counters and recorded destination paths for an ISO operation.
 */
struct FileOperationProgress {
    std::atomic<size_t>& completedBytes;          ///< Total bytes processed across operations.
    std::atomic<size_t>& completedTasks;          ///< Count of completed tasks.
    std::atomic<size_t>& failedTasks;             ///< Count of failed tasks.
    std::vector<std::string>& successfulDestPaths; ///< Destination paths written successfully.
    std::mutex& destPathsMutex;                   ///< Mutex guarding @c successfulDestPaths.
};

// --- Core File Operations (Cp, Mv, Rm, Convert) ---

/**
- Orchestrates high-level ISO file operations (Copy, Move, or Delete).
- Tracks progress via atomic counters and ensures thread-safe path recording.
 */
void handleIsoFileOperation(const FileOperationOptions& options,
                            const std::vector<int>& chunk,
                            FileOperationProgress& progress);

/**
- Finalizes the database state after a batch of file operations is completed.
 */
void updateDatabaseAfterOperations(
    const std::string& filePathsStr
);


// --- Progress Monitoring & UI ---

/**
- Renders a visual progress bar based on byte count and task completion.
 */
void displayProgressBarWithSize(
    std::atomic<size_t>* completedBytes,
    size_t totalBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation,
    const std::string& header = ""
);


// --- Metadata & Calculation Helpers ---

/**
- Calculates the sum of file sizes for a list of paths.
 */
size_t getTotalFileSize(const std::vector<std::string>& files);

/**
- Estimates total bytes specifically for conversion tasks based on enabled modes.
 */
size_t calculateTotalBytesForConversions(
    const std::vector<std::string>& filesToProcess,
    bool modeMdf,
    bool modeNrg,
    bool modeChd,
    bool modeDaa
);

/**
- Prompts user for destination directory and sets up operational flags for Cp/Mv/Rm.
 */
std::string userDestDirCpMv(CpMvPromptContext& ctx);

#endif // PROCESS_H
