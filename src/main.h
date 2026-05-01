// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONFIG_UI_OPS_H
#define CONFIG_UI_OPS_H

// C++ Standard Library Headers
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <unistd.h>

// --- Configuration & File State ---

/**
 * Reads and applies general user configuration updates from a file.
 */
bool readUserConfigUpdates(const std::string& filePath);

/**
 * Retrieves a key-value map of configuration settings.
 */
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath);

/**
 * Checks if pagination is enabled in the config file.
 */
bool paginationSet(const std::string& filePath);

/**
 * Determines if the history file is currently empty.
 */
bool isHistoryFileEmpty(const std::string& filePath);


// --- Main Menu & UI Display ---

/**
 * Prints the decorative ASCII art banner.
 */
void print_ascii();

/**
 * Displays the primary application menu options.
 */
void printMenu();

/**
 * Handles the logic and state transitions for the first submenu.
 */
void submenu1(
    std::atomic<bool>& updateHasRun, 
    std::atomic<bool>& isAtISOList, 
    std::atomic<bool>& isImportRunning, 
    std::atomic<bool>& newISOFound,
    std::atomic<bool>& stopImport, 
    std::vector<std::thread>& backgroundThreads, 
    bool& search
);

/**
 * Handles logic for the second submenu (typically settings or status).
 */
void submenu2(
    std::atomic<bool>& newISOFound, 
    std::atomic<bool>& isImportRunning
);


// --- Messaging & System Commands ---

/**
 * Logic for managing Mount/Unmount lifecycle via CLI arguments.
 */
int handleMountUmountCommands(int argc, char* argv[]);

/**
 * Clears status messages from the UI after a specific duration, 
 * provided the user is at the main screen and no imports are blocking.
 */
void clearMessageAfterTimeoutInMain(
    int timeoutSeconds, 
    std::atomic<bool>& isAtMain, 
    std::atomic<bool>& isImportRunning, 
    std::atomic<bool>& messageActive, 
    std::atomic<bool>& stopMessage
);

/**
 * A background-compatible monitor to clear UI messages based on signals.
 */
void monitorAndClearMessage(
    std::atomic<bool>& isRunning, 
    std::atomic<bool>& messageActive, 
    std::atomic<bool>& stopSignal, 
    std::atomic<bool>& isAtMain
);

#endif // CONFIG_UI_OPS_H
