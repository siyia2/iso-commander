// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.proc.process;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

struct CpMvPromptContext;

/// Input options and operation mode flags for an ISO file operation batch.
struct FileOperationOptions {
	const std::vector<std::string>& isoFiles; ///< Source ISO file paths to operate on.
	std::string userDestDir;                  ///< Raw destination directory string from user input.
	bool isMove = false;                      ///< True when performing a move operation.
	bool isCopy = false;                      ///< True when performing a copy operation.
	bool isDelete = false;                    ///< True when performing a delete operation.
	bool overwriteExisting = false;           ///< True to overwrite existing destination files.
};

/// Shared progress counters and recorded destination paths for an ISO operation.
struct FileOperationProgress {
	std::atomic<std::size_t>& completedBytes;          ///< Total bytes processed across operations.
	std::atomic<std::size_t>& completedTasks;          ///< Count of completed tasks.
	std::atomic<std::size_t>& failedTasks;             ///< Count of failed tasks.
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
    std::atomic<std::size_t>* completedBytes,
    std::size_t totalBytes,
    std::atomic<std::size_t>* completedTasks,
    std::atomic<std::size_t>* failedTasks,
    std::size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation,
    const std::string& header = ""
);

/**
 * @brief Starts the progress renderer on a dedicated thread.
 *
 * Copies the operation and header strings into the thread's argument storage,
 * just like constructing std::thread directly. Counter and verbose pointers
 * retain their existing lifetimes; the caller must join the returned thread.
 */
std::thread launchProgressBar(
    std::atomic<std::size_t>* completedBytes,
    std::size_t totalBytes,
    std::atomic<std::size_t>* completedTasks,
    std::atomic<std::size_t>* failedTasks,
    std::size_t totalTasks,
    std::atomic<bool>* isComplete,
    bool* verbose,
    const std::string& operation,
    const std::string& header
);

// --- Metadata & Calculation Helpers ---

/**
- Estimates total bytes specifically for conversion tasks based on enabled modes.
 */
std::size_t calculateTotalBytesForConversions(
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

} // extern "C++"
