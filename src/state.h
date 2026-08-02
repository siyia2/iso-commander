// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GLOBALS_H
#define GLOBALS_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// Project Headers
#include "./sharedRefreshState.h"

/**
 * @brief Identifies which UI surface, if any, has a pending asynchronous refresh.
 *
 * Background threads cannot safely call Readline functions or write to the
 * terminal directly (Readline is not thread-safe). Instead, they publish a
 * @c PendingRefreshKind via @c GlobalState::g_pendingRefreshKind to signal
 * that a redraw is needed. The main thread's Readline event hook
 * (@c checkPendingRefresh, installed via @c rl_event_hook) polls this value
 * and performs the actual redraw itself, ensuring all Readline API calls
 * originate from the single thread that owns Readline's internal state.
 */
enum class PendingRefreshKind {
    None,     ///< No refresh pending; the hook has nothing to do.
    MainMenu, ///< Re-render the ASCII banner and main menu (e.g. after an
              ///< auto-cleared status message).
    IsoList   ///< Re-render the ISO list view via @c loadAndDisplayIso,
              ///< using the state referenced by @c GlobalState::g_pendingRefreshState.
};

//==============================
// GLOBAL APPLICATION STATE
//==============================
namespace GlobalState {

    // File Paths
    inline const std::string databaseDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/share/isocmd/database/";
    inline const std::string databaseFilename  = "iso_commander_database.txt";
    inline const std::string databaseFilePath  = databaseDirectory + databaseFilename;
    inline const std::string historyFilePath   = databaseDirectory + "iso_commander_path_database.txt";
    inline const std::string filterHistoryFilePath = databaseDirectory + "iso_commander_filter_database.txt";

    inline const std::string configDirectory = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/isocmd/";
    inline const std::string configPath = configDirectory + "config";

    // Configuration Limits
    inline int MAX_HISTORY_LINES         = 100;
    inline int MAX_HISTORY_PATTERN_LINES = 50;

    inline constexpr uintmax_t maxDatabaseSize = 1024 * 1024 * 1; // 1MB

    // State Management
    inline std::atomic<bool> isoListDirty{true};
    inline std::atomic<bool> g_operationCancelled{false};
    inline std::atomic<bool> g_pendingMenuRefresh{false};
    inline std::atomic<PendingRefreshKind> g_pendingRefreshKind{PendingRefreshKind::None};
    inline std::shared_ptr<RefreshState> g_pendingRefreshState{nullptr};
    inline bool needSortingAfterflno      = false;
    inline size_t ITEMS_PER_PAGE          = 25;
    inline int lockFileDescriptor         = -1;


    // Global vector state
    inline std::vector<std::string> globalIsoFileList;
    inline std::vector<std::string> binImgFilesCache;
    inline std::vector<std::string> mdfMdsFilesCache;
    inline std::vector<std::string> nrgFilesCache;
    inline std::vector<std::string> chdFilesCache;
    inline std::vector<std::string> daaGbiFilesCache;

} // namespace GlobalState

#endif // GLOBALS_H
