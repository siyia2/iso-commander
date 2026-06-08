// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// C / System Headers
#include <readline/readline.h>

// Project Headers
#include "../inputHandling.h"
#include "../readline.h"
#include "../themes.h"
#include "../select.h"

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
    std::cout << rows[1] << R"( )\ ))\ ) ( /(     (  ( /(  (  `   (  `    (      ( /( )\ )     )\ ) )" << "\n";
    std::cout << rows[2] << R"((()/(()/( )\())    )\ )\()) )\))(  )\))(   )\     )\()(()/(  ( (()/( )" << "\n";
    std::cout << rows[3] << R"( /(_)/(_)((_)\    (((_((_)\ ((_)()\((_)()((((_)( ((_)\ /(_)) )\ /(_)) )" << "\n";
    std::cout << rows[4] << R"((_))(_))   ((_)  )\___ ((_)(_()((_(_()((_)\ _ )\ _((_(_))_ ((_)(_)) )" << "\n";
    std::cout << rows[5] << R"(|_ _/ __| / _ \ ((/ __/ _ \|  \/  |  \/  (_)_\(_| \| ||   \| __| _ \ )" << "\n";
    std::cout << rows[6] << R"( | |\__ \| (_) | | (_| (_) | |\/| | |\/| |/ _ \ | .` || |) | _||   / )" << "\n";
    std::cout << rows[7] << R"(|___|___/ \___/   \___\___/|_|  |_|_|  |_/_/ \_\|_|\_||___/|___|_|_\ )" << "\n\n" << reset;
}

void selectForIsoFiles(const std::string& operation,
    std::atomic<bool>& isAtISOList,
    std::vector<std::thread>& backgroundThreads,
    std::shared_ptr<RefreshState> refreshState);

/**
 * @brief Displays the ISO management submenu and routes user input to file operations.
 *
 * Renders a fixed menu of six operations (Mount, Umount, Delete, Move, Copy,
 * Write to USB) and dispatches single-digit choices (1–6) to @c selectForIsoFiles
 * with the corresponding operation string ("mount", "umount", "rm", "mv", "cp",
 * "write2usb"). Multi-character input is silently ignored and the menu redraws.
 *
 * @details
 * - **Loop entry:** @c isAtISOList is set to @c false and the scroll buffer is
 *   cleared on every iteration before the menu is drawn.
 * - **Exit condition:** A null readline return (Ctrl+D) or empty input breaks
 *   the loop and returns to the caller.
 * - **Readline keybindings:** @c \\f and @c \\t are bound to no-ops at the top
 *   of each iteration to prevent terminal corruption during menu input.
 *
 * @param isAtISOList       Set to @c false each iteration before the menu is drawn;
 *                          passed into @c selectForIsoFiles to track whether the
 *                          ISO list is currently active.
 * @param refreshState      Shared @c RefreshState passed through to each
 *                          @c selectForIsoFiles call for import coordination.
 * @param backgroundThreads Passed through to each @c selectForIsoFiles call for
 *                          background worker lifetime management.
 */
void submenu1(std::atomic<bool>& isAtISOList,
    std::shared_ptr<RefreshState> refreshState,
    std::vector<std::thread>& backgroundThreads) {
    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        isAtISOList.store(false);
        clearScrollBuffer();

        std::cout << color << "+-----------------------+\n"
                  << "|      ↵ ManageISO       |\n"
                  << "+-----------------------+\n"
                  << "| 1. Mount              |\n"
                  << "+-----------------------+\n"
                  << "| 2. Unmount            |\n"
                  << "+-----------------------+\n"
                  << "| 3. Delete             |\n"
                  << "+-----------------------+\n"
                  << "| 4. Move               |\n"
                  << "+-----------------------+\n"
                  << "| 5. Copy               |\n"
                  << "+-----------------------+\n"
                  << "| 6. Write              |\n"
                  << "+-----------------------+" << "\n\n";

        const ReadlineAndPromptTheme pt = getPromptTheme();
        char* rawInput = readline(( std::string(pt.primary) +
                                    "Enter choice [1-6]: " +
                                    std::string(pt.reset)).c_str());

        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        if (!input.get() || std::strlen(input.get()) == 0) {
            break;
        }

        std::string choice(input.get());
        if (choice.length() == 1) {
            switch (choice[0]) {
                case '1':
                    clearScrollBuffer();
                    selectForIsoFiles("mount", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
                case '2':
                    clearScrollBuffer();
                    selectForIsoFiles("umount", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
                case '3':
                    clearScrollBuffer();
                    selectForIsoFiles("rm", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
                case '4':
                    clearScrollBuffer();
                    selectForIsoFiles("mv", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
                case '5':
                    clearScrollBuffer();
                    selectForIsoFiles("cp", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
                case '6':
                    clearScrollBuffer();
                    selectForIsoFiles("write2usb", isAtISOList, backgroundThreads, refreshState);
                    clearScrollBuffer();
                    break;
            }
        }
    }
}

void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::shared_ptr<RefreshState> state);

/**
 * @brief Displays the conversion submenu for transforming non-ISO disk images to ISO format.
 *
 * Renders a fixed menu of five conversion tools (CCD2ISO++, CHD2ISO++, DAA2ISO++,
 * MDF2ISO++, NRG2ISO++) and dispatches single-digit choices (1–5) to
 * @c promptSearchBinImgChdDaaMdfNrg with the corresponding format string
 * ("bin", "chd", "daa", "mdf", "nrg"). Multi-character input is silently
 * ignored and the menu redraws.
 *
 * @details
 * - **Exit condition:** A null readline return (Ctrl+D) or empty input breaks
 *   the loop and returns to the caller.
 * - **Readline keybindings:** @c \\f and @c \\t are bound to no-ops at the top
 *   of each iteration to prevent terminal corruption during menu input.
 *
 * @param state   Shared @c RefreshState passed through to each
 *                @c promptSearchBinImgChdDaaMdfNrg call; not read directly
 *                by this function.
 */
void submenu2(std::shared_ptr<RefreshState> state) {
    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        clearScrollBuffer();

        std::cout << color << "+-------------------------+  \n"
                  << "|      ↵ Convert2ISO       |\n"
                  << "+-------------------------+\n"
                  << "| 1. CCD2ISO++            |\n"
                  << "+-------------------------+\n"
                  << "| 2. CHD2ISO++            |\n"
                  << "+-------------------------+\n"
                  << "| 3. DAA2ISO++            |\n"
                  << "+-------------------------+\n"
                  << "| 4. MDF2ISO++            |\n"
                  << "+-------------------------+\n"
                  << "| 5. NRG2ISO++            |\n"
                  << "+-------------------------+" << "\n\n";

        const ReadlineAndPromptTheme pt = getPromptTheme();
        char* rawInput = readline(( std::string(pt.primary) +
                                    "Enter choice [1-5]: " +
                                    std::string(pt.reset)).c_str());

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
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    clearScrollBuffer();
                    break;
                case '2':
                    operation = "chd";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    clearScrollBuffer();
                    break;
                case '3':
                    operation = "daa";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    clearScrollBuffer();
                    break;
                case '4':
                    operation = "mdf";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    clearScrollBuffer();
                    break;
                case '5':
                    operation = "nrg";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
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
    std::cout << color << "+-----------------------+\n"
              << "|      Main Menu         |\n"
              << "+-----------------------+\n"
              << "| 1. ManageISO          |\n"
              << "+-----------------------+\n"
              << "| 2. Convert2ISO        |\n"
              << "+-----------------------+\n"
              << "| 3. ImportISO          |\n"
              << "+-----------------------+\n"
              << "| 4. Settings           |\n"
              << "+-----------------------+\n"
              << "| 5. Exit               |\n"
              << "+-----------------------+" << "\n";
}

/**
 * @brief Observes background task completion and performs an asynchronous UI refresh.
 *
 * This function runs in a background thread to monitor the lifecycle of a database
 * import. Once the task finishes (or the program signals a shutdown), it evaluates
 * if the user is currently at the main menu.
 *
 * If active, it performs a "soft refresh" by:
 * 1. Clearing the terminal buffer to remove the "Auto-Update" status line.
 * 2. Re-rendering the ASCII art and menu options.
 * 3. Utilizing Readline's internal state functions (rl_on_new_line, rl_redisplay)
 *    to restore the user's current input prompt without clearing any text.
 *
 * @param state         Shared state with import flag; refresh occurs when import completes.
 * @param messageActive Atomic flag tracking if the status message is visible.
 * @param stopSignal    Atomic flag to abort monitoring during program exit.
 * @param isAtMain      Atomic flag ensuring refresh only occurs on the primary menu.
 */
void monitorAndClearMessage(std::shared_ptr<RefreshState> state, std::atomic<bool>& messageActive,
                            std::atomic<bool>& stopSignal, std::atomic<bool>& isAtMain) {
    if (state) {
        std::unique_lock<std::mutex> lock(state->importMutex);
        state->importCV.wait(lock, [&] {
            return !state->isImportRunning.load() || stopSignal.load();
        });
    }
    if (messageActive.load() && !stopSignal.load() && isAtMain.load()) {
        clearScrollBuffer();
        print_ascii();
        printMenu();
        std::cout << "\n";
        rl_on_new_line();
        rl_redisplay();
        messageActive.store(false);
    }
}

/**
 * @brief Threaded worker that clears temporary status messages from the terminal after a delay.
 *
 * Waits for a given number of 500ms ticks before clearing the message, checking
 * a stop flag on each tick to allow prompt exit when the program terminates.
 * Triggers a UI redraw if the user is currently at the main menu.
 *
 * @param timeoutTicks   Number of 500ms ticks to wait before clearing (e.g. 2 = 1s, 8 = 4s).
 * @param isAtMain       Tracks if the user is currently viewing the main menu.
 * @param messageActive  Flag indicating a temporary message is currently visible.
 * @param stopMessage    Stop flag checked every 500ms to allow early exit.
 * @param state          Shared state with import flag; prevents clearing if import is running.
 */
void clearMessageAfterTimeoutInMain(int timeoutTicks, std::atomic<bool>& isAtMain,
                                    std::shared_ptr<RefreshState> state,
                                    std::atomic<bool>& messageActive,
                                    std::atomic<bool>& stopMessage) {
    int elapsed = 0;
    while (elapsed < timeoutTicks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        elapsed++;
        if (stopMessage.load()) return;
    }

    if (!(state && state->isImportRunning.load())) {
        if (messageActive.load() && isAtMain.load()) {
            clearScrollBuffer();
            print_ascii();
            printMenu();
            std::cout << "\n";
            rl_on_new_line();
            rl_redisplay();
            messageActive.store(false);
        }
    }
}

/**
 * @brief Clears the terminal screen and resets the scrollback buffer.
 * * Uses ANSI escape sequences:
 * - \033[2J: Clear entire screen
 * - \033[3J: Clear scrollback
 * - \033[H: Move cursor to home position
 */
void clearScrollBuffer() {
    std::cout << "\033[2J\033[3J\033[H\033[0m" << std::flush;
}
