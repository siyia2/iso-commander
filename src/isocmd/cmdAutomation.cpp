// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

/**
 * @brief Primary entry point for handling mount/umount CLI commands.
 * * This function parses command-line arguments to facilitate batch mounting 
 * or unmounting of ISO files. It supports recursion depth limits, 
 * silent mode operation, and handles root privilege verification.
 * * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return int Status code (0 for success, 1 for error).
 */
int handleMountUmountCommands(int argc, char* argv[]) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    if (argc < 2) {
        std::cerr << originalColors::red << "Error: No arguments provided." << originalColors::resetPlain << "\n";
        return 1;
    }

    bool silentMode = false;
    int maxDepth = -1; 
    std::vector<std::string> args;
    args.reserve(argc);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--silent") {
            silentMode = true;
        } else if (arg.substr(0, 2) == "-d") {
            try {
                maxDepth = std::stoi(arg.substr(2));
                if (maxDepth < 0) {
                    std::cerr << originalColors::yellow << "Warning: Negative depth (" << maxDepth
                              << ") means a full recursive scan." << originalColors::resetPlain << "\n";
                    maxDepth = -1;
                }
            } catch (...) {
                std::cerr << originalColors::yellow << "Warning: Invalid depth '" << arg.substr(2)
                          << "'. Using 0 (surface scan)." << originalColors::resetPlain << "\n";
                maxDepth = 0;
            }
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        std::cerr << originalColors::red << "Error: No action provided." << originalColors::resetPlain << "\n";
        return 1;
    }

    std::string action = args.back();

    if (action == "mount") {
        if (geteuid() != 0) {
            std::cerr << originalColors::red << "Error: Root privileges required for mounting ISOs." << originalColors::resetPlain << "\n";
            return 1;
        }

        std::unordered_set<std::string> isoFiles;
        bool hasErrors = false;

        for (size_t i = 0; i < args.size() - 1; ++i) {
            if (g_operationCancelled.load()) {
                if (!silentMode) std::cout << originalColors::yellow << "\nOperation cancelled by user." << originalColors::resetPlain << "\n";
                return 1;
            }

            fs::path path(args[i]);
            std::string originalPath = path.string();

            try {
                if (!fs::exists(path)) {
                    if (!silentMode)
                        std::cerr << originalColors::yellow << "Warning: '" << originalColors::red << originalPath
                                  << originalColors::yellow << "' does not exist, skipping." << originalColors::resetPlain << "\n";
                    hasErrors = true;
                    continue;
                }

                if (fs::is_regular_file(path)) {
                    std::string ext = path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".iso") {
                        if (!silentMode)
                            std::cerr << originalColors::yellow << "Warning: '" << originalColors::red << originalPath
                                      << originalColors::yellow << "' is not an ISO file, skipping." << originalColors::resetPlain << "\n";
                        hasErrors = true;
                        continue;
                    }
                    isoFiles.insert(fs::canonical(path).string());
                } else if (fs::is_directory(path)) {
                    disableInput();
                    if (!silentMode) {
                        std::cout << "Scanning directory " << path << " ("<< (maxDepth == 0 ? "surface scan" : "max depth: " + (maxDepth < 0 ? "unlimited" : std::to_string(maxDepth))) << ")...\n";
                    }

                    std::function<void(const fs::path&, int)> scanDir;
                    scanDir = [&](const fs::path& dir, int currentDepth) {
                        try {
                            for (const auto& entry : fs::directory_iterator(dir)) {
                                if (g_operationCancelled.load()) return;
                                if (entry.is_symlink()) continue; 

                                if (entry.is_regular_file()) {
                                    std::string ext = entry.path().extension().string();
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                    if (ext == ".iso") {
                                        isoFiles.insert(fs::canonical(entry.path()).string());
                                    }
                                }
                                else if (entry.is_directory() && (maxDepth == -1 || currentDepth < maxDepth)) {
                                    scanDir(entry.path(), currentDepth + 1);
                                }
                            }
                        } catch (const fs::filesystem_error& e) {
                            if (!silentMode)
                                std::cerr << originalColors::yellow << "Warning: Error scanning directory '" << originalColors::red << dir.string()
                                          << originalColors::yellow << "': " << e.what() << originalColors::resetPlain << "\n";
                            hasErrors = true;
                        }
                    };
                    
                    scanDir(path, 0);
                } else {
                    if (!silentMode)
                        std::cerr << originalColors::yellow << "Warning: '" << originalColors::red << originalPath
                                  << originalColors::yellow << "' is not a valid file or directory, skipping." << originalColors::resetPlain << "\n";
                    hasErrors = true;
                }
            } catch (const fs::filesystem_error& e) {
                if (!silentMode)
                    std::cerr << originalColors::yellow << "Warning: Error processing '" << originalColors::red << originalPath
                              << originalColors::yellow << "': " << e.what() << originalColors::resetPlain << "\n";
                hasErrors = true;
            }
        }
        
        if (!silentMode && g_operationCancelled.load()) std::cout << originalColors::yellow << "Mount Operation cancelled by user." << originalColors::resetPlain << "\n";
        
        if (isoFiles.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "\n" << originalColors::yellow << "No ISO files found to mount." << originalColors::resetPlain << "\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) {
            std::cout << "\nLocated " << isoFiles.size() << " ISO file" << (isoFiles.size() == 1 ? "" : "s") << "; Attempting to mount...\n";
        }

        std::unordered_set<std::string> mountedFiles;
        std::unordered_set<std::string> skippedMessages;
        std::unordered_set<std::string> mountedFails;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        mountIsoFiles(std::vector<std::string>(isoFiles.begin(), isoFiles.end()),
                      mountedFiles, skippedMessages, mountedFails,
                      &completedTasks, &failedTasks, silentMode);

        if (!silentMode) {
            for (const auto& msg : mountedFiles) std::cout << msg << "\n";
            for (const auto& msg : skippedMessages) std::cout << msg << "\n";
            for (const auto& msg : mountedFails) std::cout << msg << "\n";
            std::cout << "\nMount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << originalColors::resetPlain << "\n" ;
        }

        return (completedTasks.load() > 0 || (!hasErrors && isoFiles.empty())) ? 0 : 1;
    }

    else if (action == "umount" || action == "unmount") {
        std::unordered_set<std::string> mountPointsToUnmount; 
        bool hasErrors = false;
        
        if (geteuid() != 0) {
            std::cerr << originalColors::red << "Error: Root privileges required for unmounting ISOs." << originalColors::resetPlain << "\n";
            return 1;
        }

        if (args.size() <= 1 || (args.size() == 2 && (args[0] == "all"))) {
            disableInput();
            if (!silentMode) std::cout << "Scanning /mnt for ISO mount points (surface scan)...\n";
            try {
                for (const auto& entry : fs::directory_iterator("/mnt")) {
                    if (g_operationCancelled.load()) {
                        if (!silentMode) std::cout << originalColors::yellow << "\nOperation cancelled by user." << originalColors::resetPlain << "\n";
                        return 1;
                    }
                    if (entry.is_directory()) {
                        std::string dirName = entry.path().filename().string();
                        if (dirName.substr(0, 4) == "iso_") {
                            mountPointsToUnmount.insert(fs::canonical(entry.path()).string());
                        }
                    }
                }
            } catch (const fs::filesystem_error& e) {
                std::cerr << originalColors::red << "Error scanning /mnt: " << e.what() << originalColors::resetPlain << "\n";
                return 1;
            }
        } else {
            for (size_t i = 0; i < args.size() - 1; ++i) {
                if (g_operationCancelled.load()) {
                    if (!silentMode) std::cout << originalColors::yellow << "\nOperation cancelled by user." << originalColors::resetPlain << "\n";
                    return 1;
                }

                fs::path path(args[i]);
                std::string originalPath = path.string();

                try {
                    if (fs::exists(path) && fs::is_directory(path)) {
                        auto canonicalPath = fs::canonical(path);
                        std::string canonicalStr = canonicalPath.string();

                        if (canonicalStr == "/mnt") {
                            disableInput();
                            if (!silentMode) std::cout << "Scanning /mnt for ISO mount points (surface scan)...\n";
                            for (const auto& entry : fs::directory_iterator(canonicalPath)) {
                                if (entry.is_directory()) {
                                    std::string entryDirName = entry.path().filename().string();
                                    if (entryDirName.rfind("iso_", 0) == 0) {
                                        mountPointsToUnmount.insert(fs::canonical(entry.path()).string());
                                    }
                                }
                            }
                        } else if (canonicalStr.rfind("/mnt/iso_", 0) == 0) {
                            mountPointsToUnmount.insert(canonicalStr);
                        } else {
                            if (!silentMode)
                                std::cerr << originalColors::yellow << "Warning: Directory parameter '" << originalColors::red << originalPath
                                          << originalColors::yellow << "' is not allowed. Only " << originalColors::blue << "/mnt" << originalColors::yellow 
                                          << " or " << originalColors::blue << "/mnt/iso_*" << originalColors::yellow << " allowed." << originalColors::resetPlain << "\n";
                            hasErrors = true;
                        }
                    } else {
                        fs::path candidatePath = path;
                        if (path.is_relative()) {
                            candidatePath = fs::path("/mnt") / ("iso_" + path.filename().string());
                        }

                        if (fs::exists(candidatePath) && fs::is_directory(candidatePath)) {
                            mountPointsToUnmount.insert(fs::canonical(candidatePath).string());
                        } else {
                            if (!silentMode)
                                std::cerr << originalColors::yellow << "Warning: Mount point '" << originalColors::red << originalPath
                                          << originalColors::yellow << "' does not exist or is invalid, skipping." << originalColors::resetPlain << "\n";
                            hasErrors = true;
                        }
                    }
                } catch (const fs::filesystem_error& e) {
                    if (!silentMode)
                        std::cerr << originalColors::yellow << "Warning: Error processing '" << originalColors::red << originalPath
                                  << originalColors::yellow << "': " << e.what() << originalColors::resetPlain << "\n";
                    hasErrors = true;
                }
            }
        }
        
        if (!silentMode && g_operationCancelled.load()) std::cout << originalColors::yellow << "Umount Operation cancelled by user." << originalColors::resetPlain << "\n";
        
        if (mountPointsToUnmount.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "\n" << originalColors::yellow << "No ISO mount points found to unmount." << originalColors::resetPlain << "\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) std::cout << "\nLocated " << mountPointsToUnmount.size() << " mount point" << (mountPointsToUnmount.size() == 1 ? "" : "s") << "; Attempting to unmount...\n";
        std::unordered_set<std::string> unmountedFiles;
        std::unordered_set<std::string> unmountedErrors;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        unmountISO(std::vector<std::string>(mountPointsToUnmount.begin(), mountPointsToUnmount.end()),
                   unmountedFiles, unmountedErrors,
                   &completedTasks, &failedTasks, silentMode);

        if (!silentMode) {
            for (const auto& msg : unmountedFiles) std::cout << msg << "\n";
            for (const auto& msg : unmountedErrors) std::cout << msg << "\n";
            std::cout << "\nUnmount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << originalColors::resetPlain << "\n";
        }

        return (failedTasks.load() == 0 && !hasErrors) || completedTasks.load() > 0 ? 0 : 1;
    }

    std::cerr << originalColors::red << "Error: Unknown action '" << action << "'" << originalColors::resetPlain << "\n";
    return 1;
}
