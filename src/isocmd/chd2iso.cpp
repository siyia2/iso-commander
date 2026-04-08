
#include "../headers.h"

/**
 * @brief Converts an ISO to CHD using libchdr.
 * Uses a hunk-based approach for compression.
 */
bool convertIsoToChd(const std::string& inputPath, const std::string& outputPath) {
    // Build command – stderr is merged into stdout so we see everything
    std::string command = "chdman createcd -i \"" + inputPath + "\" -o \"" + outputPath + "\" -c cdzl 2>&1";

    // Use popen to read output while still letting us monitor cancellation
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return false;
    }

    // Read and print every line so chdman's progress appears in the terminal
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        // Check for cancellation request
        if (g_operationCancelled.load()) {
            // Optionally kill the child process here if possible, but returning false is enough
            return false;
        }
        // Print the line exactly as chdman produced it
        std::cout << buffer;
    }

    // Check exit status
    int status = pclose(pipe.release());
    if (status == -1) {
        return false;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
