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
 * @brief Displays an interactive, multi-line progress bar with byte tracking and status reporting.
 * * Renders a dynamic terminal UI that tracks progress, speed, and task counts. 
 * Upon completion, it shrinks the UI to a compact final layout, displays a 
 * success/failure status, and prompts the user for verbose output preference.
 *
 * @param completedBytes   Atomic counter for bytes processed (nullptr to disable byte tracking).
 * @param totalBytes       Total bytes to process.
 * @param completedTasks   Atomic counter for successful tasks.
 * @param failedTasks      Atomic counter for failed tasks.
 * @param totalTasks       Total number of tasks.
 * @param isComplete       Atomic boolean signaling the worker thread has finished.
 * @param verbose          [out] Stores the user's choice to display detailed logs.
 * @param operation        The name of the operation (used for width adjustment and status).
 */
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes,
    std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks,
    std::atomic<bool>* isComplete, bool* verbose, const std::string& operation) {
    
    const ListTheme* theme = getActiveTheme();
    const bool isOriginal = (globalTheme == "original");

    std::string colorSuccess = isOriginal ? std::string(originalColors::green)   : std::string(theme->primary);
    std::string colorFailure = isOriginal ? std::string(originalColors::red)     : std::string(theme->secondary);
    std::string colorWarning = isOriginal ? std::string(originalColors::yellow)  : std::string(theme->warning);
    std::string colorStatus  = isOriginal ? std::string(originalColors::boldAlt) : std::string(theme->muted);
    
    disableInputForProgressBar(&oldt, &oldf);

    int processingBarWidth = 42;
    int finalBarWidth = 30;

    if (operation.find("mount") != std::string::npos ||
        operation.find("umount") != std::string::npos) {
        processingBarWidth = 46;
        finalBarWidth = 30;
    } else if (operation.find("mdf2iso") != std::string::npos ||
               operation.find("nrg2iso") != std::string::npos ||
               operation.find("chd2iso") != std::string::npos ||
               operation.find("daa2iso") != std::string::npos || 
               operation.find("ccd2iso") != std::string::npos) {
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

    auto renderProgressBar = [&](bool forceFull = false, bool useFinalLayout = false, bool cancelled = false) -> std::string {
        const size_t completedTasksValue  = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue     = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue  = bytesTrackingEnabled
            ? completedBytes->load(std::memory_order_acquire) : 0;

        double tasksProgress   = static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        double overallProgress = tasksProgress;

        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = std::max(bytesProgress, tasksProgress);
        }

        if (forceFull) overallProgress = 1.0;

        int barWidth    = useFinalLayout ? finalBarWidth : processingBarWidth;
        int progressPos = static_cast<int>(barWidth * overallProgress);

        auto currentTime    = std::chrono::high_resolution_clock::now();
        auto elapsedTime    = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        double elapsedSeconds = elapsedTime.count() / 1000.0;

        if (bytesTrackingEnabled && completedBytesValue != lastCompletedBytes) {
            cachedCompletedBytesFormatted = formatSize(static_cast<double>(completedBytesValue));
            double speed = elapsedSeconds > 0.0
                ? static_cast<double>(completedBytesValue) / elapsedSeconds : 0.0;
            cachedSpeedFormatted = formatSize(speed) + "/s";
            lastCompletedBytes = completedBytesValue;
        }

        std::stringstream ss;
        ss << "\r\033[2K" << color << "[";
        for (int i = 0; i < barWidth; ++i)
            ss << (i < progressPos ? "=" : (i == progressPos && !useFinalLayout ? ">" : " "));
        
        ss << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
           << "% (" << completedTasksValue << "/" << totalTasks << ") Time Elapsed: "
           << std::fixed << std::setprecision(1) << elapsedSeconds << "s";

        int outputLines = 1;
        if (bytesTrackingEnabled) {
            int percentPos = barWidth + 3;
            ss << '\n' << "\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            
            ss << "Processed: "
               << ((useFinalLayout && !cancelled) ? totalBytesFormatted : cachedCompletedBytesFormatted)
               << "/" << totalBytesFormatted;

            ss << '\n' << "\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            ss << "Speed: " << cachedSpeedFormatted;
            outputLines = 3;
        }

        lastRenderedLines = outputLines;
        return ss.str();
    };

    bool enterPressed = false;
    struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };

    while (!enterPressed) {
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            while (read(STDIN_FILENO, &ch, 1) > 0); 
        }

        std::string progressOutput = renderProgressBar(false, false, false);
        std::cout << progressOutput << std::flush;

        if (bytesTrackingEnabled && !isComplete->load(std::memory_order_acquire) && lastRenderedLines > 1) {
            std::cout << "\033[" << (lastRenderedLines - 1) << "A";
        }

        if (isComplete->load(std::memory_order_acquire)) {
            signal(SIGINT, SIG_IGN);

            if (bytesTrackingEnabled) std::cout << "\033[1J\033[3A";
            else                      std::cout << "\033[1J\033[1A";

            std::atomic_thread_fence(std::memory_order_acq_rel);

            const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
            const size_t failedTasksValue    = failedTasks->load(std::memory_order_acquire);
            const bool wasCancelled          = g_operationCancelled.load(std::memory_order_acquire);

            bool snapTo100 = (!wasCancelled && failedTasksValue == 0);

            std::cout << "\r\033[2K" << colorStatus
                      << " Status: " << operation << colorStatus << " → "
                      << (!wasCancelled
                          ? (failedTasksValue > 0
                             ? (completedTasksValue > 0
                                ? std::string(colorWarning) + "PARTIAL"
                                : std::string(colorFailure)  + "FAILED")
                             : std::string(colorSuccess) + "COMPLETED")
                          : std::string(colorWarning) + "INTERRUPTED")
                      << originalColors::boldAlt << '\n';

            std::cout << renderProgressBar(snapTo100, true, wasCancelled);

            disableReadlineForConfirmation();
            enterPressed = true;
            std::cout << "\n\n";

            restoreInput(&oldt, oldf);

            const std::string prompt =
                "\001" + std::string(color) + "\002" +
                "Display verbose output? (y/n): " +
                "\001" + std::string(originalColors::boldAlt) + "\002";

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
