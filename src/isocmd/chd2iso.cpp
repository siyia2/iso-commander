
#include "../headers.h"

/**
 * @brief Converts an ISO to CHD using libchdr.
 * Uses a hunk-based approach for compression.
 */
bool convertIsoToChd(const std::string& inputPath, const std::string& outputPath, std::atomic<size_t>* completedBytes) {
    // 1. Prepare the command
    std::string command = "chdman createcd -i \"" + inputPath + "\" -o \"" + outputPath + "\" -c cdzl 2>&1";

    // 2. Open the pipe to capture chdman's output
    // FIX: Using an explicit function pointer type to silence the [-Wignored-attributes] warning
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    
    if (!pipe) return false;

    // 3. Monitor output
    std::error_code ec;
    uint64_t totalSize = std::filesystem::file_size(inputPath, ec);
    if (ec) totalSize = 0;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        // Handle Cancellation
        if (g_operationCancelled.load()) {
            // pkill is a bit "nuclear"; pclose handles the pipe, 
            // but the subprocess might need a SIGTERM.
            std::string killCmd = "pkill -f \"chdman createcd.*" + outputPath + "\"";
            system(killCmd.c_str());
            return false;
        }
        
        // 4. (Optional) Simple Progress Parsing
        // chdman lines look like: "Total progress:  15.2%"
        std::string line(buffer);
        size_t pos = line.find("Total progress:");
        if (pos != std::string::npos && totalSize > 0 && completedBytes) {
            try {
                // Find the first digit after "Total progress:"
                size_t start = line.find_first_of("0123456789", pos);
                if (start != std::string::npos) {
                    double percent = std::stod(line.substr(start));
                    completedBytes->store(static_cast<size_t>((percent / 100.0) * totalSize));
                }
            } catch (...) { /* Ignore parsing errors */ }
        }
    }

    // 5. Verify Success
    if (std::filesystem::exists(outputPath) && std::filesystem::file_size(outputPath) > 0) {
        if (completedBytes && totalSize > 0) {
            completedBytes->store(totalSize); 
        }
        return true;
    }

    return false;
}
