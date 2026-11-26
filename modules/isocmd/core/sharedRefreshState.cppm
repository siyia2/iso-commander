// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.sharedRefreshState;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

struct RefreshState {
	std::vector<std::string> filteredFiles;
	std::vector<std::string> pendingIndices;
	bool isFiltered;
	bool hasPendingProcess;
	bool umountMvRmBreak;
	std::string listSubtype;
	std::size_t currentPage;
	std::size_t originalPage;
	std::mutex importMutex;
	std::condition_variable importCV;
	std::atomic<bool> isImportRunning{false};
	std::atomic<bool> isWatcherRunning{false};
	std::atomic<bool> stopImport{false};
	std::mutex printMutex;
	std::condition_variable workerCV;
	std::mutex workerMutex;
	std::atomic<std::size_t> activeWorkers{0};
};

} // extern "C++"
