
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
    std::cout << "Converting: " << inputPath << " -> " << outputPath << std::endl;

    int pipefds[2];
    if (pipe(pipefds) == -1) return false;

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefds[0]);
        close(pipefds[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefds[0]);
        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        close(pipefds[1]);

        char* args[] = {
            (char*)"chdman", (char*)"createcd", 
            (char*)"-i", const_cast<char*>(inputPath.c_str()), 
            (char*)"-o", const_cast<char*>(outputPath.c_str()), 
            (char*)"-c", (char*)"cdzl", nullptr
        };
        
        execvp("chdman", args);
        _exit(1);
    }

    close(pipefds[1]);
    
    int flags = fcntl(pipefds[0], F_GETFL, 0);
    fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);

    char buffer[4096];
    std::string lineAccumulator;
    bool cancelled = false;

    // Only print lines that contain "100.0%"
    auto verboseFilter = [](const std::string& line) -> bool {
        return line.find("100.0%") != std::string::npos;
    };

    while (true) {
        if (g_operationCancelled.load() && !cancelled) {
            kill(pid, SIGTERM);
            cancelled = true;
        }

        ssize_t bytesRead = read(pipefds[0], buffer, sizeof(buffer));

        if (bytesRead > 0) {
            if (!cancelled) {
                lineAccumulator.append(buffer, bytesRead);
                
                size_t pos;
                while ((pos = lineAccumulator.find_first_of("\n\r")) != std::string::npos) {
                    std::string line = lineAccumulator.substr(0, pos);
                    lineAccumulator.erase(0, pos + 1);

                    if (!line.empty() && verboseFilter(line)) {
                        std::cout << "[chdman] " << line << std::endl;
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
