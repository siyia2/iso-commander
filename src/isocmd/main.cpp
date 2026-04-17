// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"
#include "../themes.h"

/**
 * @brief Outputs the current program version to the standard output.
 * * Uses ANSI bold escape sequences to highlight the version string.
 * * @param version A string representing the semantic version (e.g., "6.3.5").
 */
void printVersionNumber(const std::string& version) {    
    std::cout << originalColors::boldAlt << "Iso Commander v" << version << originalColors::resetPlain << "\n";
}

/**
 * @brief Entry point for Iso Commander.
 * * This function initializes the application state, manages a single-instance lock 
 * via file descriptors, handles command-line arguments, and starts the background 
 * ISO discovery thread if auto-update is enabled. It contains the primary 
 * execution loop for the main menu.
 * * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Returns 0 on successful exit, 1 if another instance is already running.
 */
int main(int argc, char *argv[]) {

    // --- State Initialization ---
    std::atomic<bool> isImportRunning{false};
    std::atomic<bool> messageActive{false};
    std::atomic<bool> isAtMain{true};
    std::atomic<bool> isAtISOList{false};
    std::atomic<bool> updateHasRun{false};
    std::atomic<bool> newISOFound{false};
    
    globalIsoFileList.reserve(100);
    setupReadlineToIgnoreCtrlC();

    // --- Command Line Argument Handling ---
    if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        printVersionNumber("6.4.9");
        return 0;
    }
    
    if (argc >= 3 || (argc == 2 && (std::string(argv[1]) == "umount" || std::string(argv[1]) == "unmount" || std::string(argv[1]) == "mount"))) {
        return handleMountUmountCommands(argc, argv);
    }
    
    // --- Readline Configuration ---
    rl_completer_word_break_characters = ";";
    rl_completion_display_matches_hook = customListingsFunction;

    // --- Single Instance Lock Mechanism ---
    const char* lockFile = "/tmp/isocmd.lock";
    lockFileDescriptor = open(lockFile, O_CREAT | O_RDWR, 0666);

    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(lockFileDescriptor, F_SETLK, &fl) == -1) {
        std::cerr << originalColors::red << "error: " 
          << originalColors::yellow << "failed to setup transaction (unable to lock database)\n"
          << "  " << originalColors::boldAlt << "if you're sure isocmd isn't already running, you can remove '/tmp/isocmd.lock'\n" 
          << originalColors::resetPlain << std::endl;
        close(lockFileDescriptor);
        return 1;
    }

    // --- Signal Management ---
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C in the main loop
    signal(SIGTERM, signalHandler); // Handle graceful termination

    // --- Configuration and Background Tasks ---
    bool exitProgram = false;
    bool search = false;      
    
    std::map<std::string, std::string> config = readUserConfigLists(configPath);
    search = readUserConfigUpdates(configPath); 
    
    if (search) {
        isImportRunning.store(true);
        std::thread([&isImportRunning, &newISOFound]() {
            backgroundDatabaseImport(isImportRunning, newISOFound);
        }).detach();
        updateHasRun.store(true);
    }
    
    paginationSet(configPath);
    
    // --- Main Execution Loop ---
    while (!exitProgram) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
        
        g_operationCancelled.store(false);
        isAtMain.store(true);
        isAtISOList.store(false);

        clearScrollBuffer();
        print_ascii();
        
        // --- Status Message Handling ---
        static bool messagePrinted = false;
        if (search && !isHistoryFileEmpty(historyFilePath) && isImportRunning.load()) {
            std::cout << originalColors::dim << "[Auto-Update: running in the background...]\n" << originalColors::resetPlain;
            messageActive.store(true);
            std::thread(clearMessageAfterTimeout, 1, std::ref(isAtMain), std::ref(isImportRunning), std::ref(messageActive)).detach();
        } else if ((search && !messagePrinted) && (isHistoryFileEmpty(historyFilePath) || !fs::is_regular_file(historyFilePath))) {
            std::cout << originalColors::dim << "[Auto-Update: no stored folder paths to scan...]\n" << originalColors::resetPlain;
            messagePrinted = true;
            messageActive.store(true);
            std::thread(clearMessageAfterTimeout, 4, std::ref(isAtMain), std::ref(isImportRunning), std::ref(messageActive)).detach();
        }
        
        printMenu();
        clear_history();

        // --- User Input Processing ---
        const MainTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");
        char* rawInput = readline(("\n\001" + 
									std::string(isOriginal ? UI::Palette::Blue : theme->muted) + 
									"\002Choose an option:" + 
									std::string(UI::Palette::RL_BoldAlt) + 
									" ").c_str());
        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        if (!input.get()) {
            break; // Handle Ctrl+D
        }

        std::string choice(input.get());
        std::string initialDir = "";

        if (choice == "1") {
            isAtMain.store(false);
            isAtISOList.store(false);
            submenu1(updateHasRun, isAtISOList, isImportRunning, newISOFound);
        } else if (choice.length() == 1) {
            bool promptFlag;
            bool filterHistory;
            int maxDepth;

            switch (choice[0]) {
                case '2':
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    submenu2(newISOFound, isImportRunning);
                    break;
                case '3':
                    isAtMain.store(false);
                    isAtISOList.store(false);
                    promptFlag = true;
                    filterHistory = false;
                    maxDepth = -1;
                    refreshForDatabase(promptFlag, maxDepth, filterHistory, newISOFound);
                    clearScrollBuffer();
                    break;
                case '4':
                    exitProgram = true;
                    clearScrollBuffer();
                    break;
                default:
                    break;
            }
        }
    }

    // --- Cleanup ---
    std::cout << originalColors::resetPlain << std::flush;
    close(lockFileDescriptor);
    unlink(lockFile);
    return 0;
}
