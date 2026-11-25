// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <sys/types.h>

export module isocmd.ops.cpMvRm;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- ops/cpMvRm ----

// C / System Headers

// Third-Party Library Headers

// Project Headers

namespace fs = std::filesystem;

/**
 * @struct CpMvPromptContext
 * @brief Holds inputs, prompt configuration, and mutable loop state for copy,
 *        move, and remove prompts.
 */
struct CpMvPromptContext {
	// Inputs
	const std::vector<std::string>& isoFiles;        ///< Candidate ISO file paths or names
	const std::vector<std::vector<int>>& indexChunks; ///< Index chunks used to build the displayed entries

	// Prompt appearance & operation flags
	std::string operationColor;                       ///< ANSI color code for the active operation
	std::string operationDescription;                 ///< Human-readable operation label for prompts
	bool isDelete = false;                            ///< True when performing a delete/remove operation
	bool isCopy = false;                              ///< True when performing a copy operation

	// Mutable state & outputs updated during the prompt loop
	std::string userDestDir;                          ///< Destination directory entered by the user
	bool umountMvRmBreak = false;                     ///< Set when an unmount condition aborts the prompt
	bool filterHistory = false;                       ///< Enables history filtering for destination input
	bool abortDel = false;                            ///< Set when the user aborts a delete prompt
	bool overwriteExisting = false;                   ///< Allows overwriting existing destination files
};

/**
 * @enum TransferOp
 * @brief Kind of file transfer performed by the copy/move pipeline.
 *
 * Replaces the pair of @c isMove / @c isCopy booleans in the internal transfer
 * structs, so the invalid "both true" / "both false" states cannot be built.
 */
enum class TransferOp { Copy, Move };

/**
 * @struct TransferPaths
 * @brief Source/destination paths plus the display strings used in messages.
 *
 * Shared by the destination check, the move operation, and the buffered copy
 * operation so the same six fields are not redeclared in each of them.
 */
struct TransferPaths {
	const fs::path& srcPath;                          ///< Full source filesystem path
	const fs::path& destPath;                         ///< Full destination filesystem path
	std::string_view srcDir;                          ///< Source directory for display/logging
	std::string_view srcFile;                         ///< Source filename for display/logging
	std::string_view destDirProcessed;                ///< Processed destination directory for display/logging
	std::string_view destFile;                        ///< Destination filename for display/logging
};

/**
 * @struct MultiDestState
 * @brief Per-source-file tracking state for multi-destination copy/move.
 *
 * Updated while the destinations of one source file are processed and read
 * afterwards to decide whether the source may be removed after a move.
 */
struct MultiDestState {
	int validDestinations = 0;                      ///< Incremented for each non-same-file destination
	bool atLeastOneCopySucceeded = false;             ///< Set when at least one multi-dest move copy succeeds
};

/**
 * @struct DestinationCheckParams
 * @brief Inputs for validating and dispatching one copy/move destination.
 */
struct DestinationCheckParams {
	TransferPaths paths;                              ///< Source/destination paths and display strings
	TransferOp op = TransferOp::Copy;                 ///< Copy or move
	bool overwriteExisting = false;                   ///< True if existing destinations may be overwritten
	std::size_t fileSize = 0;                              ///< Source file size in bytes
	std::size_t destDirCount = 0;                          ///< Number of destination directories targeted
	MultiDestState& state;                            ///< Mutable tracking state shared across a file's destinations
	std::vector<std::string>* successfulDestPaths = nullptr; ///< Optional: collects destination paths of successful operations
	std::mutex* destPathsMutex = nullptr;             ///< Optional: protects @c successfulDestPaths
};

/**
 * @struct RlFormFeedGuard
 * @brief RAII guard to restore the '\f' key binding to @c prevent_readline_keybindings.
 *
 * Use in functions that bind '\f' but do not modify '\t' to ensure scope integrity.
 */
struct RlFormFeedGuard {
	~RlFormFeedGuard();
};

/**
 * @struct RlKeyBindGuard
 * @brief RAII guard that restores the '\f' and '\t' bindings and rebinds Esc to
 *        @c exit_handler, undoing the Esc hijack left by a completion list.
 *
 * Fires only when the guarded call returns, not on in-loop page turns.
 */
struct RlKeyBindGuard {
	~RlKeyBindGuard();
};

/**
 * @struct RlCompleteModeGuard
 * @brief RAII guard to preserve the state of @c g_rl_complete_mode.
 */
struct RlCompleteModeGuard {
	int saved; ///< Saved value of @c RetainAndRestoreReadlineBuffer::g_rl_complete_mode
	RlCompleteModeGuard();
	~RlCompleteModeGuard();
};

/**
 * @struct RlStartupHookGuard
 * @brief RAII guard to clear @c rl_startup_hook on scope exit.
 *
 * Prevents stale hooks from persisting into subsequent Readline calls.
 */
struct RlStartupHookGuard {
	~RlStartupHookGuard();
};

// ======================================================================
// Helper classes & functions
// ======================================================================

/**
 * @brief Thread-local batched reporter for success/error messages.
 *
 * Each worker thread creates its own instance. Messages are first buffered
 * locally; when the buffer reaches a threshold they are flushed under the
 * global mutex to the shared verboseSets containers. This reduces mutex
 * contention compared to locking on every message.
 *
 * @note Must call flush() when the thread finishes to ensure no messages
 *       are left in the local buffers.
 */
class BatchReporter {
public:
	/**
	 * @brief Constructs a batched reporter tied to global sets.
	 * @param globalIsos   Reference to the global completed operations set.
	 * @param globalErrors Reference to the global failed operations set.
	 * @param mutex        Mutex protecting both global sets.
	 * @param batchSize    Number of messages to accumulate before auto-flushing.
	 */
	BatchReporter(std::unordered_set<std::string>& globalIsos,
	              std::unordered_set<std::string>& globalErrors,
	              std::mutex& mutex,
	              std::size_t batchSize = 50);

	/**
	 * @brief Buffers a success message and flushes if the batch size is reached.
	 * @param msg The formatted success message to add.
	 */
	void addSuccess(std::string msg);

	/**
	 * @brief Buffers an error message and flushes if the batch size is reached.
	 * @param msg The formatted error message to add.
	 */
	void addError(std::string msg);

	/**
	 * @brief Immediately flushes all buffered messages to the global sets.
	 * @details Must be called when the worker thread finishes processing
	 *          to ensure no messages remain in local buffers.
	 */
	void flush();

private:
	/**
	 * @brief Checks if either local buffer has reached the batch threshold
	 *        and flushes if so.
	 */
	void tryFlush();

	std::vector<std::string> localIsos_;            ///< Thread-local success buffer
	std::vector<std::string> localErrors_;          ///< Thread-local error buffer
	std::unordered_set<std::string>& globalIsos_;   ///< Global completed set
	std::unordered_set<std::string>& globalErrors_; ///< Global failed set
	std::mutex& mutex_;                             ///< Protects global sets
	const std::size_t batchSize_;                        ///< Flush threshold
};

/**
 * @brief Bundles all shared state that flows through every file operation.
 *
 * Groups counters, batch reporting, the operation success flag, and an optional
 * ownership-change callback into a single object. This eliminates the need to
 * pass 8-12 separate parameters to every operation function.
 *
 * Thread-local; one instance per worker thread.
 */
struct OperationContext {
	BatchReporter& reporter;                               ///< Batched message output
	std::atomic<std::size_t>* completedTasks = nullptr;         ///< Successful operation counter
	std::atomic<std::size_t>* failedTasks    = nullptr;         ///< Failed operation counter
	std::atomic<bool>&   operationSuccessful;              ///< Set to false on any failure
	/// Optional callback to chown newly created files to the real user.
	std::function<void(const std::filesystem::path&)> changeOwnership;
	/// Bytes processed counter (copy/move/delete progress). Optional.
	std::atomic<std::size_t>* completedBytes = nullptr;
};

/**
 * @brief Builds the display source path respecting the user's display preference.
 *
 * When displayConfig::toggleNamesOnly is false, returns "srcDir/srcFile";
 * when true, returns only "srcFile". This avoids duplicating the conditional
 * logic throughout the codebase.
 *
 * @param srcDir  Source directory path.
 * @param srcFile Source filename.
 * @return The formatted display string.
 */
std::string buildDisplaySrc(std::string_view srcDir, std::string_view srcFile);

/**
 * @brief Builds formatted display entries for the selected ISO files.
 *
 * Shared by the copy/move destination prompt and the remove confirmation.
 */
std::vector<std::string> generateIsoEntries(
    const std::vector<std::vector<int>>& indexChunks,
    const std::vector<std::string>& isoFiles
);

/**
 * @brief Resolves the real (invoking) user and group IDs and names.
 */
void getRealUserId(
    uid_t& real_uid,
    gid_t& real_gid,
    std::string& real_username,
    std::string& real_groupname
);

/**
 * @brief Copies a file to a temporary destination and renames it on success.
 */
bool bufferedCopyWithProgress(
    const fs::path& src,
    const fs::path& dst,
    std::atomic<std::size_t>* completedBytes,
    std::error_code& ec
);

/**
 * @brief Logs the result of a copy or move and updates operation counters.
 */
void logOperationResult(
    OperationContext& ctx,
    bool success,
    bool cancelled,
    const std::error_code& ec,
    std::string_view verb,
    std::string_view srcDir,
    std::string_view srcFile,
    std::string_view destDirProcessed,
    std::string_view destFile
);

/**
 * @brief Performs a buffered copy to one destination and reports its result.
 */
bool performBufferedCopyOperation(
    OperationContext& ctx,
    const TransferPaths& paths,
    std::string_view verb
);

/**
 * @brief Performs a move to one destination, using copy/delete across devices.
 */
bool performMoveOperation(
    OperationContext& ctx,
    const TransferPaths& paths,
    std::size_t fileSize
);

/**
 * @brief Handles the interactive confirmation for permanently deleting files.
 */
bool handleDeleteOperation(
    const std::vector<std::string>& isoFiles,
    const std::vector<std::vector<int>>& indexChunks,
    bool& umountMvRmBreak,
    bool& abortDel
);

/**
 * @brief Performs a single-file deletion and reports its result.
 */
void performDeleteOperation(
    OperationContext& ctx,
    const fs::path& srcPath,
    std::string_view srcDir,
    std::string_view srcFile,
    std::size_t fileSize
);

} // extern "C++"
