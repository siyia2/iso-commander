// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../caches.h"
#include "../threadpool.h"
#include "../databaseOps.h"
#include "../inputHandling.h"
#include "../main.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"
#include "../select.h"

namespace fs = std::filesystem;

/**
 * @brief Outputs the current program version to the standard output.
 * * Uses ANSI bold escape sequences to highlight the version string.
 * * @param version A string representing the semantic version (e.g., "6.3.5").
 */
void printVersionNumber(const std::string& version) {
    std::cout << UI::Palette::BoldReset << "Iso Commander v" << version << UI::Palette::Reset << "\n";
}


/**
 * @brief Entry point for the isocmd application - an ISO management and mounting utility.
 *
 * Initializes application state, processes command-line arguments, establishes a single-instance
 * lock, configures signal handling, launches background tasks (auto-update/database import),
 * and enters the main interactive menu loop.
 *
 * The application supports the following command-line modes:
 * - `-v` / `--version` : Prints version information and exits.
 * - `mount` / `umount` / `unmount` : Delegates to mount/unmount command handler.
 * - No arguments : Launches the interactive TUI (terminal user interface).
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of null-terminated argument strings.
 * @returns 0 on successful execution, 1 on failure (e.g., lock acquisition failure).
 */
int main(int argc, char *argv[]) {
    /// @name Application State Flags
    /// Atomic booleans controlling concurrency and UI state between main thread and background workers.
    /// @{
    std::atomic<bool> messageActive{false}, isAtMain{true}, isAtISOList{false},
                     stopMessage{false}, monitorThreadSpawned{false};
    /// @}

    // Generous reserve for future lists
    GlobalCaches::globalIsoFileList.reserve(1000);
    GlobalCaches::binImgFilesCache.reserve(1000);
    GlobalCaches::mdfMdsFilesCache.reserve(1000);
    GlobalCaches::nrgFilesCache.reserve(1000);
    GlobalCaches::chdFilesCache.reserve(1000);
    GlobalCaches::daaGbiFilesCache.reserve(1000);

    setupReadlineToIgnoreCtrlC();

    // --- Version & Utility Command Dispatch ---
    if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v"))
        return printVersionNumber("7.2.0"), 0;
    if (argc >= 3 || (argc == 2 && (std::string(argv[1]) == "umount" || std::string(argv[1]) == "unmount" || std::string(argv[1]) == "mount")))
        return handleMountUmountCommands(argc, argv);

    /// Configure readline completion behavior
    rl_completer_word_break_characters = ";";
    rl_completion_display_matches_hook = customListingsFunction;

    // Bind PgUp and PgDn to arrow keys
    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

    /**
     * @brief Single-instance lock mechanism
     * Attempts to acquire an exclusive advisory lock on /tmp/isocmd.lock.
     * If lock fails, displays a user-friendly error message and exits.
     */
    const char* lockFile = "/tmp/isocmd.lock";
    GlobalState::lockFileDescriptor = open(lockFile, O_CREAT | O_RDWR, 0666);
    struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    if (fcntl(GlobalState::lockFileDescriptor, F_SETLK, &fl) == -1) {
        std::cerr << UI::Palette::Red << "error: " << UI::Palette::Yellow
                  << "failed to setup transaction (unable to lock database)\n  "
                  << UI::Palette::BoldReset << "if you're sure isocmd isn't already running, you can remove '/tmp/isocmd.lock'\n"
                  << UI::Palette::Reset;
        close(GlobalState::lockFileDescriptor);
        return 1;
    }

    // Initialize static thread pool early for improved performance
    getStaticThreadPool();

    /// @name Signal Handling
    /// Ignore Ctrl+C (SIGINT) in main loop; gracefully handle SIGTERM.
    /// @{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, signalHandler);
    /// @}

    /**
     * @brief Configuration loading and background task initialization
     * - `search` controls whether auto-update scanning is enabled
     * - `config` contains user-defined folder paths and settings
     */
    bool exitProgram = false;
    std::atomic<bool> search{readUserConfigUpdates(GlobalState::configPath)};

    std::map<std::string, std::string> config = readUserConfigLists(GlobalState::configPath);
    std::vector<std::thread> backgroundThreads;

    // Shared state for background import coordination (isImportRunning, stopImport, worker sync)
    std::shared_ptr<RefreshState> importState;
    importState = std::make_shared<RefreshState>();

    /// Start background database import if auto-update is enabled and FolderPaths exist in history file
    if (search.load() && (!(isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath)))) {
        importState->isImportRunning.store(true);
        backgroundThreads.emplace_back([importState, &search] {
            backgroundDatabaseImport(importState);
            search.store(false);
        });
    }
    paginationSet(GlobalState::configPath);

    /**
     * @brief Main interactive loop
     * Displays ASCII art header, status messages for background tasks,
     * renders the menu, and processes user input (menu options 1-4).
     */
    while (!exitProgram) {
        /// Prevent default readline keybindings from interfering
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        GlobalState::g_operationCancelled.store(false);
        isAtMain.store(true);
        isAtISOList.store(false);

        clearScrollBuffer();
        print_ascii();

        /**
         * @brief Status message display logic
         * Shows appropriate background task status:
         * 1. Auto-update running in background (with monitor thread)
         * 2. No stored folder paths available for scanning (one-time message)
         */
        static bool messagePrinted = false;

        if (search.load() && !isHistoryFileEmpty(GlobalState::historyFilePath) &&
            importState && importState->isImportRunning.load()) {
            std::cout << UI::Palette::Dim << "[Auto-Update: running in the background...]\n" << UI::Palette::Reset;
            messageActive = true;
            if (!monitorThreadSpawned.exchange(true))
                backgroundThreads.emplace_back(monitorAndClearMessage,
                                               importState, std::ref(messageActive),
                                               std::ref(stopMessage), std::ref(isAtMain));
        } else if ((search.load() && !messagePrinted) &&
                   (isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath))) {
            std::cout << UI::Palette::Dim << "[Auto-Update: no stored FolderPaths to scan...]\n" << UI::Palette::Reset;
            messagePrinted = true;
            messageActive = true;
            backgroundThreads.emplace_back(clearMessageAfterTimeoutInMain, 4, std::ref(isAtMain),
                                           importState, std::ref(messageActive),
                                           std::ref(stopMessage));
        }

        printMenu();
        clear_history();

        /// Read user input with theme-aware styling
        const ReadlineAndPromptTheme pt = getPromptTheme();
        char* rawInput = readline(("\n" + std::string(pt.primary) +
                                   "Choose an option: " + std::string(pt.reset)).c_str());
        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);
        if (!input) break; ///< Handle EOF (Ctrl+D)

        /**
         * @brief Menu option dispatch
         * - 1: Browse/select ISOs (submenu1)
         * - 2: Select Image files for ISO conversion (submenu2)
         * - 3: Refresh ISO database from filesystem
         * - 4: Settings Editor
         * - 5: Exit application
         */
        std::string choice(input.get());
        if (choice == "1") {
            isAtMain.store(false);
            isAtISOList.store(false);
            submenu1(isAtISOList, importState, backgroundThreads);
        } else if (choice.length() == 1) {
            switch (choice[0]) {
                case '2':
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    submenu2(importState);
                    break;
                case '3': {
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    bool newISOFound = false;
                    refreshForDatabase(true, -1, false, newISOFound);
                    clearScrollBuffer();
                    break;
                }
                case '4':
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    interactiveConfigEditor(GlobalState::configPath);
                    clearScrollBuffer();
                    break;
                case '5':
                    exitProgram = true;
                    clearScrollBuffer();
                    break;
                default: break;
            }
        }
    }

    //// @name Cleanup and Resource Release
    /// Shut down the thread pool, signal background tasks to stop, and release system locks.
    /// @{
    getStaticThreadPool().shutdown();

    importState->stopImport = stopMessage = true;
    GlobalState::g_operationCancelled.store(true, std::memory_order_release);

    if (importState) {
        importState->isImportRunning.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(importState->workerMutex);
            // Use notify_all() to wake up all background database threads safely
            importState->workerCV.notify_all();
        }
    }

    for (auto& t : backgroundThreads) {
        if (t.joinable()) {
            t.join();
        }
    }

    std::cout << UI::Palette::Reset << std::flush;
    close(GlobalState::lockFileDescriptor);
    unlink(lockFile);
    return 0;
    /// @}
}
