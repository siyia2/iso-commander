// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONFIG_UI_OPS_H
#define CONFIG_UI_OPS_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <vector>

/**
 * CONFIGURATION & PERSISTENCE
 * Logic for reading, parsing, and validating user preferences and history.
 */

// Applies configuration key-pairs to the running state
bool readUserConfigUpdates(const std::string& filePath);

// Returns a map of all valid config entries
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);

// Specific check for UI pagination status
bool paginationSet(const std::string& filePath);

// Validation for history state
bool isHistoryFileEmpty(const std::string& filePath);


/**
 * USER INTERFACE & NAVIGATION
 * Functions responsible for rendering the visual state and managing submenus.
 */

// Visual decorative elements
void print_ascii();
void printMenu();

// Primary navigation logic for the ISO list and scanning
void submenu1(
    std::atomic<bool>& updateHasRun,
    std::atomic<bool>& isAtISOList,
    std::shared_ptr<RefreshState> state,
    std::atomic<bool>& newISOFound,
    std::vector<std::thread>& backgroundThreads,
    bool& search
);

// Secondary navigation logic for settings and global status
void submenu2(
    std::atomic<bool>& newISOFound,
    std::shared_ptr<RefreshState> state
);


/**
 * ASYNCHRONOUS UI MONITORING
 * Threaded workers that manage temporary UI states like status messages.
 */

// Clears the status bar after a timeout if the user is in the Main view
void clearMessageAfterTimeoutInMain(
    int timeoutSeconds,
    std::atomic<bool>& isAtMain,
    std::shared_ptr<RefreshState> state,
    std::atomic<bool>& messageActive,
    std::atomic<bool>& stopMessage
);

// Dedicated monitor thread to react to message clear signals
void monitorAndClearMessage(
    std::shared_ptr<RefreshState> state,
    std::atomic<bool>& messageActive,
    std::atomic<bool>& stopSignal,
    std::atomic<bool>& isAtMain
);


/**
 * EXTERNAL COMMAND INTERFACE
 * Entry point for non-interactive CLI usage.
 */

int handleMountUmountCommands(int argc, char* argv[]);

#endif // CONFIG_UI_OPS_H
