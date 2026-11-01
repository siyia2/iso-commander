// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <unistd.h>

// Third-Party Library Headers
#include <readline/history.h>
#include <readline/readline.h>

// Project Headers
#include "../threadpool.h"
#include "../databaseOps.h"
#include "../display.h"
#include "../inputHandling.h"
#include "../rendering.h"
#include "../main.h"
#include "../readline.h"
#include "../state.h"
#include "../themes.h"
#include "../settings.h"
#include "../sharedRefreshState.h"
#include "../write2usbUI.h"

namespace fs = std::filesystem;

/**
 * @brief Prints the program version to stdout, bolded.
 *
 * @param version The semantic version string (e.g. "6.3.5").
 */
void printVersionNumber(const std::string& version) {
    std::cout << UI::Palette::BoldReset << "Iso Commander v" << version << UI::Palette::Reset << "\n";
}

/**
 * @brief Entry point for isocmd, an ISO management and mounting utility.
 *
 * Reads the user config, dispatches command-line modes, acquires a
 * single-instance lock, configures signal handling, launches background
 * tasks (auto-update / database import), and enters the interactive menu
 * loop.
 *
 * Command-line modes:
 * - `-v` / `--version` : print version and exit.
 * - `mount` / `umount` / `unmount` : delegate to the mount/unmount handler.
 * - `c2iso` : non-interactive image -> ISO conversion
 *   (e.g. `isocmd c2iso -d2 ~/Games`).
 * - `w2usb` : non-interactive ISO-to-USB writer
 *   (e.g. `isocmd w2usb file.iso /dev/sdd`).
 * - No arguments : interactive TUI.
 *
 * The main menu prompt is wrapped in a @c ListResizeWatcher (via
 * @c readlineWithResize), so the menu (banner, status message, menu box) is
 * repainted when the terminal width changes while the prompt waits.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of null-terminated argument strings.
 * @return 0 on success, 1 on failure (e.g. lock acquisition failure).
 */
int main(int argc, char *argv[]) {

    // User-defined folder paths and settings.
    std::map<std::string, std::string> config = readUserConfigLists(GlobalState::configPath);

    // --- Version & Utility Command Dispatch ---
    const std::string arg1 = (argc >= 2) ? argv[1] : "";

    if (argc == 2 && (arg1 == "--version" || arg1 == "-v"))
        return printVersionNumber("7.9.3"), 0;
    if (argc >= 3 || (argc == 2 && (arg1 == "umount" || arg1 == "unmount" ||
                                    arg1 == "mount"  || arg1 == "c2iso" ||
                                    arg1 == "w2usb")))
        return handleMountUmountConvertWriteCommands(argc, argv);

    /// @name Application State Flags
    /// Atomic booleans coordinating concurrency and UI state between the main
    /// thread and background workers.
    /// @{
    std::atomic<bool> messageActive{false}, stopMessage{false}, monitorThreadSpawned{false};
    std::atomic<bool>& isAtMain    = GlobalState::isAtMain;
    std::atomic<bool>& isAtISOList = GlobalState::isAtISOList;
    /// @}

    GlobalState::isNonInteractive.store(false);

    // Generous reserves for future lists.
    GlobalState::globalIsoFileList.reserve(1000);
    GlobalState::binImgFilesCache.reserve(1000);
    GlobalState::mdfMdsFilesCache.reserve(1000);
    GlobalState::nrgFilesCache.reserve(1000);
    GlobalState::chdFilesCache.reserve(1000);
    GlobalState::daaGbiFilesCache.reserve(1000);

    setupReadlineToIgnoreCtrlC();

    // Readline completion behavior.
    rl_completer_word_break_characters = ";";
    rl_completion_display_matches_hook = customListingsFunction;

    // Esc as universal exit handler; PgUp/PgDn as history navigation.
    rl_variable_bind("keyseq-timeout", "10");
    rl_bind_keyseq("\\e", exit_handler);
    rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
    rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

    /**
     * @brief Single-instance lock.
     *
     * Acquires an exclusive advisory lock on /tmp/isocmd.lock; on failure,
     * prints a hint about the stale lock file and exits.
     */
    const PrintListTheme c = getListColors();

    const char* lockFile = "/tmp/isocmd.lock";
    GlobalState::lockFileDescriptor = open(lockFile, O_CREAT | O_RDWR, 0666);
    struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    if (fcntl(GlobalState::lockFileDescriptor, F_SETLK, &fl) == -1) {
        std::cerr << c.indexA << "error: " << c.num
                  << "failed to setup transaction (unable to lock database)\n  "
                  << UI::Palette::BoldReset << "if you're sure isocmd isn't already running, you can remove '/tmp/isocmd.lock'\n"
                  << UI::Palette::Reset;
        close(GlobalState::lockFileDescriptor);
        return 1;
    }

    /// @name Signal Handling
    /// Ignore Ctrl+C (SIGINT) in the main loop; handle SIGTERM gracefully.
    /// @{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, signalHandler);
    /// @}

    bool exitProgram = false;
    std::atomic<bool> search{readUserConfigUpdates(GlobalState::configPath)};

    std::vector<std::thread> backgroundThreads;

    // Shared state for background import coordination (isImportRunning,
    // stopImport, worker sync).
    std::shared_ptr<RefreshState> importState;
    importState = std::make_shared<RefreshState>();

    // Start background database import if auto-update is enabled and the
    // history file has stored FolderPaths.
    if (search.load() && (!(isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath)))) {
        importState->isImportRunning.store(true);
        backgroundThreads.emplace_back([importState, &search] {
            backgroundDatabaseImport(importState);
            search.store(false);
        });
    }
    paginationSet(GlobalState::configPath);
    liveFilteringSet(GlobalState::configPath);

    // Readline refresh hook for pending UI refreshes.
    rl_event_hook = checkPendingRefresh;

    /**
     * @brief Main interactive loop.
     *
     * Displays the ASCII banner, background-task status, and menu, then
     * processes user input (menu options 1-5).
     */
    while (!exitProgram) {
        // Prevent default readline keybindings from interfering.
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        GlobalState::g_operationCancelled.store(false);
        GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::None);
        isAtMain.store(true);
        isAtISOList.store(false);

        /**
         * @brief Status-line logic (runs once per iteration, not on resize
         * repaints).
         *
         * Picks the background-task status to show and starts the helper
         * threads that later clear it:
         * 1. Auto-update running in the background (with a monitor thread).
         * 2. No stored folder paths available for scanning (one-time message).
         * The text is kept in @c statusLine; drawMain() only prints it.
         */
        static bool messagePrinted = false;
        std::string statusLine;

        if (search.load() && !isHistoryFileEmpty(GlobalState::historyFilePath) &&
            importState && importState->isImportRunning.load()) {
            statusLine = std::string(UI::Palette::Dim) +
                         "[Auto-Update: running in the background...]\n" +
                         std::string(UI::Palette::Reset);
            messageActive = true;
            if (!monitorThreadSpawned.exchange(true))
                backgroundThreads.emplace_back(monitorAndClearMessage,
                                               importState, std::ref(messageActive),
                                               std::ref(stopMessage), std::ref(isAtMain));
        } else if ((search.load() && !messagePrinted) &&
                   (isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath))) {
            statusLine = std::string(UI::Palette::Dim) +
                         "[Auto-Update: no stored FolderPaths to scan...]\n" +
                         std::string(UI::Palette::Reset);
            messagePrinted = true;
            messageActive = true;
            backgroundThreads.emplace_back(clearMessageAfterTimeoutInMain, 4, std::ref(isAtMain),
                                           importState, std::ref(messageActive),
                                           std::ref(stopMessage));
        }

        // Draws the main menu screen: clear, banner, status line (while the
        // message is active), menu. Ends just after the menu's last line; the
        // blank line before the prompt comes from the caller / resize watcher.
        // Used for both the normal draw and the resize repaint; runs on the
        // Readline thread and must not call readline().
        auto drawMain = [&] {
            SyncOutput sync;
            clearScrollBuffer();
            print_ascii();
            if (!statusLine.empty() && messageActive.load())
                std::cout << statusLine;
            printMenu();
        };

        drawMain();
        std::cout << "\n";   // blank line before the prompt

        clear_history();

        const ReadlineAndPromptTheme pt = getPromptTheme();
        const std::string prompt = std::string(pt.primary) +
                                   "Enter choice [1-5]: " +
                                   std::string(pt.reset);
        RlLine input = readlineWithResize(prompt, drawMain);
        if (!input) break; ///< EOF (Ctrl+D)

        /**
         * @brief Menu dispatch.
         * - 1: Browse/select ISOs (submenu1)
         * - 2: Select image files for ISO conversion (submenu2)
         * - 3: Refresh ISO database from filesystem
         * - 4: Settings editor
         * - 5: Exit
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
                    break;
                }
                case '4':
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    interactiveConfigEditor(GlobalState::configPath);
                    break;
                case '5':
                    exitProgram = true;
                    clearScrollBuffer();
                    break;
                default: break;
            }
        }
    }

    /// @name Cleanup and Resource Release
    /// Shut down the thread pool, signal background tasks to stop, and release
    /// system locks.
    /// @{
    getStaticThreadPool().shutdown();

    importState->stopImport = stopMessage = true;
    GlobalState::g_operationCancelled.store(true, std::memory_order_release);

    if (importState) {
        importState->isImportRunning.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(importState->workerMutex);
            // Wake all background database threads.
            importState->workerCV.notify_all();
        }
    }

    for (auto& t : backgroundThreads) {
        if (t.joinable()) {
            t.join();
        }
    }

    g_drainingManager.joinAll();

    std::cout << UI::Palette::Reset << std::flush;
    close(GlobalState::lockFileDescriptor);
    unlink(lockFile);
    return 0;
    /// @}
}
