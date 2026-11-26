// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.paths;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

namespace GlobalState {

	// File Paths
	extern const std::string databaseDirectory;
	extern const std::string databaseFilename;
	extern const std::string databaseFilePath;
	extern const std::string historyFilePath;
	extern const std::string filterHistoryFilePath;

	extern const std::string configDirectory;
	extern const std::string configPath;

	// Configuration Limits
	inline int MAX_HISTORY_LINES         = 30;
	inline int MAX_HISTORY_PATTERN_LINES = 15;

	inline constexpr std::uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

} // namespace GlobalState

} // extern "C++"
