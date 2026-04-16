// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

/**
 * @file progress_bar.cpp
 * @brief Terminal progress bar implementation with input blocking and themed output.
 */

struct termios oldt;
int oldf;

/**
 * @brief Disables terminal canonical mode and echoing to block user input during progress updates.
 * @param oldt Pointer to store original terminal settings.
 * @param oldf Pointer to store original file descriptor flags.
 */
void disableInputForProgressBar(struct termios *oldt, int *oldf) {
    struct termios newt;
    
    tcgetattr(STDIN_FILENO, oldt);
    newt = *oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    *oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, *oldf | O_NONBLOCK);
}

/**
 * @brief Restores terminal settings to their original state.
 * @param oldt Pointer to original terminal settings.
 * @param oldf Original file descriptor flags.
 */
void restoreInput(struct termios *oldt, int oldf) {
    tcsetattr(STDIN_FILENO, TCSANOW, oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
}

/**
 * @brief Displays a multi-line progress bar with byte tracking, task counts, and time elapsed.
 * @param completedBytes Atomic counter for bytes processed.
 * @param totalBytes Total bytes to process.
 * @param completedTasks Atomic counter for successful tasks.
 * @param failedTasks Atomic counter for failed tasks.
 * @param totalTasks Total number of tasks.
 * @param isComplete Atomic boolean signaling operation completion.
 * @param verbose Pointer to boolean to store user preference for verbose output.
 * @param operation Description of the current operation.
 */
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes,
    std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks,
    std::atomic<bool>* isComplete, bool* verbose, const std::string& operation) {
    
    // 1. Resolve theme once at the start
    ProgressBarColors pc = resolveProgressTheme();
    
    disableInputForProgressBar(&oldt, &oldf);

    // --- Configuration Logic ---
    int processingBarWidth = 42;
    int finalBarWidth = 30;

    // Adjust width based on operation name length to keep UI aligned
    if (operation.find("mount") != std::string::npos || operation.find("umount") != std::string::npos) {
        processingBarWidth = 46;
    } else if (operation.find("2iso") != std::string::npos) {
        processingBarWidth = 49;
        finalBarWidth = 41;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    const bool bytesTrackingEnabled = (completedBytes != nullptr);

    auto formatSize = [](double bytes) -> std::string {
        std::stringstream ss;
        const char* units[] = {" B", " KB", " MB", " GB"};
        int unit = 0;
        double size = bytes;
        while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
        ss << std::fixed << std::setprecision(2) << size << units[unit];
        return ss.str();
    };

    std::string totalBytesFormatted;
    size_t lastCompletedBytes = SIZE_MAX;
    std::string cachedCompletedBytesFormatted = "0.00 B";
    std::string cachedSpeedFormatted = "0.00 B/s";

    if (bytesTrackingEnabled)
        totalBytesFormatted = formatSize(static_cast<double>(totalBytes));

    int lastRenderedLines = 1;

    // --- High-Frequency Render Lambda ---
    auto renderProgressBar = [&](bool forceFull = false, bool useFinalLayout = false, bool cancelled = false) -> std::string {
        const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue    = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = bytesTrackingEnabled ? completedBytes->load(std::memory_order_acquire) : 0;

        double tasksProgress = cancelled 
            ? static_cast<double>(completedTasksValue) / totalTasks 
            : static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        
        double overallProgress = tasksProgress;

        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = cancelled ? bytesProgress : std::max(bytesProgress, tasksProgress);
        }

        if (forceFull && !cancelled) overallProgress = 1.0;

        int barWidth    = useFinalLayout ? finalBarWidth : processingBarWidth;
        int progressPos = static_cast<int>(barWidth * overallProgress);

        auto currentTime    = std::chrono::high_resolution_clock::now();
        double elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count() / 1000.0;

        if (bytesTrackingEnabled && completedBytesValue != lastCompletedBytes) {
            cachedCompletedBytesFormatted = formatSize(static_cast<double>(completedBytesValue));
            double speed = elapsedSeconds > 0.0 ? static_cast<double>(completedBytesValue) / elapsedSeconds : 0.0;
            cachedSpeedFormatted = formatSize(speed) + "/s";
            lastCompletedBytes = completedBytesValue;
        }

        std::stringstream ss;
        // Draw the bar: [===>    ]
        ss << "\r\033[2K" << color << "[" << color;
        for (int i = 0; i < barWidth; ++i) {
            ss << (i < progressPos ? "=" : (i == progressPos && !useFinalLayout ? ">" : " "));
            if (i == progressPos && !useFinalLayout) ss << color;
        }
        
        ss << color << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
           << "% (" << completedTasksValue << "/" << totalTasks << ") Time: "
           << std::fixed << std::setprecision(1) << elapsedSeconds << "s";

        int outputLines = 1;
        if (bytesTrackingEnabled) {
            int padding = barWidth + 3;
            ss << '\n' << "\r\033[2K" << std::string(padding, ' ') << "Processed: "
               << ((useFinalLayout && !cancelled) ? totalBytesFormatted : cachedCompletedBytesFormatted)
               << "/" << totalBytesFormatted;

            ss << '\n' << "\r\033[2K" << std::string(padding, ' ') << "Speed: ";
            if (useFinalLayout && !cancelled) {
                double avgSpeed = elapsedSeconds > 0.0 ? static_cast<double>(totalBytes) / elapsedSeconds : 0.0;
                ss << formatSize(avgSpeed) << "/s (avg)";
            } else {
                ss << cachedSpeedFormatted;
            }
            outputLines = 3;
        }

        lastRenderedLines = outputLines;
        return ss.str();
    };

    // --- Main Update Loop ---
    bool enterPressed = false;
    struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };

    while (!enterPressed) {
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            while (read(STDIN_FILENO, &ch, 1) > 0); 
        }

        std::cout << renderProgressBar(false, false, false) << std::flush;

        if (bytesTrackingEnabled && !isComplete->load(std::memory_order_acquire) && lastRenderedLines > 1) {
            std::cout << "\033[" << (lastRenderedLines - 1) << "A";
        }

        if (isComplete->load(std::memory_order_acquire)) {
            signal(SIGINT, SIG_IGN);

            // Clean up cursor position for final report
            std::cout << (bytesTrackingEnabled ? "\033[1J\033[3A" : "\033[1J\033[1A");

            const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
            const size_t failedTasksValue    = failedTasks->load(std::memory_order_acquire);
            const bool wasCancelled          = g_operationCancelled.load(std::memory_order_acquire);
            bool snapTo100 = (!wasCancelled && failedTasksValue == 0);

            // --- Status Summary ---
            std::cout << "\r\033[2K" << pc.status << " Status: " << operation << " → ";
            if (wasCancelled) {
                std::cout << pc.warning << "INTERRUPTED";
            } else if (failedTasksValue > 0) {
                std::cout << (completedTasksValue > 0 ? pc.warning : pc.failure) 
                          << (completedTasksValue > 0 ? "PARTIAL" : "FAILED");
            } else {
                std::cout << pc.success << "COMPLETED";
            }
            std::cout << pc.reset << '\n';

            // Final bar state
            std::cout << renderProgressBar(snapTo100, true, wasCancelled);

            disableReadlineForConfirmation();
            enterPressed = true;
            std::cout << "\n\n";

            restoreInput(&oldt, oldf);

            // --- Verbose Prompt (Readline safe) ---
            const std::string prompt = "\001" + std::string(color) + "\002" +
                                       "Display verbose output? (y/n): " +
                                       "\001" + std::string(color) + "\002";

            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            if (input.get()) {
                std::string res = input.get();
                *verbose = (res == "y" || res == "Y");
            }

            restoreReadline();
        }
    }
    std::cout << '\n';
}
