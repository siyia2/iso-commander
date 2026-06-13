// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SHAREDREFRESHSTATE_H
#define SHAREDREFRESHSTATE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

struct RefreshState {
    std::vector<std::string> filteredFiles;
    std::vector<std::string> pendingIndices;
    bool isFiltered;
    bool hasPendingProcess;
    bool umountMvRmBreak;
    std::string listSubtype;
    size_t currentPage;
    size_t originalPage;
    std::mutex importMutex;
    std::condition_variable importCV;
    std::atomic<bool> isImportRunning{false};
    std::atomic<bool> isWatcherRunning{false};
    std::atomic<bool> stopImport{false};
    std::mutex printMutex;
    std::condition_variable workerCV;
    std::mutex workerMutex;
    std::atomic<size_t> activeWorkers{0};
};

#endif // SHAREDREFRESHSTATE_H
