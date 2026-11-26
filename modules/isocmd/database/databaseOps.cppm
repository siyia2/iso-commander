// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.database.databaseOps;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

// Forward Declarations
struct RefreshState;

/**
 * @brief Checks whether a path exists and refers to a directory.
 *
 * @param path Path to check.
 * @return true if the path is an existing directory, false otherwise.
 */
bool isValidDirectory(const std::string& path);

// --- Filesystem Traversal ---

/**
 * @brief Recursively traverses a directory and collects ISO file paths.
 *
 * Uses the supplied mutexes to protect shared file and error collections.
 *
 * @param path                  Directory path to traverse.
 * @param isoFiles              Output collection of discovered ISO file paths.
 * @param uniqueErrorMessages   Collection of unique traversal error messages.
 * @param totalFiles            Atomic counter tracking discovered files.
 * @param traverseFilesMutex    Mutex protecting updates to @p isoFiles.
 * @param traverseErrorsMutex   Mutex protecting updates to @p uniqueErrorMessages.
 * @param maxDepth              Maximum traversal depth; -1 means unlimited.
 * @param promptFlag            Whether interactive prompts are enabled.
 */
void traverse(
	const std::filesystem::path& path,
	std::vector<std::string>& isoFiles,
	std::unordered_set<std::string>& uniqueErrorMessages,
	std::atomic<std::size_t>& totalFiles,
	std::mutex& traverseFilesMutex,
	std::mutex& traverseErrorsMutex,
	int maxDepth,
	bool promptFlag
);

// --- Database Synchronization ---

/**
 * @brief Loads ISO paths from the database into an output list.
 *
 * @param globalIsoFileList Vector to receive the loaded ISO paths.
 */
void loadFromDatabase(std::vector<std::string>& globalIsoFileList);

/**
 * @brief Adds discovered ISO paths to the persistent database.
 *
 * Existing entries are preserved and duplicate paths are ignored.
 *
 * @param globalIsoFileList Paths to add to the database.
 * @param newISOFound Optional output flag set to true if new paths were saved.
 * @return true if new entries were successfully saved; false otherwise.
 */
bool saveToDatabase(
	const std::vector<std::string>& globalIsoFileList,
	bool* newISOFound
);

/**
 * @brief Refreshes the ISO database using the configured scan and history options.
 *
 * @param promptFlag    Whether interactive prompts are enabled.
 * @param maxDepth      Maximum traversal depth; -1 means unlimited.
 * @param filterHistory Whether history paths should be filtered before scanning.
 * @param newISOFound   Output flag indicating whether new ISO entries were found.
 */
void refreshForDatabase(
	bool promptFlag,
	int maxDepth,
	bool filterHistory,
	bool& newISOFound
);

/**
 * @brief Removes database entries whose paths no longer exist on disk.
 *
 * Updates the supplied in-memory ISO list when the database is successfully
 * rewritten after removing missing paths.
 *
 * @param globalIsoFileList In-memory ISO path list to synchronize.
 */
void removeNonExistentPathsFromDatabase(
	std::vector<std::string>& globalIsoFileList
);

// --- Background Import ---

/**
 * @brief Imports ISO files from directory paths recorded in the history file.
 *
 * Runs the import asynchronously, traverses eligible directories, and saves
 * discovered ISO paths to the database without blocking the UI thread.
 *
 * @param state Shared refresh state containing cancellation and synchronization
 *              information.
 */
void backgroundDatabaseImport(std::shared_ptr<RefreshState> state);

} // extern "C++"
