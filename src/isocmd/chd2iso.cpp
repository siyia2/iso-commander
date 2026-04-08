
#include "../headers.h"

/**
 * @brief Converts an ISO to CHD using libchdr.
 * Uses a hunk-based approach for compression.
 */
bool convertIsoToChd(const std::string& inputPath, const std::string& outputPath) {
    std::string command = "chdman createcd -i \"" + inputPath + "\" -o \"" + outputPath + "\" -c cdzl";
    int ret = std::system(command.c_str());
    // Check cancellation flag *after* system returns (or use a signal handler to kill the child)
    if (g_operationCancelled.load()) {
        return false;
    }
    return (ret == 0);
}
