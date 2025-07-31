// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"


// Terminal blocking for progress bar
struct termios oldt;
int oldf;


// Function to block input during progressBar updates
void disableInputForProgressBar(struct termios *oldt, int *oldf) {
    struct termios newt;
    
    tcgetattr(STDIN_FILENO, oldt);
    newt = *oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    *oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, *oldf | O_NONBLOCK);
}


//Function to restore input after progressBar is finished
void restoreInput(struct termios *oldt, int oldf) {
    tcsetattr(STDIN_FILENO, TCSANOW, oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
}


// End of terminal blocking for progress bar

// Function to display progress bar for native operations
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose,  const std::string& operation) {
    // Set up terminal for non-blocking input
    disableInputForProgressBar(&oldt, &oldf);

    int processingBarWidth = 42; // Default to 42
    int finalBarWidth = 30; // Default to 30
    
    if (operation.find("mount") != std::string::npos || 
    operation.find("umount") != std::string::npos) {
		processingBarWidth = 46;
		finalBarWidth = 30;
	} else if (operation.find("MDF") != std::string::npos || 
           operation.find("NRG") != std::string::npos || 
           operation.find("BIN/IMG") != std::string::npos) {
		processingBarWidth = 49;
		finalBarWidth = 40;
	}
    
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();
    const bool bytesTrackingEnabled = (completedBytes != nullptr);
    
    // Size formatting function
    auto formatSize = [](double bytes) -> std::string {
        static std::stringstream ss;
        const char* units[] = {" B", " KB", " MB", " GB"};
        int unit = 0;
        double size = bytes;
        
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        
        ss.str("");
        ss.clear();
        ss << std::fixed << std::setprecision(2) << size << units[unit];
        return ss.str();
    };
    
    // Pre-format total bytes if needed
    std::string totalBytesFormatted;
    if (bytesTrackingEnabled) {
        totalBytesFormatted = formatSize(static_cast<double>(totalBytes));
    }
    
    // Function to render the progress bar
    auto renderProgressBar = [&](bool isFinal = false) -> std::string {
        // Load current progress information
        const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = bytesTrackingEnabled ? 
            completedBytes->load(std::memory_order_acquire) : 0;
        
        // Calculate progress
        double tasksProgress = static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        double overallProgress = tasksProgress;
        if (bytesTrackingEnabled) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = std::max(bytesProgress, tasksProgress);
        }
        
        // For final display, always show 100%
        if (isFinal) {
            overallProgress = 1.0;
        }
        
        // Use different bar width based on whether this is the final render
        int barWidth = isFinal ? finalBarWidth : processingBarWidth;
        
        // Calculate the position of the progress bar
        int progressPos = static_cast<int>(barWidth * overallProgress);
        
        // Calculate elapsed time and speed
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        double elapsedSeconds = elapsedTime.count() / 1000.0;
        double speed = bytesTrackingEnabled && elapsedSeconds > 0.0 ? 
            (static_cast<double>(completedBytesValue) / elapsedSeconds) : 0.0;
        
        // Construct the progress bar display
        std::stringstream ss;
        
        // First line: progress bar with percentage, task count, and time elapsed
        ss << "\r[";
        for (int i = 0; i < barWidth; ++i) {
            ss << (i < progressPos ? "=" : (i == progressPos && !isFinal ? ">" : " "));
        }
        ss << "] " << std::fixed << std::setprecision(0) << (overallProgress * 100.0)
           << "% (" << completedTasksValue << "/" << totalTasks << ") Time Elapsed: " 
           << std::fixed << std::setprecision(1) << elapsedSeconds << "s\033[K";
        
        // Second line: size information right under the percentage part
        if (bytesTrackingEnabled) {
            // Calculate position to align "Completed" under the percentage
            int percentPos = barWidth + 3; // This positions "Completed" under the percentage
            ss << "\n\r";
            // Add spaces to align "Completed" under the percentage value
            for (int i = 0; i < percentPos; i++) {
                ss << " ";
            }
            ss << "Processed: " << formatSize(static_cast<double>(completedBytesValue)) 
               << "/" << totalBytesFormatted;

            // Add a new line and align the speed info under "Completed"
            ss << "\n\r";
            for (int i = 0; i < percentPos; i++) {
                ss << " ";
            }
            
            ss << "Speed: " << formatSize(speed) << "/s";
            
            ss << "\033[K";  // Clear to the end of line
        }
        
        return ss.str();
    };
    
    // Main loop to update progress bar
    while (!isComplete->load(std::memory_order_acquire) || !enterPressed) {
        // Non-blocking read to check for input
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0);
        
        // Display current progress
        std::string progressOutput = renderProgressBar();
        std::cout << progressOutput << std::flush;
        
        // Move cursor back up if we have a multi-line output (for updating in place)
        if (bytesTrackingEnabled && !isComplete->load(std::memory_order_acquire)) {
            std::cout << "\033[2A";  // Move cursor up one line
        }
        
        // If processing is complete, show a final message
        if (isComplete->load(std::memory_order_acquire) && !enterPressed) {
            signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C after completion
            
            // Show completion status (need to account for multi-line output)
            if (bytesTrackingEnabled) {
                std::cout << "\033[1J\033[3A";  // Clear above and move up two lines
            } else {
                std::cout << "\033[1J\033[1A";  // Clear above and move up one line
            }
            
            // Get current task counts for status determination
            const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
            const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
            
            // Using ternary operators to determine status based on task completion
            std::cout << "\r\033[0;1m Status: " << operation << "\033[0;1m → " 
                      << (!g_operationCancelled.load() 
                          ? (failedTasksValue > 0 
                             ? (completedTasksValue > 0 
                                ? "\033[1;93mPARTIAL" // Yellow, some completed, some failed
                                : "\033[1;91mFAILED")  // Red, none completed, some failed
                             : "\033[1;92mCOMPLETED") // Green, all completed successfully
                          : "\033[1;33mINTERRUPTED") // Yellow, operation cancelled
                      << "\033[0;1m" << std::endl;
            
            // Show final progress bar
            std::cout << renderProgressBar(true);
            
            // Disable certain key bindings temporarily
            disableReadlineForConfirmation();
            
            enterPressed = true;
            std::cout << "\n\n";
            
            // Restore terminal settings for input
            restoreInput(&oldt, oldf);
            // Prompt for verbose output
            const std::string prompt = "\033[1;94mDisplay verbose output? (y/n):\033[0;1m ";
            std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
            
            if (input.get()) {
                *verbose = (std::string(input.get()) == "y" || std::string(input.get()) == "Y");
            }
            
            // Restore key bindings after the prompt
            restoreReadline();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    std::cout << std::endl;
    
    // Final restoration of terminal settings
    restoreInput(&oldt, oldf);
}
