// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

/**
 * @brief Renders a multi-colored ASCII art banner to the terminal.
 * * Uses TrueColor (24-bit RGB) escape sequences to create a vertical flame 
 * gradient sampled from real fire photography, transitioning from 
 * near-white heat at the top to burnt maroon at the base.
 */
void print_ascii() {

    auto rgb = [](int r, int g, int b) -> std::string {
        return "\033[38;2;" + std::to_string(r) + ";" +
               std::to_string(g) + ";" + std::to_string(b) + "m";
    };

    const std::string reset = "\033[0m";

    // True flame gradient sampled from real fire photography
    std::string rows[] = {
        rgb(255, 255, 240),  // row 0: near-white (hottest tips)
        rgb(255, 248, 150),  // row 1: pale lemon
        rgb(255, 220,  50),  // row 2: bright yellow
        rgb(255, 165,   0),  // row 3: orange
        rgb(240,  80,   0),  // row 4: red-orange
        rgb(200,  20,   0),  // row 5: bright red
        rgb(175,  20,  15),  // row 6: rich crimson
        rgb(110,  10,  5),  // row 7: burnt maroon
    };

    std::cout << rows[0] << R"( (   (       )             )    * * ) (         (  )" << "\n";
    std::cout << rows[1] << R"( )\ ))\ ) ( /(     (  ( /(  (  `   (  `    (      ( /( )\ )      )\ ) )" << "\n";
    std::cout << rows[2] << R"((()/(()/( )\())    )\ )\()) )\))(  )\))(   )\     )\()(()/(  (  (()/( )" << "\n";
    std::cout << rows[3] << R"( /(_)/(_)((_)\    (((_((_)\ ((_)()\((_)()((((_)( ((_)\ /(_)) )\  /(_)) )" << "\n";
    std::cout << rows[4] << R"((_))(_))   ((_)  )\___ ((_)(_()((_(_()((_)\ _ )\ _((_(_))_ ((_)(_)) )" << "\n";
    std::cout << rows[5] << R"(|_ _/ __| / _ \ ((/ __/ _ \|  \/  |  \/  (_)_\(_| \| ||   \| __| _ \ )" << "\n";
    std::cout << rows[6] << R"( | |\__ \| (_) | | (_| (_) | |\/| | |\/| |/ _ \ | .` || |) | _||   / )" << "\n";
    std::cout << rows[7] << R"(|___|___/ \___/   \___\___/|_|  |_|_|  |_/_/ \_\|_|\_||___/|___|_|_\ )" << "\n\n" << reset;
}

/**
 * @brief Displays the ISO management submenu and handles user input for file operations.
 * * Provides options for mounting, unmounting, deleting, moving, copying, or writing ISOs.
 * * @param updateHasRun Indicates if the file list needs a refresh.
 * @param isAtISOList Atomic flag used to track terminal state for background processes.
 * @param isImportRunning Tracks if an import process is currently active.
 * @param newISOFound Signals if a new ISO has been discovered during background scans.
 */
void submenu1(std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
        
        isAtISOList.store(false);
        clearScrollBuffer();

        std::cout << color << "+-------------------------+\n"
                  << "|↵ Manage ISO             |\n"
                  << "+-------------------------+\n"
                  << "|1. Mount                 |\n"
                  << "+-------------------------+\n"
                  << "|2. Umount                |\n"
                  << "+-------------------------+\n"
                  << "|3. Delete                |\n"
                  << "+-------------------------+\n"
                  << "|4. Move                  |\n"
                  << "+-------------------------+\n"
                  << "|5. Copy                  |\n"
                  << "+-------------------------+\n"
                  << "|6. Write                 |\n"
                  << "+-------------------------+" << reset << std::endl << "\n";
        
        const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");
        char* rawInput = readline(("\001" + 
									std::string(isOriginal ? originalColors::blue : theme->muted) + 
									"\002Choose an option:" + 
									std::string(originalColors::rl_boldAlt) + 
									" ").c_str());
        
        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        if (!input.get() || std::strlen(input.get()) == 0) {
            break; 
        }

        std::string choice(input.get());
        if (choice.length() == 1) {
            switch (choice[0]) {
                case '1':
                    clearScrollBuffer();
                    selectForIsoFiles("mount", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
                case '2':
                    clearScrollBuffer();
                    selectForIsoFiles("umount", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
                case '3':
                    clearScrollBuffer();
                    selectForIsoFiles("rm", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
                case '4':
                    clearScrollBuffer();
                    selectForIsoFiles("mv", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
                case '5':
                    clearScrollBuffer();
                    selectForIsoFiles("cp", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
                case '6':
                    clearScrollBuffer();
                    selectForIsoFiles("write", updateHasRun, isAtISOList, isImportRunning, newISOFound);
                    clearScrollBuffer();
                    break;
            }
        }
    }
}

/**
 * @brief Displays the conversion submenu for transforming non-ISO disk images into ISO format.
 * * Supports .CCD, .MDF, and .NRG image formats using C++ implementations of conversion tools.
 * * @param newISOFound Atomic flag to notify main thread of newly created ISO files.
 * @param isImportRunning Tracks background import state to prevent menu collisions.
 */
void submenu2(std::atomic<bool>& newISOFound, std::atomic<bool>& isImportRunning) {
    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
        
        clearScrollBuffer();
        
        std::cout << color << "+-------------------------+\n"
                  << "|↵ Convert2ISO (DataOnly) |     |\n| ! Breaks Emu-Compat     |\n"
                  << "+-------------------------+\n"
                  << "|1. CCD2ISO++             |\n"
                  << "+-------------------------+\n"
                  << "|2. CHD2ISO++             |\n"
                  << "+-------------------------+\n"
                  << "|3. MDF2ISO++             |\n"
                  << "+-------------------------+\n"
                  << "|4. NRG2ISO++             |\n"
                  << "+-------------------------+" << reset << std::endl << "\n";
        
        const ListTheme* theme = getActiveTheme();
        const bool isOriginal = (globalTheme == "original");
        char* rawInput = readline(("\001" + 
									std::string(isOriginal ? originalColors::blue : theme->muted) + 
									"\002Choose an option:" + 
									std::string(originalColors::rl_boldAlt) + 
									" ").c_str());

        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        if (!input.get() || std::strlen(input.get()) == 0) {
            break; 
        }

        std::string choice(input.get());
        std::string operation;
        if (choice.length() == 1) {
            switch (choice[0]) {
                case '1':
                    operation = "bin";
                    promptSearchBinImgChdMdfNrg(operation, newISOFound, isImportRunning);
                    clearScrollBuffer();
                    break;
                case '2':
                    operation = "chd";
                    promptSearchBinImgChdMdfNrg(operation, newISOFound, isImportRunning);
                    clearScrollBuffer();
                    break;
                case '3':
                    operation = "mdf";
                    promptSearchBinImgChdMdfNrg(operation, newISOFound, isImportRunning);
                    clearScrollBuffer();
                    break;
                case '4':
                    operation = "nrg";
                    promptSearchBinImgChdMdfNrg(operation, newISOFound, isImportRunning);
                    clearScrollBuffer();
                    break;
            }
        }
    }
}

/**
 * @brief Prints the primary application menu options.
 */
void printMenu() {
    std::cout << color << "+-------------------------+\n"
              << "|       Menu Options      |\n"
              << "+-------------------------+\n"
              << "|1. ManageISO             |\n"
              << "+-------------------------+\n"
              << "|2. Convert2ISO           |\n"
              << "+-------------------------+\n"
              << "|3. ImportISO             |\n"
              << "+-------------------------+\n"
              << "|4. Exit                  |\n"
              << "+-------------------------+" << "\n";
}

/**
 * @brief Threaded worker that clears temporary status messages from the terminal after a delay.
 * * It ensures that messages don't persist indefinitely and triggers a UI redraw if 
 * the user is currently at the main menu.
 * * @param timeoutSeconds Duration to wait before clearing.
 * @param isAtMain Tracks if the user is currently viewing the main menu.
 * @param isImportRunning Prevents clearing if an active import is printing logs.
 * @param messageActive Flag indicating a temporary message is currently visible.
 */
void clearMessageAfterTimeout(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));
        
        if (!isImportRunning.load()) {
            if (messageActive.load() && isAtMain.load()) {
                clearScrollBuffer();
                print_ascii();
                printMenu();
                std::cout << "\n";
                rl_on_new_line(); 
                rl_redisplay();
                messageActive.store(false);
            }
            break; 
        }
    }
}

/**
 * @brief Clears the terminal screen and resets the scrollback buffer.
 * * Uses ANSI escape sequences: 
 * - \033[3J: Clear scrollback
 * - \033[2J: Clear entire screen
 * - \033[H: Move cursor to home position
 */
void clearScrollBuffer() {
    std::cout << "\033[3J\033[2J\033[H\033[0m" << std::flush;
}
