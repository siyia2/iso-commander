
#include "../headers.h"
#include <sys/wait.h>

/**
 * @brief Converts an ISO to CHD using libchdr.
 * Uses a hunk-based approach for compression.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <string>
#include <atomic>

bool convertIsoToChd(const std::string& inputPath, const std::string& outputPath) {
    int pipefds[2];
    if (pipe(pipefds) == -1) return false;

    pid_t pid = fork();

    if (pid == 0) { // Child
        close(pipefds[0]);
        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        char* args[] = {(char*)"chdman", (char*)"createcd", (char*)"-i", (char*)inputPath.c_str(), 
                        (char*)"-o", (char*)outputPath.c_str(), (char*)"-c", (char*)"cdzl", nullptr};
        execvp("chdman", args);
        exit(1);
    }

    // Parent
    close(pipefds[1]);
    std::string line;
    char buffer[1];
    bool cancelled = false;

    // Set non-blocking to keep the loop responsive to g_operationCancelled
    fcntl(pipefds[0], F_SETFL, O_NONBLOCK);

    while (true) {
        if (g_operationCancelled.load()) {
            kill(pid, SIGTERM); 
            cancelled = true;
            // No break yet; we want to drain the pipe and wait for the process to die
        }

        ssize_t bytesRead = read(pipefds[0], buffer, 1);
        if (bytesRead > 0) {
            if (buffer[0] == '\n' || buffer[0] == '\r') {
                if (!line.empty() && !cancelled) { // Don't print if we've cancelled
                    if (line.find("%") == std::string::npos || line.find("100.0%") != std::string::npos) {
                        std::cout << "[chdman] " << line << std::endl;
                    }
                }
                line.clear();
            } else {
                line += buffer[0];
            }
        } else if (bytesRead == 0) {
            break; // Pipe closed
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; 
        }
        usleep(1000); 
    }

    int status;
    waitpid(pid, &status, 0);
    close(pipefds[0]);

    // WIFEXITED checks if it finished normally, WEXITSTATUS checks if it returned 0
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
