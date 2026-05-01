// SPDX-License-Identifier: GPL-3.0-or-later

#include "../globals.h"
#include "../inputHandling.h"

/**
 * @brief Disables EOF (Ctrl+D) processing in the terminal.
 * @details Modifies the termios control characters to disable VEOF. 
 * This prevents the program from receiving an EOF signal which usually 
 * terminates a shell or input loop.
 */
void disable_ctrl_d() {
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_cc[VEOF] = _POSIX_VDISABLE;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }
}

/**
 * @brief Re-enables EOF (Ctrl+D) processing.
 * @details Restores VEOF to the standard ASCII 4 (End of Transmission).
 */
void enable_ctrl_d() {
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_cc[VEOF] = 4; // Standard default for most Unix systems
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }
}

/**
 * @brief Discards any unread data in the terminal input buffer.
 */
void flushStdin() {
    tcflush(STDIN_FILENO, TCIFLUSH);
}

/**
 * @brief Disables canonical mode and echoing.
 * @details Used during heavy processing or custom UI rendering to prevent 
 * user keystrokes from appearing on the screen or being buffered as line input.
 */
void disableInput() {
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }
}

/**
 * @brief Restores standard canonical input and echoing.
 */
void restoreInput() {
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_lflag |= ICANON | ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }
}

/**
 * @brief Configures the environment to ignore SIGINT (Ctrl+C).
 * @details Specifically instructs GNU Readline to stop catching signals 
 * and sets the system-wide SIGINT handler to SIG_IGN.
 */
void setupReadlineToIgnoreCtrlC() {
    // Prevent readline from overriding our signal logic
    rl_catch_signals = 0;

    struct sigaction sa_ignore;
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sa_ignore.sa_flags = 0;
    sigaction(SIGINT, &sa_ignore, nullptr);
}

/**
 * @brief Internal handler for cancellation signals.
 * @param signal The signal number caught (expected SIGINT).
 */
void signalHandlerCancellations(int signal) {
    if (signal == SIGINT) {
        // Atomic flag used by worker threads to stop processing
        g_operationCancelled = true;
    }
}

/**
 * @brief Sets up a handler to catch Ctrl+C for graceful cancellation.
 * @details Instead of terminating, the program sets a global flag 
 * allowing current tasks to finish or clean up before returning.
 */
void setupSignalHandlerCancellations() {
    struct sigaction sa;
    sa.sa_handler = signalHandlerCancellations;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

/**
 * @brief Global termination signal handler.
 * @details Handles fatal signals or exits by cleaning up UI buffers 
 * and releasing filesystem locks before terminating.
 * @param signum The signal number triggering the exit.
 */
void signalHandler(int signum) {
    // Ensure the terminal isn't left in a messy state
    clearScrollBuffer();

    // Release global lock if held
    if (lockFileDescriptor != -1) {
        close(lockFileDescriptor);
    }

    exit(signum);
}
