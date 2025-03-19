// SPDX-License-Identifier: GPL-2.0-or-later

#include "../headers.h"


// Global variable to lock program to one instance
int lockFileDescriptor = -1;

// Function to disable (Ctrl+D)
void disable_ctrl_d() {
    struct termios term;
    
    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &term);
    
    // Disable EOF (Ctrl+D) processing
    term.c_cc[VEOF] = _POSIX_VDISABLE;
    
    // Apply the modified settings
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to specifically re-enable Ctrl+D
void enable_ctrl_d() {
    struct termios term;
    
    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &term);
    
    // Re-enable EOF (Ctrl+D) - typically ASCII 4 (EOT)
    term.c_cc[VEOF] = 4;  // Default value for most systems
    
    // Apply the modified settings
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to flush input buffer
void flushStdin() {
    tcflush(STDIN_FILENO, TCIFLUSH);
}


// Function to disable input during processing
void disableInput() {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to restore normal input
void restoreInput() {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to ignore Ctrl+C
void setupReadlineToIgnoreCtrlC() {
    // Prevent readline from catching/interrupting signals
    rl_catch_signals = 0;

    // Configure SIGINT (Ctrl+C) to be ignored
    struct sigaction sa_ignore;
    sa_ignore.sa_handler = SIG_IGN;   // Ignore signal
    sigemptyset(&sa_ignore.sa_mask);  // Clear signal mask
    sa_ignore.sa_flags = 0;           // No special flags
    sigaction(SIGINT, &sa_ignore, nullptr);
}


// Signal handler for SIGINT (Ctrl+C)
void signalHandlerCancellations(int signal) {
    if (signal == SIGINT) {
        g_operationCancelled = true;
    }
}


// Setup signal handling
void setupSignalHandlerCancellations() {
    struct sigaction sa;
    sa.sa_handler = signalHandlerCancellations;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}


// Function to handle termination signals
void signalHandler(int signum) {

    clearScrollBuffer();
    // Perform cleanup before exiting
    if (lockFileDescriptor != -1) {
        close(lockFileDescriptor);
    }

    exit(signum);
}
