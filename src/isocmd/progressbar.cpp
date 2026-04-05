// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"


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
void displayProgressBarWithSize(std::atomic<size_t>* completedBytes, size_t totalBytes, 
                                std::atomic<size_t>* completedTasks, std::atomic<size_t>* failedTasks, 
                                size_t totalTasks, std::atomic<bool>* isComplete, bool* verbose,  
                                const std::string& operation) {
    // Set up terminal for non-blocking input
    disableInputForProgressBar(&oldt, &oldf);

    const ListTheme* theme = getActiveTheme();
    const bool isOrig = (globalTheme == "original");

    int processingBarWidth = 42;
    int finalBarWidth = 30;
    
    // Width logic remains the same
    if (operation.find("mount") != std::string::npos || operation.find("umount") != std::string::npos) {
        processingBarWidth = 46;
    } else if (operation.find("MDF") != std::string::npos || operation.find("NRG") != std::string::npos || operation.find("BIN/IMG") != std::string::npos) {
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
        while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
        ss << std::fixed << std::setprecision(2) << size << units[unit];
        return ss.str();
    };
    
    std::string totalBytesFormatted = bytesTrackingEnabled ? formatSize(static_cast<double>(totalBytes)) : "";
    
    // --- Themed Render Function ---
    auto renderProgressBar = [&](bool isFinal = false) -> std::string {
        const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
        const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
        const size_t completedBytesValue = bytesTrackingEnabled ? completedBytes->load(std::memory_order_acquire) : 0;
        
        double tasksProgress = static_cast<double>(completedTasksValue + failedTasksValue) / totalTasks;
        double overallProgress = isFinal ? 1.0 : tasksProgress;
        if (bytesTrackingEnabled && !isFinal) {
            double bytesProgress = static_cast<double>(completedBytesValue) / totalBytes;
            overallProgress = std::max(bytesProgress, tasksProgress);
        }
        
        int barWidth = isFinal ? finalBarWidth : processingBarWidth;
        int progressPos = static_cast<int>(barWidth * overallProgress);
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        double elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count() / 1000.0;
        double speed = (bytesTrackingEnabled && elapsedSeconds > 0.0) ? (static_cast<double>(completedBytesValue) / elapsedSeconds) : 0.0;
        
        std::stringstream ss;
        std::string_view accentCol = isOrig ? "\033[1;92m" : theme->accent;
        std::string_view mutedCol  = isOrig ? "\033[1;94m" : theme->muted;
        std::string_view dataCol   = "\033[1;97m";
        
        // Line 1
        ss << "\r\033[2K" << (isOrig ? "\033[0;1m" : theme->muted) << "[";
        for (int i = 0; i < barWidth; ++i) {
            if (i < progressPos) ss << accentCol << "=";
            else if (i == progressPos && !isFinal) ss << accentCol << ">";
            else ss << " ";
        }
        ss << (isOrig ? "\033[0;1m" : theme->muted) << "] " 
           << accentCol << std::fixed << std::setprecision(0) << (overallProgress * 100.0) << "%"
           << "\033[0m (" << dataCol << completedTasksValue << "/" << totalTasks << "\033[0m) "
           << mutedCol << "Time: " << dataCol << std::fixed << std::setprecision(1) << elapsedSeconds << "s\033[0m";
        
        if (bytesTrackingEnabled) {
            int percentPos = barWidth + 3;
            ss << "\n\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            ss << mutedCol << "Processed: " << dataCol << formatSize(static_cast<double>(completedBytesValue)) << "/" << totalBytesFormatted;
            
            ss << "\n\r\033[2K";
            for (int i = 0; i < percentPos; i++) ss << " ";
            ss << mutedCol << "Speed:     " << dataCol << formatSize(speed) << "/s\033[0m";
        }
        return ss.str();
    };
    
    // Update loop
    while (!isComplete->load(std::memory_order_acquire) || !enterPressed) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0);
        
        std::cout << renderProgressBar() << std::flush;
        
        if (bytesTrackingEnabled && !isComplete->load(std::memory_order_acquire)) {
            std::cout << "\033[2A"; // Move up for 3-line bar
        }
        
        if (isComplete->load(std::memory_order_acquire) && !enterPressed) {
            signal(SIGINT, SIG_IGN);
            
            // Clear current bar to write status
            std::cout << (bytesTrackingEnabled ? "\033[1J\033[3A" : "\033[1J\033[1A");
            
            const size_t completedTasksValue = completedTasks->load(std::memory_order_acquire);
            const size_t failedTasksValue = failedTasks->load(std::memory_order_acquire);
            
            // --- Themed Status Output ---
            std::string statusStr;
            if (g_operationCancelled.load()) {
                statusStr = std::string(isOrig ? "\033[1;33m" : theme->warning) + "INTERRUPTED";
            } else if (failedTasksValue > 0) {
                statusStr = (completedTasksValue > 0) ? 
                    std::string(isOrig ? "\033[1;93m" : theme->warning) + "PARTIAL" : 
                    std::string(isOrig ? "\033[1;91m" : theme->secondary) + "FAILED";
            } else {
                statusStr = std::string(isOrig ? "\033[1;92m" : theme->accent) + "COMPLETED";
            }

            std::cout << "\r\033[2K\033[0;1m Status: " << operation << "\033[0;1m → " 
                      << statusStr << "\033[0;1m" << std::endl;
            
            std::cout << renderProgressBar(true) << "\n\n";
            
            disableReadlineForConfirmation();
            enterPressed = true;
            restoreInput(&oldt, oldf);

            // --- Themed Question ---
            const std::string prompt = "\001" + std::string(isOrig ? "\033[1;94m" : theme->muted) + 
                                       "\002Display verbose output? (y/n):\001\033[0;1m\002 ";
            
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
