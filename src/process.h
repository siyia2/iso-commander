// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef OPERATIONS_H
#define OPERATIONS_H

// C++ Standard Library Headers
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// --- Core File Operations (Cp, Mv, Rm, Convert) ---

/**
 * Orchestrates high-level ISO file operations (Copy, Move, or Delete).
 * Tracks progress via atomic counters and ensures thread-safe path recording.
 */
void handleIsoFileOperation(
    const std::vector<std::string>& isoFiles,
    const std::vector<std::string>& isoFilesCopy,
    const std::string& userDestDir,
    bool isMove,
    bool isCopy,
    bool isDelete,
    std::atomic<size_t>* completedBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool overwriteExisting,
    std::vector<std::string>* successfulDestPaths,
    std::mutex* destPathsMutex
);

/**
 * Converts various disk image formats (MDF, NRG, CHD, DAA) to standard ISO format.
 */
void convertToISO(
    const std::vector<std::string>& imageFiles,
    const bool& modeMdf,
    const bool& modeNrg,
    const bool& modeChd,
    const bool& modeDaa,
    std::atomic<size_t>* completedBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    std::vector<std::string>* successfulOutputPaths,
    std::mutex* outPathsMutex
);

/**
 * Finalizes the database state after a batch of file operations is completed.
 */
void updateDatabaseAfterOperations(
    const std::string& filePathsStr
);


// --- Progress Monitoring & UI ---

/**
 * Renders a visual progress bar based on byte count and task completion.
 */
void displayProgressBarWithSize(
    std::atomic<size_t>* completedBytes,
    size_t totalBytes,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation
);


// --- Metadata & Calculation Helpers ---

/**
 * Calculates the sum of file sizes for a list of paths.
 */
size_t getTotalFileSize(const std::vector<std::string>& files);

/**
 * Estimates total bytes specifically for conversion tasks based on enabled modes.
 */
size_t calculateTotalBytesForConversions(
    const std::vector<std::string>& filesToProcess,
    bool modeMdf,
    bool modeNrg,
    bool modeChd,
    bool modeDaa
);

/**
 * Prompts user for destination directory and sets up operational flags for Cp/Mv/Rm.
 */
std::string userDestDirCpMv(
    const std::vector<std::string>& isoFiles,
    std::vector<std::vector<int>>& indexChunks,
    std::string& userDestDir,
    std::string& operationColor,
    std::string& operationDescription,
    bool& umountMvRmBreak,
    bool& filterHistory,
    bool& isDelete,
    bool& isCopy,
    bool& abortDel,
    bool& overwriteExisting
);

#endif // OPERATIONS_H
