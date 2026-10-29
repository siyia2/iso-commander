// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "../globalMutexes.h"
#include "../inputHandling.h"
#include "../listResizeWatcher.h"
#include "../readline.h"
#include "../select.h"
#include "../state.h"
#include "../themes.h"
#include "../display.h"
#include "../sharedRefreshState.h"

/**
 * @brief Renders the multi-colored ASCII art banner to the terminal.
 *
 * Uses 24-bit TrueColor escapes for a vertical flame gradient (near-white heat
 * at the top, burnt maroon at the base).
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
        rgb(110,  10,   5),  // row 7: burnt maroon
    };

    std::cout << rows[0] << R"((   (       )            )    *      *              ) (         ( )" << "\n";
    std::cout << rows[1] << R"( )\ ))\ ) ( /(     (  ( /(  (  `   (  `    (     ( /( )\ )      )\ ) )" << "\n";
    std::cout << rows[2] << R"((()/(()/( )\())    )\ )\()) )\))(  )\))(   )\    )\()(()/(  (  (()/( )" << "\n";
    std::cout << rows[3] << R"( /(_)/(_)((_)\   (((_((_)\ ((_)()\((_)()((((_)( ((_)\ /(_)) )\  /(_)) )" << "\n";
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
 * @brief Displays the ISO management submenu and routes user input to file
 * operations.
 *
 * Renders a fixed six-operation menu (Mount, Umount, Delete, Move, Copy,
 * Write to USB) and dispatches single-digit choices (1-6) to
 * @c selectForIsoFiles with the corresponding operation string ("mount",
 * "umount", "rm", "mv", "cp", "write2usb"). Multi-character input is ignored
 * and the menu redraws.
 *
 * - @c isAtISOList is set to @c false on every iteration before drawing.
 * - A null readline return (Ctrl+D) or Esc breaks the loop.
 * - @c \\f and @c \\t are bound to no-ops each iteration so default Readline
 *   bindings cannot corrupt the menu.
 * - The menu is drawn by a lambda (@c drawMenu) also passed to
 *   @c readlineWithResize(), so it repaints on terminal-width changes while
 *   the prompt waits. The lambda stops one line short of the blank line
 *   before the prompt: the normal path prints that line itself, and the
 *   resize watcher prints it after a repaint.
 *
 * @param isAtISOList       Set to @c false each iteration; forwarded to
 *                          @c selectForIsoFiles.
 * @param refreshState      Shared @c RefreshState forwarded to each
 *                          @c selectForIsoFiles call for import coordination.
 * @param backgroundThreads Forwarded to each @c selectForIsoFiles call for
 *                          background worker lifetime management.
 */
void submenu1(std::atomic<bool>& isAtISOList,
    std::shared_ptr<RefreshState> refreshState,
    std::vector<std::thread>& backgroundThreads) {

    // Draws the menu box, ending on the line after it (NOT the blank line
    // before the prompt). Also used as the resize repaint; the blank line
    // comes from the caller / the watcher.
    auto drawMenu = [] {
        SyncOutput sync;
        clearScrollBuffer();

        std::cout << color << "+----------------------+\n"
                  << "|      ManageISO        |\n"
                  << "+----------------------+\n"
                  << "| 1. Mount             |\n"
                  << "+----------------------+\n"
                  << "| 2. Unmount           |\n"
                  << "+----------------------+\n"
                  << "| 3. Delete            |\n"
                  << "+----------------------+\n"
                  << "| 4. Move              |\n"
                  << "+----------------------+\n"
                  << "| 5. Copy              |\n"
                  << "+----------------------+\n"
                  << "| 6. Write             |\n"
                  << "+----------------------+" << "\n";
    };

    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        isAtISOList.store(false);
        drawMenu();
        std::cout << "\n";   // blank line before the prompt

        const ReadlineAndPromptTheme pt = getPromptTheme();
        const std::string prompt = std::string(pt.primary) +
                                   "Enter choice [1-6]: " +
                                   std::string(pt.reset);

        RlLine input = readlineWithResize(prompt, drawMenu);

        if (!input || input.get()[0] == 27) {
            break;
        }

        std::string choice(input.get());
        if (choice.length() == 1) {
            switch (choice[0]) {
                case '1':
                    clearScrollBuffer();
                    selectForIsoFiles("mount", isAtISOList, backgroundThreads, refreshState);
                    break;
                case '2':
                    clearScrollBuffer();
                    selectForIsoFiles("umount", isAtISOList, backgroundThreads, refreshState);
                    break;
                case '3':
                    clearScrollBuffer();
                    selectForIsoFiles("rm", isAtISOList, backgroundThreads, refreshState);
                    break;
                case '4':
                    clearScrollBuffer();
                    selectForIsoFiles("mv", isAtISOList, backgroundThreads, refreshState);
                    break;
                case '5':
                    clearScrollBuffer();
                    selectForIsoFiles("cp", isAtISOList, backgroundThreads, refreshState);
                    break;
                case '6':
                    clearScrollBuffer();
                    selectForIsoFiles("write2usb", isAtISOList, backgroundThreads, refreshState);
                    break;
            }
        }
    }
}

void promptSearchBinImgChdDaaMdfNrg(const std::string& fileTypeChoice, std::shared_ptr<RefreshState> state);

/**
 * @brief Displays the conversion submenu for transforming non-ISO disk images
 * to ISO format.
 *
 * Renders a fixed five-tool menu (CCD2ISO++, CHD2ISO++, DAA2ISO++, MDF2ISO++,
 * NRG2ISO++) and dispatches single-digit choices (1-5) to
 * @c promptSearchBinImgChdDaaMdfNrg with the corresponding format string
 * ("bin", "chd", "daa", "mdf", "nrg"). Multi-character input is ignored and
 * the menu redraws.
 *
 * - A null readline return (Ctrl+D) or Esc breaks the loop.
 * - @c \\f and @c \\t are bound to no-ops each iteration.
 * - The menu is drawn by a lambda also passed to @c readlineWithResize(), so
 *   it repaints on terminal-width changes while the prompt waits. The lambda
 *   stops one line short of the blank line before the prompt (the prompt has
 *   no leading newline): the normal path prints that line itself, and the
 *   resize watcher prints it after a repaint.
 *
 * @param state Shared @c RefreshState forwarded to each
 *              @c promptSearchBinImgChdDaaMdfNrg call; not read directly here.
 */
void submenu2(std::shared_ptr<RefreshState> state) {

    // Same layout rule as submenu1: ends one line short of the blank line.
    auto drawMenu = [] {
        SyncOutput sync;
        clearScrollBuffer();

        std::cout << color << "+------------------------+  \n"
                  << "|      Convert2ISO        |\n"
                  << "+------------------------+\n"
                  << "| 1. CCD2ISO++           |\n"
                  << "+------------------------+\n"
                  << "| 2. CHD2ISO++           |\n"
                  << "+------------------------+\n"
                  << "| 3. DAA2ISO++           |\n"
                  << "+------------------------+\n"
                  << "| 4. MDF2ISO++           |\n"
                  << "+------------------------+\n"
                  << "| 5. NRG2ISO++           |\n"
                  << "+------------------------+" << "\n";
    };

    while (true) {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);

        drawMenu();
        std::cout << "\n";   // blank line before the prompt

        const ReadlineAndPromptTheme pt = getPromptTheme();
        const std::string prompt = std::string(pt.primary) +
                                   "Enter choice [1-5]: " +
                                   std::string(pt.reset);

        RlLine input = readlineWithResize(prompt, drawMenu);

        if (!input || input.get()[0] == 27) {
            break;
        }

        std::string choice(input.get());
        std::string operation;
        if (choice.length() == 1) {
            switch (choice[0]) {
                case '1':
                    operation = "bin";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    break;
                case '2':
                    operation = "chd";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    break;
                case '3':
                    operation = "daa";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    break;
                case '4':
                    operation = "mdf";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    break;
                case '5':
                    operation = "nrg";
                    promptSearchBinImgChdDaaMdfNrg(operation, state);
                    break;
            }
        }
    }
}

/**
 * @brief Prints the primary application menu options.
 */
void printMenu() {
    std::cout << color << "+----------------------+\n"
              << "|      Main Menu        |\n"
              << "+----------------------+\n"
              << "| 1. ManageISO         |\n"
              << "+----------------------+\n"
              << "| 2. Convert2ISO       |\n"
              << "+----------------------+\n"
              << "| 3. ImportISO         |\n"
              << "+----------------------+\n"
              << "| 4. Settings          |\n"
              << "+----------------------+\n"
              << "| 5. Exit              |\n"
              << "+----------------------+" << "\n";
}

/**
 * @brief Draws the main menu screen (clear, banner, menu).
 *
 * Ends right after the menu's last line, so the caller (or the resize watcher)
 * supplies the blank line before the prompt. Shared by the main prompt's
 * resize repaint (@c readlineWithResize) and @c checkPendingRefresh(). Must
 * not call @c readline().
 */
void drawMainMenu() {
    SyncOutput sync;
    clearScrollBuffer();
    print_ascii();
    printMenu();
}

/**
 * @brief Readline event hook that performs deferred UI refreshes on the main
 * thread.
 *
 * Readline is not thread-safe, so no background thread may call Readline
 * functions (@c rl_on_new_line, @c rl_redisplay, ...) or write to the terminal
 * while a @c readline() call may be active. Background threads instead publish
 * a @c PendingRefreshKind via @c GlobalState::g_pendingRefreshKind (see
 * @c monitorAndClearMessage, @c clearMessageAfterTimeoutInMain,
 * @c refreshListAfterAutoUpdate). This function is installed as
 * @c rl_event_hook and invoked periodically by Readline itself, on the main
 * thread, while idle and waiting for input. It drains any pending request and
 * performs the corresponding redraw before restoring the user's current input
 * line.
 *
 * - **Suppression:** If @c GlobalState::g_suppressPendingRefresh is set, the
 *   hook returns immediately without consuming or acting on any pending
 *   request. Nested Readline prompts (e.g. the FilterTerms prompt in
 *   @c runSharedFilterFlow) use a different prompt/keybinding context, and
 *   redrawing over them mid-call would desync Readline's internal cursor/line
 *   state and crash. The pending request is left intact in
 *   @c g_pendingRefreshKind and retried on a later poll — refreshes are
 *   deferred, not dropped.
 * - **MainMenu:** Re-renders the main menu screen via @c drawMainMenu()
 *   (clear screen/scrollback, banner, menu).
 * - **IsoList:** Copies @c GlobalState::g_pendingRefreshState under its
 *   dedicated mutex (the pointer handoff is not otherwise thread-safe), then
 *   re-renders the ISO list via @c loadAndDisplayIso using that state.
 * - **None:** No-op.
 * - In all non-@c None cases, finishes by calling @c rl_on_new_line() and
 *   @c rl_redisplay() to repaint the user's prompt/input line beneath the new
 *   output without discarding what they've typed.
 * - **Staleness:** After consuming a request, the hook verifies on the main
 *   thread that the target screen is still active (@c GlobalState::isAtMain for
 *   MainMenu, @c GlobalState::isAtISOList for IsoList); otherwise the request
 *   is discarded. The publisher-side checks are only an optimization, since
 *   they can go stale before this hook runs.
 * - **Chaining:** When a @c ListResizeWatcher is active, its hook calls this
 *   function as the previously installed hook, so both keep working.
 *
 * @return Always 0, per the @c rl_event_hook / @c rl_hook_func_t contract.
 */
int checkPendingRefresh() {
    if (GlobalState::g_suppressPendingRefresh.load()) return 0; // don't consume; retry later

    PendingRefreshKind kind = GlobalState::g_pendingRefreshKind.exchange(PendingRefreshKind::None);
    if (kind == PendingRefreshKind::None) return 0;

    // Validate at consumption time: the publisher's flag check can go stale
    // before this hook runs (e.g. the user left the screen in between).
    if (kind == PendingRefreshKind::MainMenu && !GlobalState::isAtMain.load())    return 0;
    if (kind == PendingRefreshKind::IsoList  && !GlobalState::isAtISOList.load()) return 0;

    if (kind == PendingRefreshKind::MainMenu) {
        drawMainMenu();
        std::cout << "\n" << std::flush;
        rl_on_new_line();
        rl_redisplay();
    } else if (kind == PendingRefreshKind::IsoList) {
        std::shared_ptr<RefreshState> state;
        {
            std::lock_guard<std::mutex> lk(GlobalMutexes::readLineMutex);
            state = GlobalState::g_pendingRefreshState;
        }
        if (state) {
            SyncOutput sync;
            loadAndDisplayIso(state->filteredFiles, state->isFiltered, state->listSubtype,
                              state->umountMvRmBreak, state->pendingIndices, state->hasPendingProcess,
                              state->currentPage, state->originalPage, state);
            std::cout << "\n" << std::flush;
            rl_on_new_line();
            rl_redisplay();
        }
    }
    return 0;
}

/**
 * @brief Observes background task completion and requests an asynchronous UI
 * refresh.
 *
 * Runs in a background thread, waiting for a database import to finish (or the
 * program to signal shutdown), then, if the user is at the main menu, publishes
 * @c PendingRefreshKind::MainMenu via @c GlobalState::g_pendingRefreshKind.
 * The actual redraw — clearing the "Auto-Update" status line, re-rendering the
 * banner and menu, and restoring the user's input prompt via Readline's
 * @c rl_on_new_line() / @c rl_redisplay() — happens later, on the main thread,
 * in @c checkPendingRefresh. This function makes no Readline calls and writes
 * nothing to the terminal, since Readline is not thread-safe.
 *
 * @param state         Shared state with the import flag; refresh fires when
 *                      the import completes.
 * @param messageActive Flag tracking whether the status message is visible.
 * @param stopSignal    Abort monitoring during program exit.
 * @param isAtMain      Restricts the refresh to the primary menu.
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
        messageActive.store(false);
        GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::MainMenu);
    }
}

/**
 * @brief Threaded worker that requests a status-message clear/redraw after a
 * delay.
 *
 * Waits for a given number of 500 ms ticks, checking a stop flag each tick so
 * it can exit promptly on program termination. If the user is at the main menu
 * and no import is running, publishes @c PendingRefreshKind::MainMenu via
 * @c GlobalState::g_pendingRefreshKind so @c checkPendingRefresh performs the
 * actual redraw on the main thread. Makes no Readline calls and writes nothing
 * to the terminal itself.
 *
 * @param timeoutTicks  Number of 500 ms ticks to wait before clearing
 *                      (e.g. 2 = 1s, 8 = 4s).
 * @param isAtMain      Tracks whether the user is on the main menu.
 * @param state         Shared state with the import flag; suppresses the
 *                      clear while an import is running.
 * @param messageActive Flag indicating a temporary message is visible.
 * @param stopMessage   Stop flag checked every 500 ms for early exit.
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
            messageActive.store(false);
            GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::MainMenu);
        }
    }
}

/**
 * @brief Clears the terminal screen and resets the scrollback buffer.
 *
 * Uses ANSI escapes: \033[2J (clear screen), \033[3J (clear scrollback),
 * \033[H (cursor home), \033[0m (reset attributes).
 */
void clearScrollBuffer() {
    std::cout << "\033[2J\033[3J\033[H\033[0m";
}
