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
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose, const std::string& operation) {
    disableInputForProgressBar(&oldt, &oldf);
    
    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    int processingBarWidth = 42;
    int finalBarWidth = 30;
    
    if (operation.find("mount") != std::string::npos || 
        operation.find("umount") != std::string::npos) {
        processingBarWidth = 46;
        finalBarWidth = 30;
    } else if (operation.find("MDF") != std::string::npos || 
               operation.find("NRG") != std::string::npos ||
               operation.find("CHD") != std::string::npos || 
               operation.find("DAA") != std::string::npos || 
               operation.find("BIN/IMG") != std::string::npos) {
        processingBarWidth = 49;
        finalBarWidth = 40;
    }
    
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();
    const bool bytesTrackingEnabled = (completedBytes != nullptr);
    
    auto formatSize = [](double bytes) -> std::string {
        std::stringstream ss;
        const char* units[] = {" B", " KB", " MB", " GB"};
        int unit = 0;
        double size = bytes;
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        ss << std::fixed << std::setprecision(2) << size << units[unit];
        return ss.str();
    };
    
    std::string totalBytesFormatted;
    if (bytesTrackingEnabled) {
        totalBytesFormatted = formatSize(static_cast<double>(totalBytes));
    }
    
    auto renderProgressBar = [&](bool isFinal = false) -> std::string {
        const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = bytesTrackingEnabled ? 
            completedBytes->load(std::memory_order_acquire) : 0;
        
        double tasksProgress = static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        double overallProgress = tasksProgress;
        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = std::max(bytesProgress, tasksProgress);
        }
        
        if (isFinal) overallProgress = 1.0;
        
        int barWidth = isFinal ? finalBarWidth : processingBarWidth;
        int progressPos = static_cast<int>(barWidth * overallProgress);
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        double elapsedSeconds = elapsedTime.count() / 1000.0;
        double speed = bytesTrackingEnabled && elapsedSeconds > 0.0 ? 
            (static_cast<double>(completedBytesValue) / elapsedSeconds) : 0.0;
        
        std::stringstream ss;
        
        // Use boldAlt for the standard bar text to match \033[0;1m
        ss << "\r\033[2K" << originalColors::boldAlt << "[";
        for (int i = 0; i < barWidth; ++i) {
            ss << (i < progressPos ? "=" : (i == progressPos && !isFinal ? ">" : " "));
        }
        ss << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
           << "% (" << completedTasksValue << "/" << totalTasks << ") Time Elapsed: " 
           << std::fixed << std::setprecision(1) << elapsedSeconds << "s";
        
        if (bytesTrackingEnabled) {
            int percentPos = barWidth + 3;
            ss << "\n\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            ss << "Processed: " << (isFinal ? totalBytesFormatted : formatSize(static_cast<double>(completedBytesValue)))
				<< "/" << totalBytesFormatted;

            ss << "\n\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            ss << "Speed: " << formatSize(speed) << "/s";
        }
        
        return ss.str();
    };
    
    while (!isComplete->load(std::memory_order_acquire) || !enterPressed) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0);
        
        std::string progressOutput = renderProgressBar();
        std::cout << progressOutput << std::flush;
        
        if (bytesTrackingEnabled && !isComplete->load(std::memory_order_acquire)) {
            std::cout << "\033[2A";
        }
        
        if (isComplete->load(std::memory_order_acquire) && !enterPressed) {
            signal(SIGINT, SIG_IGN);
            
            if (bytesTrackingEnabled) std::cout << "\033[1J\033[3A";
            else std::cout << "\033[1J\033[1A";
            
            // Full memory barrier to ensure all operations are synchronized
            std::atomic_thread_fence(std::memory_order_seq_cst);
            
            // Brief pause to allow any pending updates to settle
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            // Load all states with memory ordering
            size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
            size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
            bool wasCancelled = g_operationCancelled.load(std::memory_order_acquire);
            
            // Double-check after a tiny delay to catch any race conditions
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            // Re-check all states to ensure they're final
            size_t finalCompletedTasks = completedTasks->load(std::memory_order_acquire);
            size_t finalFailedTasks = failedTasks->load(std::memory_order_acquire);
            bool finalCancelled = g_operationCancelled.load(std::memory_order_acquire);
            
            // Use the most recent values if they changed
            if (finalCancelled != wasCancelled || 
                finalCompletedTasks != completedTasksValue || 
                finalFailedTasks != failedTasksValue) {
                completedTasksValue = finalCompletedTasks;
                failedTasksValue = finalFailedTasks;
                wasCancelled = finalCancelled;
            }
            
            // Status line using originalColors mappings with verified values
            std::cout << "\r\033[2K" << originalColors::boldAlt << " Status: " << operation << originalColors::boldAlt << " → " 
                      << (!wasCancelled 
                          ? (failedTasksValue > 0 
                             ? (completedTasksValue > 0 
                                ? std::string(originalColors::yellow) + "PARTIAL"
                                : std::string(originalColors::red) + "FAILED")
                             : std::string(originalColors::green) + "COMPLETED")
                          : std::string(originalColors::yellow) + "INTERRUPTED")
                      << originalColors::boldAlt << std::endl;
            
            // Render final progress bar with verified state
            std::cout << renderProgressBar(true);
            
            disableReadlineForConfirmation();
            enterPressed = true;
            std::cout << "\n\n";
            restoreInput(&oldt, oldf);
            
            // Readline prompt using originalColors::blue for the "original" look
            const std::string prompt = "\001" + std::string(isOrig ? originalColors::blue : theme->muted) + "\002" +
                                       "Display verbose output? (y/n): " + 
                                       "\001" + std::string(originalColors::boldAlt) + "\002";
                                       
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            
            if (input.get()) {
                std::string res = input.get();
                *verbose = (res == "y" || res == "Y");
            }
            
            restoreReadline();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    std::cout << std::endl;
    restoreInput(&oldt, oldf);
}
