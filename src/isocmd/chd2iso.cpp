
#include "../headers.h"
#include <sys/wait.h>

/**
 * @brief Converts an ISO to CHD using libchdr.
 * Uses a hunk-based approach for compression.
 */
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <atomic>
bool convertIsoToChd(const std::string& inputPath, const std::string& outputPath) {
    int pipefds[2];
    if (pipe(pipefds) == -1) return false;

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefds[0]);
        close(pipefds[1]);
        return false;
    }

    if (pid == 0) { // Child
        close(pipefds[0]);
        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        close(pipefds[1]);

        // Fix: "cdzl" must be a string literal. 
        // Using const_cast is the "standard" way to pass strings to execvp.
        char* args[] = {
            (char*)"chdman", (char*)"createcd", 
            (char*)"-i", const_cast<char*>(inputPath.c_str()), 
            (char*)"-o", const_cast<char*>(outputPath.c_str()), 
            (char*)"-c", (char*)"cdzl", nullptr
        };
        
        execvp("chdman", args);
        _exit(1); 
    }

    // Parent
    close(pipefds[1]);
    
    // Set non-blocking
    int flags = fcntl(pipefds[0], F_GETFL, 0);
    fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);

    char buffer[4096]; 
    std::string lineAccumulator;
    bool cancelled = false;

    while (true) {
        if (g_operationCancelled.load() && !cancelled) {
            kill(pid, SIGTERM);
            cancelled = true;
        }

        ssize_t bytesRead = read(pipefds[0], buffer, sizeof(buffer));

        if (bytesRead > 0) {
            if (!cancelled) {
                lineAccumulator.append(buffer, bytesRead);
                
                // Process full lines only to keep output clean
                size_t pos;
                while ((pos = lineAccumulator.find_first_of("\n\r")) != std::string::npos) {
                    std::string line = lineAccumulator.substr(0, pos);
                    lineAccumulator.erase(0, pos + 1);

                    if (!line.empty()) {
                        // Filter: Only show the line if it's NOT a progress update
                        // OR if it's the final 100% mark.
                        if (line.find('%') == std::string::npos || line.find("100.0%") != std::string::npos) {
                            std::cout << "[chdman] " << line << std::endl;
                        }
                    }
                }
            }
        } else if (bytesRead == 0) {
            break; 
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; 
        }

        usleep(10000); 
    }

    close(pipefds[0]);
    int status;
    waitpid(pid, &status, 0);

    return !cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
